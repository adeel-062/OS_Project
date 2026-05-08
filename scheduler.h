#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "process.h"
#include "queue.h"

#define PRIORITY_LEVELS 4

void addToReadyQueue(Process processes[], Queue readyQueues[], int pid);
int getNextProcess(Queue readyQueues[]);

#endif
