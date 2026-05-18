#ifndef FACTORY_H
#define FACTORY_H

/*
 * factory.h — Multithreaded Car Parts Factory
 * ============================================
 *
 * OS concepts demonstrated
 * ─────────────────────────
 *  Threads      : one pthread per workstation (one per priority level)
 *                 + a dispatcher thread + a logger thread
 *  Mutex        : g_mutex   — protects processes[] and queues[]
 *                 g_printMutex — serialises console output
 *  Semaphores   : g_taskReady[p] — signals workstation p that a part arrived
 *                 g_beltSlots    — limits conveyor belt capacity
 *  IPC (pipe)   : workstation threads write completed PIDs to g_pipe[1];
 *                 the logger thread reads from g_pipe[0] and records stats
 *
 * Factory analogy
 * ───────────────
 *  processes[]       → parts being manufactured
 *  queues[p]         → staging area in front of workstation p
 *  beltSlots sem     → conveyor belt between stages (finite capacity)
 *  taskReady[p] sem  → green light on workstation p ("part ready for you")
 *  g_mutex           → shared tool rack / clipboard that workers take turns using
 *  pipe              → intercom between shopfloor and the plant manager's log
 */

#include <pthread.h>
#include <semaphore.h>
#include "process.h"
#include "queue.h"
#include "scheduler.h"

/* ── Gantt chart capacity ──────────────────────────────── */
#define GANTT_MAX 4000

/* ── Shared simulation state (defined in factory.c) ──────
   All access must be guarded by g_mutex except g_shutdown
   which is written once and only read afterwards.          */
extern Process     *g_processes;
extern int          g_processCount;
extern Queue        g_queues[PRIORITY_LEVELS];
extern int          g_tick;
extern int          g_completed;
extern int          g_timeSlice;
extern volatile int g_shutdown;   /* set to 1 by dispatcher at end */

/* ── Synchronisation primitives ───────────────────────────
   Locking order rule (to prevent deadlock):
     ALWAYS acquire g_mutex before g_printMutex, never the reverse.
   In practice we avoid holding both at the same time by building
   formatted strings under g_mutex and printing after releasing it. */
extern pthread_mutex_t g_mutex;        /* guards processes[] + queues[]  */
extern pthread_mutex_t g_printMutex;   /* serialises printf              */
extern sem_t           g_taskReady[PRIORITY_LEVELS]; /* one per queue    */
extern sem_t           g_beltSlots;    /* conveyor belt capacity         */

/* ── IPC pipe ─────────────────────────────────────────────
   g_pipe[1] write-end: workstation threads (producers)
   g_pipe[0] read-end:  logger thread       (consumer)    */
extern int g_pipe[2];

/* ── Gantt log (guarded by g_mutex) ──────────────────────
   Each entry records one tick of work at one workstation. */
extern int g_ganttPid  [GANTT_MAX];   /* pid processed (-1 = idle)      */
extern int g_ganttStage[GANTT_MAX];   /* workstation priority level      */
extern int g_ganttTick [GANTT_MAX];   /* simulated tick number           */
extern int g_ganttSize;

/* ── Thread handles ───────────────────────────────────────*/
extern pthread_t g_wsThreads[PRIORITY_LEVELS]; /* workstation threads    */
extern pthread_t g_dispThread;                 /* dispatcher thread      */
extern pthread_t g_logThread;                  /* IPC logger thread      */

/* ── Public API ───────────────────────────────────────────*/

/**
 * factoryInit — initialise all shared state and OS primitives.
 * Must be called before factoryStart().
 */
void factoryInit(Process *procs, int count, int timeSlice);

/**
 * factoryStart — spawn all threads (dispatcher, workstations, logger).
 */
void factoryStart(void);

/**
 * factoryWait — join all threads; blocks until the factory is done.
 */
void factoryWait(void);

/**
 * factoryPrintStats — print the final production report and Gantt chart.
 * Call after factoryWait().
 */
void factoryPrintStats(void);

/**
 * factoryDestroy — release semaphores, mutexes, and the pipe.
 */
void factoryDestroy(void);

#endif /* FACTORY_H */
