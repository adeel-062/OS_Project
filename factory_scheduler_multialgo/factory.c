/*
 * factory.c — Multithreaded Car Parts Factory Scheduler
 * ======================================================
 *
 * Thread roles
 * ─────────────
 *  Dispatcher thread  : drives the simulated clock; admits new parts onto
 *                       the conveyor belt each tick; applies aging.
 *
 *  Workstation threads: one thread per priority level (4 total).
 *                       Each waits on its taskReady semaphore, picks up a
 *                       part, works tick-by-tick, and either finishes the
 *                       part or requeues it (MLFQ demotion).
 *
 *  Logger thread      : reads completed PIDs from the IPC pipe written by
 *                       workstation threads and records final stats.
 *
 * Synchronisation
 * ────────────────
 *  g_mutex          — mutual exclusion for all shared data
 *  g_printMutex     — prevents interleaved console output
 *  g_taskReady[p]   — counting semaphore; posted when a part enters queue p
 *  g_beltSlots      — counting semaphore; limits parts on the belt
 *  g_pipe           — POSIX unnamed pipe for IPC completion events
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include "factory.h"

/* ══════════════════════════════════════════════════════════
 *  Global variable definitions
 * ══════════════════════════════════════════════════════════ */

Process     *g_processes    = NULL;
int          g_processCount = 0;
Queue        g_queues[PRIORITY_LEVELS];
int          g_tick         = 0;
int          g_completed    = 0;
int          g_timeSlice    = DEFAULT_BASE_QUANTUM;  /* dynamic base quantum */
int          g_agingLimit   = MIN_AGING_LIMIT;          /* dynamic aging threshold */
int          g_contextSwitches = 0;
volatile int g_shutdown     = 0;

pthread_mutex_t g_mutex      = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t g_printMutex = PTHREAD_MUTEX_INITIALIZER;
sem_t           g_taskReady[PRIORITY_LEVELS];
sem_t           g_beltSlots;

int g_pipe[2];

int g_ganttPid  [GANTT_MAX];
int g_ganttStage[GANTT_MAX];
int g_ganttTick [GANTT_MAX];
int g_ganttSize = 0;

pthread_t g_wsThreads[PRIORITY_LEVELS];
pthread_t g_dispThread;
pthread_t g_logThread;

/* ══════════════════════════════════════════════════════════
 *  Internal helpers
 * ══════════════════════════════════════════════════════════ */

/* Thread-safe printf — serialises output with g_printMutex. */
static void safePrintf(const char *fmt, ...) {
    va_list ap;
    pthread_mutex_lock(&g_printMutex);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    pthread_mutex_unlock(&g_printMutex);
}

/*
 * factoryReadyCountLocked — count parts currently waiting on the belt.
 * Caller must already hold g_mutex.
 */
int factoryReadyCountLocked(void) {
    int count = 0;
    for (int i = 0; i < g_processCount; i++) {
        if (g_processes[i].state == READY)
            count++;
    }
    return count;
}

/*
 * factoryRecalculatePolicyLocked — dynamic scheduling policy.
 * Caller must already hold g_mutex.
 *
 * 1. Collect remaining burst times of all READY parts.
 * 2. Calculate base quantum using median remaining burst time.
 * 3. Enforce context-switch rule: q must be at least 10 × cs.
 * 4. Decide aging time from base quantum and current queue load.
 */
void factoryRecalculatePolicyLocked(void) {
    int bursts[MAX_PROCESSES];
    int readyCount = 0;

    for (int i = 0; i < g_processCount && readyCount < MAX_PROCESSES; i++) {
        if (g_processes[i].state == READY) {
            bursts[readyCount++] = g_processes[i].remainingTime;
        }
    }

    g_timeSlice  = computeDynamicBaseQuantumFromBursts(bursts, readyCount, g_timeSlice);
    g_agingLimit = decideAgingLimit(g_timeSlice, readyCount);
}

static void reportPolicyChange(const char *reason,
                               int oldQ, int newQ,
                               int oldAge, int newAge,
                               int readyCount) {
    if (oldQ != newQ || oldAge != newAge) {
        safePrintf("[Policy]  %s: ready=%d, base quantum=%d tick(s), "
                   "aging limit=%d tick(s), min q from cs=%d tick(s)\n",
                   reason, readyCount, newQ, newAge,
                   minQuantumTicksFromContextSwitch());
    }
}

