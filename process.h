#ifndef PROCESS_H
#define PROCESS_H

typedef enum {
    NEW,
    READY,
    RUNNING,
    BLOCKED,
    TERMINATED
} State;

typedef struct {
    int pid;
    int priority;
    int arrivalTime;
    int burstTime;
    int remainingTime;
    int timeSliceRemaining;

    int waitingTime;
    int turnaroundTime;
    int responseTime;
    int completionTime;

    int readyWaitTime;

    State state;
} Process;

#endif
