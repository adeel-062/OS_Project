CC = gcc
CFLAGS = -Wall -Wextra -std=c11
OBJ = main.o scheduler.o queue.o

scheduler: $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) -o scheduler

main.o: main.c scheduler.h queue.h process.h
	$(CC) $(CFLAGS) -c main.c

scheduler.o: scheduler.c scheduler.h queue.h process.h
	$(CC) $(CFLAGS) -c scheduler.c

queue.o: queue.c queue.h
	$(CC) $(CFLAGS) -c queue.c

clean:
	rm -f *.o scheduler
