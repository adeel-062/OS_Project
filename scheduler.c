#include "scheduler.h"

void addToReadyQueue(Process processes[], Queue readyQueues[], int pid) {
    int pr = processes[pid].priority;
    enqueue(&readyQueues[pr], pid);
    processes[pid].state = READY;
}

int getNextProcess(Queue readyQueues[]) {
    for (int p = PRIORITY_LEVELS - 1; p >= 0; p--) {
        if (!isEmpty(&readyQueues[p])) {
            return dequeue(&readyQueues[p]);
        }
    }
    return -1;
}