/*
 * recordGanttEntryLocked — append a visible Gantt event.
 * Caller must already hold g_mutex.
 *
 * pid >= 0                    → actual part/work tick
 * pid == GANTT_CONTEXT_SWITCH → context-switch marker shown as "CS"
 *
 * Context-switch entries are markers. They make switching overhead visible
 * in the final chart without pretending that a car part was worked on.
 */
static void recordGanttEntryLocked(int pid, int stage, int tick) {
    if (g_ganttSize < GANTT_MAX) {
        g_ganttPid  [g_ganttSize] = pid;
        g_ganttStage[g_ganttSize] = stage;
        g_ganttTick [g_ganttSize] = tick;
        g_ganttSize++;
    }
}

/*
 * beltEnqueue — place a part on the conveyor belt and into its priority queue.
 *
 * Protocol (producer side of producer-consumer pattern):
 *   1. sem_wait(beltSlots)   — claim one belt slot (blocks if belt is full)
 *   2. lock(g_mutex)         — exclusive access to the queue
 *   3. enqueue + set READY
 *   4. unlock(g_mutex)
 *   5. sem_post(taskReady)   — signal the target workstation
 *
 * The workstation thread (consumer) does the symmetric release:
 *   sem_wait(taskReady) → dequeue under lock → sem_post(beltSlots)
 */
static void beltEnqueue(int pid) {
    /* ── Step 1: claim a belt slot (may block if belt full) ── */
    sem_wait(&g_beltSlots);

    int oldQ, newQ, oldAge, newAge, readyCount;

    /* ── Steps 2-4: enqueue under mutex ────────────────────── */
    pthread_mutex_lock(&g_mutex);
    int pr = g_processes[pid].priority;
    enqueue(&g_queues[pr], pid);
    g_processes[pid].state         = READY;
    g_processes[pid].readyWaitTime = 0;

    oldQ = g_timeSlice;
    oldAge = g_agingLimit;
    factoryRecalculatePolicyLocked();
    newQ = g_timeSlice;
    newAge = g_agingLimit;
    readyCount = factoryReadyCountLocked();

    pthread_mutex_unlock(&g_mutex);

    /* ── Step 5: wake the workstation for this priority ─────── */
    sem_post(&g_taskReady[pr]);

    safePrintf("  [Belt] P%d placed on conveyor → %s queue\n",
               pid, STAGE_NAMES[pr]);
    reportPolicyChange("dynamic quantum/aging recalculated after belt enqueue",
                       oldQ, newQ, oldAge, newAge, readyCount);
}

/* ══════════════════════════════════════════════════════════
 *  Dispatcher thread
 *  ─────────────────
 *  Drives the simulated clock. Each iteration = one tick.
 *  Responsibilities:
 *    • Admit newly arrived parts onto the belt
 *    • Increment readyWaitTime and apply aging
 *    • Print a queue snapshot
 *    • Signal shutdown when all parts are done
 * ══════════════════════════════════════════════════════════ */
