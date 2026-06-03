#include "queue.h"

void initQueue(Queue *q) {
    q->front = 0;
    q->rear  = -1;
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
        return q->items[q->front++];
    }
    return -1;
}


/*
 * removeAt — remove and return the item stored at a specific active
 * array index. This lets non-FIFO algorithms such as SJF/SRTF and LJF
 * select an item from the middle of the queue.
 */
int removeAt(Queue *q, int index) {
    if (isEmpty(q) || index < q->front || index > q->rear) {
        return -1;
    }

    int pid = q->items[index];

    for (int i = index; i < q->rear; i++) {
        q->items[i] = q->items[i + 1];
    }

    q->rear--;
    return pid;
}
