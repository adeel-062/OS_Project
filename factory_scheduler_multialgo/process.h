#ifndef PROCESS_H
#define PROCESS_H

/*
 * process.h — Car Parts Factory Scheduler
 *
 * Each Process represents a manufacturing task (car part).
 * Priority maps to the workstation stage:
 *   0 = Stamping  (lowest)
 *   1 = Painting
 *   2 = Welding
 *   3 = Assembly  (highest)
 */

typedef enum {
    NEW,
    READY,
    RUNNING,
    BLOCKED,
    TERMINATED
} State;

typedef struct {
    int pid;               /* part / task ID                              */
    int priority;          /* current priority (changes via aging & MLFQ) */
    int initialPriority;   /* original priority entered by user           */
    int arrivalTime;       /* tick at which the part enters the factory   */
    int burstTime;         /* total work units needed                     */
    int remainingTime;     /* work units still to do                      */
    int timeSliceRemaining;/* ticks left in current quantum               */
    int waitingTime;       /* computed at end: TAT - burst                */
    int turnaroundTime;    /* completionTime - arrivalTime                */
    int responseTime;      /* tick of first CPU - arrivalTime             */
    int completionTime;    /* tick when the part finished                 */
    int readyWaitTime;     /* ticks spent waiting (for aging counter)     */
    State state;
} Process;

#endif /* PROCESS_H */