static void *dispatcherFn(void *arg) {
    (void)arg;
    safePrintf("[Dispatcher] Factory dispatcher started.\n\n");

    while (1) {
        usleep(TICK_MS * 1000);   /* advance one simulated tick */

        /* Read and increment global tick under lock */
        pthread_mutex_lock(&g_mutex);
        int tick  = g_tick++;
        int done  = g_completed;
        int total = g_processCount;
        pthread_mutex_unlock(&g_mutex);

        if (done >= total) break;

        safePrintf("\n╔══ Tick %-3d ══════════════════════════════╗\n", tick);

        /* ── Admit arriving parts ─────────────────────────── */
        for (int i = 0; i < g_processCount; i++) {
            pthread_mutex_lock(&g_mutex);
            int arrives = (g_processes[i].arrivalTime == tick &&
                           g_processes[i].state == NEW);
            pthread_mutex_unlock(&g_mutex);

            if (arrives) {
                safePrintf("[Dispatcher] P%d arrived at tick %d — "
                           "entering factory as %s task\n",
                           i, tick, STAGE_NAMES[g_processes[i].priority]);
                beltEnqueue(i);
            }
        }

        /* ── Aging ────────────────────────────────────────── */
        /*
         * Aging prevents starvation: if a READY part waits too long it is
         * promoted to the next priority queue (higher-urgency workstation).
         *
         * We collect age events under g_mutex, rebuild queues, then print
         * outside the lock to avoid holding g_mutex and g_printMutex together.
         */
        typedef struct { int pid; int oldPr; int newPr; } AgeEvt;
        AgeEvt ageEvts[MAX_PROCESSES];
        int    ageCount = 0;

        pthread_mutex_lock(&g_mutex);
        for (int i = 0; i < g_processCount; i++) {
            if (g_processes[i].state == READY) {
                g_processes[i].readyWaitTime++;
                if (g_processes[i].readyWaitTime >= g_agingLimit &&
                    g_processes[i].priority < PRIORITY_LEVELS - 1) {

                    ageEvts[ageCount++] = (AgeEvt){
                        i,
                        g_processes[i].priority,
                        g_processes[i].priority + 1
                    };
                    g_processes[i].priority++;
                    g_processes[i].readyWaitTime = 0;
                }
            }
        }

        int oldQ = g_timeSlice;
        int oldAge = g_agingLimit;
        int readyCountAfterAging = factoryReadyCountLocked();

        if (ageCount > 0) {
            /*
             * Rebuild all queues from the READY process list at their
             * new priorities.  Belt slot count is unchanged (same number
             * of parts on the belt, just re-sorted).
             */
            for (int p = 0; p < PRIORITY_LEVELS; p++)
                initQueue(&g_queues[p]);

            for (int i = 0; i < g_processCount; i++) {
                if (g_processes[i].state == READY) {
                    int pr = g_processes[i].priority;
                    enqueue(&g_queues[pr], i);
                    /* Wake whichever workstation now owns this part */
                    sem_post(&g_taskReady[pr]);
                }
            }

            factoryRecalculatePolicyLocked();
            readyCountAfterAging = factoryReadyCountLocked();
        }

        int newQ = g_timeSlice;
        int newAge = g_agingLimit;
        pthread_mutex_unlock(&g_mutex);

        /* Print aging events (outside lock) */
        for (int k = 0; k < ageCount; k++) {
            safePrintf("[Aging]  P%d promoted: %s (Q%d) → %s (Q%d)\n",
                       ageEvts[k].pid,
                       STAGE_NAMES[ageEvts[k].oldPr], ageEvts[k].oldPr,
                       STAGE_NAMES[ageEvts[k].newPr], ageEvts[k].newPr);
        }

        if (ageCount > 0) {
            reportPolicyChange("dynamic quantum/aging recalculated after aging",
                               oldQ, newQ, oldAge, newAge, readyCountAfterAging);
        }

        /* ── Queue snapshot ───────────────────────────────── */
        /*
         * Build snapshot string under g_mutex, print after releasing it.
         * This avoids holding two mutexes simultaneously.
         */
        char snapBuf[PRIORITY_LEVELS][512];

        pthread_mutex_lock(&g_mutex);
        for (int p = PRIORITY_LEVELS - 1; p >= 0; p--) {
            int n = snprintf(snapBuf[p], 512, "  Q%d [%-9s | %-24s]: ", p, STAGE_NAMES[p], QUEUE_ALGO_NAMES[p]);
            if (isEmpty(&g_queues[p])) {
                snprintf(snapBuf[p] + n, 512 - n, "(empty)");
            } else {
                for (int j = g_queues[p].front; j <= g_queues[p].rear; j++)
                    n += snprintf(snapBuf[p] + n, 512 - n,
                                  "P%d ", g_queues[p].items[j]);
            }
        }
        pthread_mutex_unlock(&g_mutex);

        safePrintf("[Queue Snapshot]\n");
        for (int p = PRIORITY_LEVELS - 1; p >= 0; p--)
            safePrintf("%s\n", snapBuf[p]);
    }

    safePrintf("\n[Dispatcher] All %d parts completed. Shutting down factory.\n",
               g_processCount);

    /* Signal workstation threads to exit */
    g_shutdown = 1;
    for (int p = 0; p < PRIORITY_LEVELS; p++)
        sem_post(&g_taskReady[p]);

    /* Send sentinel (-1) through IPC pipe to stop the logger */
    int sentinel = -1;
    write(g_pipe[1], &sentinel, sizeof(int));

    return NULL;
}

/* ══════════════════════════════════════════════════════════
 *  Logger thread  (IPC pipe consumer)
 *  ────────────────────────────────────
 *  Reads completed PIDs from the pipe written by workstation threads.
 *  This demonstrates IPC: the shopfloor (workstation threads) reports
 *  completions to the plant manager's log (this thread) via a pipe.
 * ══════════════════════════════════════════════════════════ */
