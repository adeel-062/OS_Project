/*
 * main.c — Car Parts Factory Scheduler  (Phase 9)
 * =================================================
 * Entry point only.  All OS logic lives in factory.c.
 *
 * What is demonstrated
 * ─────────────────────
 *  pthread_create / pthread_join  — parallel workstations + dispatcher + logger
 *  pthread_mutex_t                — protects shared process/queue state
 *  sem_t (POSIX semaphores)       — taskReady[p] and beltSlots (producer-consumer)
 *  pipe() IPC                     — workstation threads → logger thread
 *  MLFQ                           — quantum expiry demotes task to lower queue
 *  Aging                          — long-waiting tasks promoted to higher queue
 */

#include <stdio.h>
#include "process.h"
#include "scheduler.h"   /* PRIORITY_LEVELS, STAGE_NAMES */
#include "factory.h"

#define MAX_INPUT_PROCESSES 20
/* User enters the base quantum. Actual station quantum is progressive:
   Q3=base, Q2=2×base, Q1=4×base, Q0=8×base. */

/* ── Helper: initialise, start, wait, print, destroy ─────────────────────── */
static void runFactory(Process procs[], int count, int timeSlice) {
    factoryInit(procs, count, timeSlice);
    factoryStart();
    factoryWait();
    factoryPrintStats();
    factoryDestroy();
}

/* ── Automatic mode ──────────────────────────────────────────────────────── */
static void automaticMode(void) {
    int ts = DEFAULT_BASE_QUANTUM;

    /*
     * Five pre-defined parts:
     *   P0 – Assembly  (priority 3, arrives t=0, burst=10)
     *   P1 – Welding   (priority 2, arrives t=0, burst=6)
     *   P2 – Painting  (priority 1, arrives t=2, burst=8)
     *   P3 – Assembly  (priority 3, arrives t=1, burst=7)
     *   P4 – Welding   (priority 2, arrives t=3, burst=4)
     *
     * Layout: {pid, priority, initialPriority, arrival, burst, remaining,
     *          timeSlice, waitingTime, turnaroundTime, responseTime,
     *          completionTime, readyWaitTime, state}
     */
    Process procs[5] = {
        {0, 3, 3, 0, 10, 10, ts, 0, 0, -1, 0, 0, NEW},
        {1, 2, 2, 0,  6,  6, ts, 0, 0, -1, 0, 0, NEW},
        {2, 1, 1, 2,  8,  8, ts, 0, 0, -1, 0, 0, NEW},
        {3, 3, 3, 1,  7,  7, ts, 0, 0, -1, 0, 0, NEW},
        {4, 2, 2, 3,  4,  4, ts, 0, 0, -1, 0, 0, NEW},
    };

    printf("\nAutomatic Mode — running predefined factory workload (5 parts).\n");
    runFactory(procs, 5, ts);
}

/* ── Manual mode ─────────────────────────────────────────────────────────── */
static void manualMode(void) {
    Process procs[MAX_INPUT_PROCESSES];
    int count, ts;

    printf("\nManual Mode\n");
    printf("Enter number of parts (1-%d): ", MAX_INPUT_PROCESSES);
    if (scanf("%d", &count) != 1 || count <= 0 || count > MAX_INPUT_PROCESSES) {
        printf("Invalid count.\n");
        return;
    }

    printf("Enter base time quantum (ticks): ");
    if (scanf("%d", &ts) != 1 || ts <= 0) {
        printf("Invalid time slice.\n");
        return;
    }

    for (int i = 0; i < count; i++) {
        int arr, burst, prio;

        printf("\n--- Part P%d ---\n", i);
        printf("  Stage / Priority\n");
        for (int p = PRIORITY_LEVELS - 1; p >= 0; p--)
            printf("    %d = %s\n", p, STAGE_NAMES[p]);
        printf("  Arrival Time  : ");
        if (scanf("%d", &arr)   != 1 || arr < 0)   { printf("Invalid.\n"); return; }
        printf("  Burst Time    : ");
        if (scanf("%d", &burst) != 1 || burst <= 0) { printf("Invalid.\n"); return; }
        printf("  Priority (0-%d): ", PRIORITY_LEVELS - 1);
        if (scanf("%d", &prio)  != 1 || prio < 0 || prio >= PRIORITY_LEVELS) {
            printf("Invalid.\n"); return;
        }

        procs[i] = (Process){
            i, prio, prio, arr, burst, burst, ts,
            0, 0, -1, 0, 0, NEW
        };
    }

    runFactory(procs, count, ts);
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(void) {
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║       Car Parts Factory Scheduler  —  Phase 9           ║\n");
    printf("║ pthread | semaphore | dynamic q | aging | MLFQ | IPC    ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");
    printf("1. Manual Mode\n");
    printf("2. Automatic Mode\n");
    printf("Choice: ");

    int choice;
    if (scanf("%d", &choice) != 1) {
        printf("Invalid input.\n");
        return 1;
    }

    if      (choice == 1) manualMode();
    else if (choice == 2) automaticMode();
    else                  printf("Invalid choice.\n");

    return 0;
}
