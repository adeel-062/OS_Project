#include "scheduler.h"

/*
 * STAGE_NAMES[priority] — factory workstation label for each priority level.
 * Index 0 is the lowest priority, index 3 is the highest.
 */
const char *STAGE_NAMES[PRIORITY_LEVELS] = {
    "Stamping",   /* priority 0 — lowest  */
    "Painting",   /* priority 1           */
    "Welding",    /* priority 2           */
    "Assembly"    /* priority 3 — highest */
};


/*
 * QUEUE_ALGO_NAMES[priority] — internal scheduling rule used inside
 * each workstation queue. The overall system is still MLFQ, but each
 * queue now chooses its next part differently.
 */
const char *QUEUE_ALGO_NAMES[PRIORITY_LEVELS] = {
    "Longest Job First",        /* Q0 Stamping  */
    "FCFS / FIFO",              /* Q1 Painting  */
    "Round Robin / FIFO",       /* Q2 Welding   */
    "SJF / Shortest Remaining"  /* Q3 Assembly  */
};

/*
 * addToReadyQueue — place a process into the correct priority queue
 * and mark it READY.  Used by the original single-threaded path.
 * In the threaded factory, beltEnqueue() in factory.c is used instead.
 */
void addToReadyQueue(Process processes[], Queue readyQueues[], int pid) {
    int pr = processes[pid].priority;
    enqueue(&readyQueues[pr], pid);
    processes[pid].state         = READY;
    processes[pid].readyWaitTime = 0;
}

/*
 * getNextProcess — return the pid of the highest-priority waiting process,
 * or -1 if all queues are empty.
 */
int getNextProcess(Queue readyQueues[]) {
    for (int p = PRIORITY_LEVELS - 1; p >= 0; p--) {
        if (!isEmpty(&readyQueues[p]))
            return dequeue(&readyQueues[p]);
    }
    return -1;
}

/*
 * selectProcessFromQueue — queue-specific internal scheduling policy.
 *
 * The overall system remains MLFQ/priority-based. Once a workstation queue
 * is chosen, this function decides which PID inside that queue should run.
 *
 * Q3 Assembly : SJF/SRTF-style shortest remaining time first
 * Q2 Welding  : Round Robin/FIFO
 * Q1 Painting : FCFS/FIFO
 * Q0 Stamping : Longest Job First
 *
 * Ties are intentionally resolved by the earliest queue position, so the
 * behavior remains predictable and fair within equal burst/remaining times.
 */
int selectProcessFromQueue(Queue readyQueues[], Process processes[], int priority) {
    Queue *q = &readyQueues[priority];

    if (isEmpty(q))
        return -1;

    /* Q2 and Q1 both use front-of-line selection. Q2 is explained as
       Round Robin because quantum expiry requeues unfinished parts. Q1 is
       explained as FCFS because it preserves arrival/order flow. */
    if (priority == 2 || priority == 1) {
        return dequeue(q);
    }

    int bestIndex = q->front;

    for (int i = q->front + 1; i <= q->rear; i++) {
        int candidatePid = q->items[i];
        int bestPid      = q->items[bestIndex];

        if (priority == 3) {
            /* Assembly: shortest urgent remaining task first. */
            if (processes[candidatePid].remainingTime < processes[bestPid].remainingTime) {
                bestIndex = i;
            }
        }
        else if (priority == 0) {
            /* Stamping: longest background job first. */
            if (processes[candidatePid].remainingTime > processes[bestPid].remainingTime) {
                bestIndex = i;
            }
        }
    }

    return removeAt(q, bestIndex);
}

int clampInt(int value, int low, int high) {
    if (value < low)  return low;
    if (value > high) return high;
    return value;
}

/*
 * Context-switch safeguard:
 * The screenshot rule says q should be at least 10 × cs.
 * Because our simulation quantum is measured in ticks, we convert
 * the required milliseconds into the minimum number of ticks.
 */
int minQuantumTicksFromContextSwitch(void) {
    int requiredMs = CONTEXT_SWITCH_FACTOR * CONTEXT_SWITCH_MS;
    int ticks = (requiredMs + TICK_MS - 1) / TICK_MS;  /* ceiling division */
    return ticks < 1 ? 1 : ticks;
}

int enforceContextSwitchGuard(int quantumTicks) {
    int minTicks = minQuantumTicksFromContextSwitch();
    if (quantumTicks < minTicks)
        return minTicks;
    return quantumTicks;
}

/*
 * Progressive MLFQ quantum:
 * Higher-priority stations get shorter quanta for responsiveness.
 * Lower-priority stations get longer quanta to reduce unnecessary
 * context switching for heavier/background work.
 *
 * Q3 Assembly  : base
 * Q2 Welding   : 2 × base
 * Q1 Painting  : 4 × base
 * Q0 Stamping  : 8 × base
 */
int quantumForPriority(int priority, int baseQuantum) {
    int multiplier = 1 << (PRIORITY_LEVELS - 1 - priority);
    int q = baseQuantum * multiplier;
    return enforceContextSwitchGuard(q);
}

/* Small insertion sort, enough for the tiny process counts in this project. */
static void sortAscending(int a[], int n) {
    for (int i = 1; i < n; i++) {
        int key = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > key) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = key;
    }
}

/*
 * Dynamic base quantum:
 * We use the median remaining burst time of READY tasks.
 * Median is preferred over average because one huge outlier should not make
 * every other workstation receive an excessively long quantum.
 */
int computeDynamicBaseQuantumFromBursts(const int bursts[], int count, int fallbackQuantum) {
    if (count <= 0) {
        return enforceContextSwitchGuard(
            clampInt(fallbackQuantum, MIN_BASE_QUANTUM, MAX_BASE_QUANTUM)
        );
    }

    int temp[MAX_PROCESSES];
    if (count > MAX_PROCESSES)
        count = MAX_PROCESSES;

    for (int i = 0; i < count; i++)
        temp[i] = bursts[i];

    sortAscending(temp, count);

    int median;
    if (count % 2 == 1) {
        median = temp[count / 2];
    } else {
        median = (temp[count / 2 - 1] + temp[count / 2] + 1) / 2;
    }

    median = clampInt(median, MIN_BASE_QUANTUM, MAX_BASE_QUANTUM);
    return enforceContextSwitchGuard(median);
}

/*
 * Dynamic aging limit:
 * Aging should respond to both quantum length and queue load.
 * - Shorter base quantum → aging can happen sooner.
 * - More ready tasks → allow a little more waiting because the belt is busier.
 *
 * The result is clamped so aging never becomes too aggressive or too slow.
 */
int decideAgingLimit(int baseQuantum, int readyCount) {
    int limit = (2 * baseQuantum) + (readyCount / PRIORITY_LEVELS) + 1;
    return clampInt(limit, MIN_AGING_LIMIT, MAX_AGING_LIMIT);
}