static void *loggerFn(void *arg) {
    (void)arg;
    safePrintf("[Logger]  IPC logger started — listening on pipe.\n");

    int pid;
    while (read(g_pipe[0], &pid, sizeof(int)) == sizeof(int)) {
        if (pid == -1) break;   /* sentinel from dispatcher */

        pthread_mutex_lock(&g_mutex);
        g_completed++;
        int done  = g_completed;
        int total = g_processCount;
        pthread_mutex_unlock(&g_mutex);

        safePrintf("[Logger]  ★ IPC pipe event: P%d finished. "
                   "(%d / %d parts complete)\n", pid, done, total);
    }

    safePrintf("[Logger]  IPC logger shutting down.\n");
    return NULL;
}

/* ══════════════════════════════════════════════════════════
 *  Workstation thread  (one per priority level)
 *  ─────────────────────────────────────────────
 *  Each thread represents one manufacturing station.
 *  It blocks on its taskReady semaphore until the dispatcher or another
 *  workstation places a part in its queue, then processes it tick-by-tick.
 *
 *  MLFQ demotion: when a part's time quantum expires the part is placed
 *  on the belt again at the next lower priority (handed off to the station
 *  below).  This prevents CPU-intensive tasks from monopolising high-priority
 *  stations, just like in a real factory.
 * ══════════════════════════════════════════════════════════ */
typedef struct { int prio; } WsArg;

static void *workstationFn(void *arg) {
    WsArg *ws   = (WsArg *)arg;
    int    prio = ws->prio;
    free(arg);

    safePrintf("[%s] Workstation thread started using %s.\n",
               STAGE_NAMES[prio], QUEUE_ALGO_NAMES[prio]);

    while (1) {
        /*
         * ── Consumer side of producer-consumer (belt semaphore) ──────
         * Block until the dispatcher (or re-queue from demotion) posts
         * g_taskReady[prio].
         */
        sem_wait(&g_taskReady[prio]);

        if (g_shutdown) break;

        /* ── Select/dequeue the part under mutex using this queue's algorithm ── */
        pthread_mutex_lock(&g_mutex);
        int pid = selectProcessFromQueue(g_queues, g_processes, prio);
        if (pid == -1) {
            /* Spurious wake (e.g. after aging rebuild posted extra signal) */
            pthread_mutex_unlock(&g_mutex);
            continue;
        }

        g_processes[pid].state = RUNNING;
        g_contextSwitches++;

        /* Show the context switch itself in the Gantt chart before work starts. */
        recordGanttEntryLocked(GANTT_CONTEXT_SWITCH, prio, g_tick);

        /* Record response time on first execution */
        if (g_processes[pid].responseTime == -1)
            g_processes[pid].responseTime =
                g_tick - g_processes[pid].arrivalTime;

        int baseQ = g_timeSlice;
        int qForThisStation = quantumForPriority(prio, baseQ);
        int currentAgingLimit = g_agingLimit;
        g_processes[pid].timeSliceRemaining = qForThisStation;
        int burstLeft = g_processes[pid].remainingTime;
        pthread_mutex_unlock(&g_mutex);

        /*
         * Release the belt slot this part was occupying.
         * (Symmetric to the sem_wait in beltEnqueue — the consumer
         *  always posts beltSlots after picking up the item.)
         */
        sem_post(&g_beltSlots);

        safePrintf("[%s] ▶ Picked up P%d using %s  "
                   "(burst left: %d, base q: %d, station q: %d, aging limit: %d)\n",
                   STAGE_NAMES[prio], pid, QUEUE_ALGO_NAMES[prio],
                   burstLeft,
                   baseQ,
                   qForThisStation,
                   currentAgingLimit);

        /* ── Tick-by-tick processing loop ────────────────────────────── */
        int taskDone = 0;
        while (!taskDone) {
            usleep(TICK_MS * 1000);   /* simulate one unit of work */

            /* Update process counters under mutex */
            pthread_mutex_lock(&g_mutex);
            g_processes[pid].remainingTime--;
            g_processes[pid].timeSliceRemaining--;
            int tickNow  = g_tick;
            int rem      = g_processes[pid].remainingTime;
            int sliceRem = g_processes[pid].timeSliceRemaining;

            /* Record actual work tick in Gantt chart */
            recordGanttEntryLocked(pid, prio, tickNow);
            pthread_mutex_unlock(&g_mutex);

            /* ── Case 1: Part finished ───────────────────────────────── */
            if (rem == 0) {
                pthread_mutex_lock(&g_mutex);
                g_processes[pid].state          = TERMINATED;
                g_processes[pid].completionTime = tickNow + 1;
                pthread_mutex_unlock(&g_mutex);

                safePrintf("[%s] ✔ P%d COMPLETED at tick %d.\n",
                           STAGE_NAMES[prio], pid, tickNow + 1);

                /*
                 * IPC: notify the logger thread via pipe.
                 * The logger reads the PID and increments g_completed.
                 * This decouples the workstation from stat tracking.
                 */
                write(g_pipe[1], &pid, sizeof(int));
                taskDone = 1;

            /* ── Case 2: Time-slice quantum expired → MLFQ demotion ──── */
            } else if (sliceRem == 0) {
                /*
                 * The part has used its full time quantum at this station.
                 * Under MLFQ it is demoted to the next lower priority queue
                 * — handed off to the station below, like a car body that
                 * needs more painting time passing back through the system.
                 */
                pthread_mutex_lock(&g_mutex);
                g_processes[pid].state = READY;
                int oldPr = g_processes[pid].priority;
                int newPr = oldPr;
                if (MLFQ_ENABLED && g_processes[pid].priority > 0) {
                    g_processes[pid].priority--;
                    newPr = g_processes[pid].priority;
                }
                pthread_mutex_unlock(&g_mutex);

                safePrintf("[%s] ↓ P%d quantum expired — MLFQ demotion: "
                           "%s (Q%d) → %s (Q%d), requeued on belt\n",
                           STAGE_NAMES[prio], pid,
                           STAGE_NAMES[oldPr], oldPr,
                           STAGE_NAMES[newPr], newPr);

                beltEnqueue(pid);   /* back onto conveyor at lower priority */
                taskDone = 1;
            }
            /* Otherwise: keep working (next iteration of while loop) */
        }
    }

    safePrintf("[%s] Workstation thread shutting down.\n", STAGE_NAMES[prio]);
    return NULL;
}

