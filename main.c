#include <stdio.h>
#include "process.h"
#include "queue.h"
#include "scheduler.h"

#define MAX_INPUT_PROCESSES 20
#define DEFAULT_TIME_SLICE 2
#define AGING_LIMIT 5
#define MLFQ_ENABLED 1

void runSimulation(Process processes[], int processCount, int timeSlice);
void automaticMode();
void manualMode();
int hasHigherPriorityProcess(Queue readyQueues[], int currentPriority);
void applyAging(Process processes[], int processCount, Queue readyQueues[]);
void applyMLFQDemotion(Process processes[], int pid);

int main() {
    int choice;

    printf("============================================\n");
    printf(" Priority-Based Preemptive Scheduler\n");
    printf("============================================\n");
    printf("1. Manual Mode\n");
    printf("2. Automatic Mode\n");
    printf("Enter your choice: ");
    scanf("%d", &choice);

    if (choice == 1) {
        manualMode();
    }
    else if (choice == 2) {
        automaticMode();
    }
    else {
        printf("Invalid choice. Exiting program.\n");
    }

    return 0;
}

void automaticMode() {
    Process processes[4];

    int processCount = 4;
    int timeSlice = DEFAULT_TIME_SLICE;

    processes[0] = (Process){0, 3, 0, 5, 5, timeSlice, 0, 0, -1, 0, 0, NEW};
    processes[1] = (Process){1, 2, 1, 4, 4, timeSlice, 0, 0, -1, 0, 0, NEW};
    processes[2] = (Process){2, 3, 2, 3, 3, timeSlice, 0, 0, -1, 0, 0, NEW};
    processes[3] = (Process){3, 1, 3, 2, 2, timeSlice, 0, 0, -1, 0, 0, NEW};

    printf("\nAutomatic Mode Selected.\n");
    printf("Running predefined workload...\n");

    runSimulation(processes, processCount, timeSlice);
}

void manualMode() {
    Process processes[MAX_INPUT_PROCESSES];
    int processCount;
    int timeSlice;

    printf("\nManual Mode Selected.\n");

    printf("Enter number of processes: ");
    scanf("%d", &processCount);

    if (processCount <= 0 || processCount > MAX_INPUT_PROCESSES) {
        printf("Invalid number of processes. Must be between 1 and %d.\n", MAX_INPUT_PROCESSES);
        return;
    }

    printf("Enter time slice: ");
    scanf("%d", &timeSlice);

    if (timeSlice <= 0) {
        printf("Invalid time slice. Time slice must be greater than 0.\n");
        return;
    }

    for (int i = 0; i < processCount; i++) {
        int arrivalTime;
        int burstTime;
        int priority;

        printf("\nEnter details for P%d\n", i);

        printf("Arrival Time: ");
        scanf("%d", &arrivalTime);

        printf("Burst Time: ");
        scanf("%d", &burstTime);

        printf("Priority (0 lowest, 3 highest): ");
        scanf("%d", &priority);

        if (arrivalTime < 0 || burstTime <= 0 || priority < 0 || priority >= PRIORITY_LEVELS) {
            printf("Invalid input for P%d. Please restart and enter valid values.\n", i);
            return;
        }

        processes[i] = (Process){i, priority, arrivalTime, burstTime, burstTime, timeSlice, 0, 0, -1, 0, 0, NEW};
    }

    runSimulation(processes, processCount, timeSlice);
}

int hasHigherPriorityProcess(Queue readyQueues[], int currentPriority) {
    for (int p = PRIORITY_LEVELS - 1; p > currentPriority; p--) {
        if (!isEmpty(&readyQueues[p])) {
            return 1;
        }
    }
    return 0;
}

void applyAging(Process processes[], int processCount, Queue readyQueues[]) {
    int aged = 0;

    for (int i = 0; i < processCount; i++) {
        if (processes[i].state == READY) {
            processes[i].readyWaitTime++;

            if (processes[i].readyWaitTime >= AGING_LIMIT &&
                processes[i].priority < PRIORITY_LEVELS - 1) {

                int oldPriority = processes[i].priority;
                processes[i].priority++;
                processes[i].readyWaitTime = 0;
                aged = 1;

                printf("Aging applied: P%d priority increased from %d to %d\n",
                       processes[i].pid, oldPriority, processes[i].priority);
            }
        }
    }

    if (aged) {
        for (int p = 0; p < PRIORITY_LEVELS; p++) {
            initQueue(&readyQueues[p]);
        }

        for (int i = 0; i < processCount; i++) {
            if (processes[i].state == READY) {
                int pr = processes[i].priority;
                enqueue(&readyQueues[pr], i);
            }
        }
    }
}

