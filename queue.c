#include "queue.h"

void initQueue(Queue *q) {
    q->front = 0;
    q->rear = -1;
}

int isEmpty(Queue *q) {
    return q->front > q->rear;
}

void enqueue(Queue *q, int pid) {
    if (q->rear < MAX_PROCESSES - 1) {
        q->rear++;
        q->items[q->rear] = pid;
    }
}

int dequeue(Queue *q) {
    if (!isEmpty(q)) {
        int pid = q->items[q->front];
        q->front++;
        return pid;
    }
    return -1;
}
