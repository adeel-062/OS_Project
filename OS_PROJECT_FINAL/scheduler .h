#ifndef SCHEDULER_H
#define SCHEDULER_H

/*
 * scheduler.h — Constants, stage names, and core scheduler helpers.
 *
 * PRIORITY_LEVELS  : number of workstation tiers (queues)
 * MLFQ_ENABLED     : 1 = demote priority after quantum expires
 * AGING_LIMIT      : ticks a READY task waits before promotion
 * BELT_CAPACITY    : max parts on the conveyor belt simultaneously
 *                    (controls the beltSlots semaphore initial value)
 * TICK_MS          : real milliseconds per simulated clock tick
 */

#include "process.h"
#include "queue.h"

#define PRIORITY_LEVELS  4
#define MLFQ_ENABLED     1
#define AGING_LIMIT      5
#define BELT_CAPACITY   MAX_PROCESSES   /* set high so belt never blocks in demos */
#define TICK_MS          80             /* 80 ms per tick → comfortable demo speed */

/* Human-readable names for each priority level (index = priority value).
 * Defined in scheduler.c, declared here for all translation units. */
extern const char *STAGE_NAMES[PRIORITY_LEVELS];

/* Original single-threaded helpers (kept for backwards compatibility).
 * The factory threads use beltEnqueue() in factory.c instead. */
void addToReadyQueue(Process processes[], Queue readyQueues[], int pid);
int  getNextProcess (Queue readyQueues[]);

#endif /* SCHEDULER_H */
