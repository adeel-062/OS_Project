#ifndef QUEUE_H
#define QUEUE_H

#define MAX_PROCESSES 100

typedef struct {
    int items[MAX_PROCESSES];
    int front;
    int rear;
} Queue;

void initQueue(Queue *q);
int  isEmpty  (Queue *q);
void enqueue  (Queue *q, int pid);
int  dequeue  (Queue *q);
int  removeAt (Queue *q, int index);

#endif /* QUEUE_H */