void applyMLFQDemotion(Process processes[], int pid) {
    if (MLFQ_ENABLED && processes[pid].priority > 0) {
        int oldPriority = processes[pid].priority;
        processes[pid].priority--;

        printf("MLFQ demotion: P%d priority decreased from %d to %d\n",
               processes[pid].pid,
               oldPriority,
               processes[pid].priority);
    }
}

void runSimulation(Process processes[], int processCount, int timeSlice) {
    Queue readyQueues[PRIORITY_LEVELS];

    for (int i = 0; i < PRIORITY_LEVELS; i++) {
        initQueue(&readyQueues[i]);
    }

    int completed = 0;
    int tick = 0;
    int currentPid = -1;
    int contextSwitches = 0;

    int ganttChart[1000];
    int ganttSize = 0;

    while (completed < processCount) {
        printf("\n=== Tick %d ===\n", tick);

        for (int i = 0; i < processCount; i++) {
        	if (processes[i].arrivalTime == tick && processes[i].state == NEW) {
                	addToReadyQueue(processes, readyQueues, i);
                	printf("P%d arrived and entered Ready queue %d\n", i, processes[i].priority);
            	}
	}

	applyAging(processes, processCount, readyQueues);

	if (currentPid != -1) {
                int currentPriority = processes[currentPid].priority;

                if (hasHigherPriorityProcess(readyQueues, currentPriority)) {
                	printf("Higher-priority process arrived. P%d preempted immediately\n", currentPid);

        		processes[currentPid].state = READY;
        		processes[currentPid].timeSliceRemaining = timeSlice;
        		addToReadyQueue(processes, readyQueues, currentPid);

        		currentPid = -1;
    		}
    	}

        if (currentPid == -1) {
            currentPid = getNextProcess(readyQueues);

            if (currentPid != -1) {
                processes[currentPid].state = RUNNING;
                contextSwitches++;

                if (processes[currentPid].responseTime == -1) {
                    processes[currentPid].responseTime = tick - processes[currentPid].arrivalTime;
                }

                printf("P%d scheduled\n", currentPid);
            }
        }

        if (currentPid != -1) {
            printf("P%d is running\n", currentPid);

	    ganttChart[ganttSize++] = currentPid;

            processes[currentPid].remainingTime--;
            processes[currentPid].timeSliceRemaining--;

            if (processes[currentPid].remainingTime == 0) {
                processes[currentPid].state = TERMINATED;
                processes[currentPid].completionTime = tick + 1;
                printf("P%d finished and terminated\n", currentPid);
                completed++;
                currentPid = -1;
            }
            else if (processes[currentPid].timeSliceRemaining == 0) {
		printf("P%d quantum expired\n", currentPid);

		applyMLFQDemotion(processes, currentPid);

                processes[currentPid].state = READY;
                processes[currentPid].timeSliceRemaining = timeSlice;
                addToReadyQueue(processes, readyQueues, currentPid);

		printf("P%d quantum expired, preempted and requeued\n", currentPid);
                currentPid = -1;
            }
        }
        else {
            printf("CPU is idle\n");
	    ganttChart[ganttSize++] = -1;
        }

        tick++;
    }

    printf("\nAll processes completed.\n");

    float totalWaitingTime = 0;
    float totalTurnaroundTime = 0;
    float totalResponseTime = 0;

    printf("\nFinal Process Statistics:\n");
    printf("PID\tPriority\tArrival\tBurst\tCompletion\tTurnaround\tWaiting\tResponse\n");

    for (int i = 0; i < processCount; i++) {
        processes[i].turnaroundTime = processes[i].completionTime - processes[i].arrivalTime;
        processes[i].waitingTime = processes[i].turnaroundTime - processes[i].burstTime;

        totalWaitingTime += processes[i].waitingTime;
        totalTurnaroundTime += processes[i].turnaroundTime;
        totalResponseTime += processes[i].responseTime;

        printf("P%d\t%d\t\t%d\t%d\t%d\t\t%d\t\t%d\t%d\n",
               processes[i].pid,
               processes[i].priority,
               processes[i].arrivalTime,
               processes[i].burstTime,
               processes[i].completionTime,
               processes[i].turnaroundTime,
               processes[i].waitingTime,
               processes[i].responseTime);
    }

    printf("\nAverage Waiting Time: %.2f\n", totalWaitingTime / processCount);
    printf("Average Turnaround Time: %.2f\n", totalTurnaroundTime / processCount);
    printf("Average Response Time: %.2f\n", totalResponseTime / processCount);
    printf("Total Context Switches: %d\n", contextSwitches);

    printf("\nGantt Trace:\n");

    for (int i = 0; i < ganttSize; i++) {
        if (ganttChart[i] == -1) {
            printf("| IDLE ");
        } else {
            printf("| P%d ", ganttChart[i]);
        }
    }
    printf("|\n");

    printf("\nTime:\n");
    for (int i = 0; i <= ganttSize; i++) {
        printf("%d\t", i);
    }
    printf("\n");
}