/* ══════════════════════════════════════════════════════════
 *  Public API
 * ══════════════════════════════════════════════════════════ */

void factoryInit(Process *procs, int count, int timeSlice) {
    g_processes    = procs;
    g_processCount = count;
    g_timeSlice    = enforceContextSwitchGuard(
                         clampInt(timeSlice, MIN_BASE_QUANTUM, MAX_BASE_QUANTUM)
                     );
    g_agingLimit   = decideAgingLimit(g_timeSlice, 0);
    g_contextSwitches = 0;
    g_tick         = 0;
    g_completed    = 0;
    g_shutdown     = 0;
    g_ganttSize    = 0;

    printf("[Config] Requested base quantum: %d tick(s)\n", timeSlice);
    printf("[Config] Context switch time: %d ms, rule: q >= %d × cs, "
           "minimum quantum: %d tick(s)\n",
           CONTEXT_SWITCH_MS, CONTEXT_SWITCH_FACTOR,
           minQuantumTicksFromContextSwitch());
    printf("[Config] Starting base quantum: %d tick(s); initial aging limit: %d tick(s)\n",
           g_timeSlice, g_agingLimit);
    printf("[Config] Queue algorithms: Q3=%s, Q2=%s, Q1=%s, Q0=%s\n",
           QUEUE_ALGO_NAMES[3], QUEUE_ALGO_NAMES[2],
           QUEUE_ALGO_NAMES[1], QUEUE_ALGO_NAMES[0]);

    memset(g_ganttPid,   -1, sizeof(g_ganttPid));
    memset(g_ganttStage, -1, sizeof(g_ganttStage));
    memset(g_ganttTick,  -1, sizeof(g_ganttTick));

    for (int p = 0; p < PRIORITY_LEVELS; p++)
        initQueue(&g_queues[p]);

    /*
     * Semaphore initialisation:
     *   beltSlots   : starts full (BELT_CAPACITY slots available)
     *   taskReady[p]: starts at 0 (no tasks yet; workstations block)
     * The second argument to sem_init is pshared=0 (thread-local, not process-shared).
     */
    sem_init(&g_beltSlots, 0, BELT_CAPACITY);
    for (int p = 0; p < PRIORITY_LEVELS; p++)
        sem_init(&g_taskReady[p], 0, 0);

    /* Create the IPC pipe */
    if (pipe(g_pipe) == -1) {
        perror("pipe");
        exit(EXIT_FAILURE);
    }
}

