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
