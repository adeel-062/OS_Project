#ifndef SCHEDULER_H
#define SCHEDULER_H

/*
 * scheduler.h — Constants, stage names, and core scheduler helpers.
 *
 * PRIORITY_LEVELS  : number of workstation tiers (queues)
 * MLFQ_ENABLED     : 1 = demote priority after quantum expires
 * TICK_MS          : real milliseconds per simulated clock tick
 *
 * New scheduling additions:
 *  1. Progressive MLFQ quantum:
 *     Q3 highest priority gets base quantum,
 *     Q2 gets 2 × base,
 *     Q1 gets 4 × base,
 *     Q0 lowest priority gets 8 × base.
 *
 *  2. Dynamic base quantum:
 *     The base quantum is recalculated from the current ready workload
 *     using the median remaining burst time.
 *
 *  3. Context-switch safeguard:
 *     The chosen quantum must represent at least
 *     CONTEXT_SWITCH_FACTOR × CONTEXT_SWITCH_MS worth of time.
 *
 *  4. Dynamic aging limit:
 *     Aging threshold is derived from current base quantum and ready
 *     queue load instead of being only one fixed magic number.

 *  5. Different scheduling algorithm per queue:
 *     Q3 Assembly uses SJF/SRTF-style shortest remaining time first,
 *     Q2 Welding uses Round Robin/FIFO,
 *     Q1 Painting uses FCFS/FIFO,
 *     Q0 Stamping uses Longest Job First.
 */

#include "process.h"
#include "queue.h"

#define PRIORITY_LEVELS  4
#define MLFQ_ENABLED     1

#define DEFAULT_BASE_QUANTUM 2
#define MIN_BASE_QUANTUM     1
#define MAX_BASE_QUANTUM     12

#define CONTEXT_SWITCH_MS        2
#define CONTEXT_SWITCH_FACTOR    10

#define MIN_AGING_LIMIT  3
#define MAX_AGING_LIMIT  20

#define BELT_CAPACITY   MAX_PROCESSES   /* set high so belt never blocks in demos */
#define TICK_MS          80             /* 80 ms per tick → comfortable demo speed */

/* Human-readable names for each priority level (index = priority value).
 * Defined in scheduler.c, declared here for all translation units. */
extern const char *STAGE_NAMES[PRIORITY_LEVELS];
extern const char *QUEUE_ALGO_NAMES[PRIORITY_LEVELS];

/* Original single-threaded helpers (kept for backwards compatibility).
 * The factory threads use beltEnqueue() in factory.c instead. */
void addToReadyQueue(Process processes[], Queue readyQueues[], int pid);
int  getNextProcess (Queue readyQueues[]);
int  selectProcessFromQueue(Queue readyQueues[], Process processes[], int priority);

/* Shared scheduling policy helpers used by both the C factory simulation
 * and the Python GUI mirror. */
int clampInt(int value, int low, int high);
int minQuantumTicksFromContextSwitch(void);
int enforceContextSwitchGuard(int quantumTicks);
int quantumForPriority(int priority, int baseQuantum);
int computeDynamicBaseQuantumFromBursts(const int bursts[], int count, int fallbackQuantum);
int decideAgingLimit(int baseQuantum, int readyCount);

#endif /* SCHEDULER_H */