void factoryStart(void) {
    /* Logger must start first so it is ready before completions arrive */
    pthread_create(&g_logThread, NULL, loggerFn, NULL);

    /* Spawn one workstation thread per priority level */
    for (int p = 0; p < PRIORITY_LEVELS; p++) {
        WsArg *a = malloc(sizeof(WsArg));
        if (!a) { perror("malloc"); exit(EXIT_FAILURE); }
        a->prio  = p;
        pthread_create(&g_wsThreads[p], NULL, workstationFn, a);
    }

    /* Dispatcher last — it starts the clock */
    pthread_create(&g_dispThread, NULL, dispatcherFn, NULL);
}

void factoryWait(void) {
    pthread_join(g_dispThread, NULL);
    for (int p = 0; p < PRIORITY_LEVELS; p++)
        pthread_join(g_wsThreads[p], NULL);
    pthread_join(g_logThread, NULL);
}

void factoryPrintStats(void) {
    printf("\n╔══════════════════════════════════════════════════════════╗\n");
    printf("║              FACTORY PRODUCTION REPORT                  ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    float totalWT = 0, totalTAT = 0, totalRT = 0;

    printf("%-4s %-10s  %6s %7s  %7s %5s  %10s %5s %5s  %8s\n",
           "PID", "Stage", "InitPr", "FinalPr",
           "Arrival", "Burst", "Completion", "TAT", "Wait", "Response");
    printf("──────────────────────────────────────────────────────────────\n");

    for (int i = 0; i < g_processCount; i++) {
        Process *p = &g_processes[i];
        p->turnaroundTime = p->completionTime - p->arrivalTime;
        p->waitingTime    = p->turnaroundTime  - p->burstTime;
        totalWT  += p->waitingTime;
        totalTAT += p->turnaroundTime;
        totalRT  += p->responseTime;

        printf("P%-3d %-10s  %6d %7d  %7d %5d  %10d %5d %5d  %8d\n",
               p->pid,
               STAGE_NAMES[p->initialPriority],
               p->initialPriority,
               p->priority,
               p->arrivalTime,
               p->burstTime,
               p->completionTime,
               p->turnaroundTime,
               p->waitingTime,
               p->responseTime);
    }

    int n = g_processCount;
    printf("──────────────────────────────────────────────────────────────\n");
    printf("Avg Waiting Time:     %.2f\n", totalWT  / n);
    printf("Avg Turnaround Time:  %.2f\n", totalTAT / n);
    printf("Avg Response Time:    %.2f\n", totalRT  / n);
    printf("Context Switches:     %d\n", g_contextSwitches);
    printf("Final Base Quantum:   %d tick(s)\n", g_timeSlice);
    printf("Final Aging Limit:    %d tick(s)\n", g_agingLimit);
    printf("Context Switch Guard: q >= %d × %d ms; minimum = %d tick(s)\n",
           CONTEXT_SWITCH_FACTOR, CONTEXT_SWITCH_MS,
           minQuantumTicksFromContextSwitch());
    printf("Queue Algorithms:      Q3=%s | Q2=%s | Q1=%s | Q0=%s\n",
           QUEUE_ALGO_NAMES[3], QUEUE_ALGO_NAMES[2],
           QUEUE_ALGO_NAMES[1], QUEUE_ALGO_NAMES[0]);

    /* ── Per-workstation Gantt chart ─────────────────────── */
    printf("\nGantt Chart (per workstation — work cells plus visible context switches):\n");
    printf("CS = context switch / scheduling handoff before a part starts running.\n");
    printf("──────────────────────────────────────────────────────\n");

    for (int p = PRIORITY_LEVELS - 1; p >= 0; p--) {
        printf("%-10s: ", STAGE_NAMES[p]);
        int printed = 0;
        for (int t = 0; t < g_ganttSize; t++) {
            if (g_ganttStage[t] == p) {
                if (g_ganttPid[t] == GANTT_CONTEXT_SWITCH)
                    printf("| CS  ");
                else if (g_ganttPid[t] == -1)
                    printf("|IDLE ");
                else
                    printf("| P%-2d ", g_ganttPid[t]);
                printed = 1;
            }
        }
        if (!printed) printf("(idle throughout)");
        printf("|\n");
    }
}

void factoryDestroy(void) {
    close(g_pipe[0]);
    close(g_pipe[1]);
    sem_destroy(&g_beltSlots);
    for (int p = 0; p < PRIORITY_LEVELS; p++)
        sem_destroy(&g_taskReady[p]);
    pthread_mutex_destroy(&g_mutex);
    pthread_mutex_destroy(&g_printMutex);
}
