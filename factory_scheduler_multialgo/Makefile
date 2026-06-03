# ── Car Parts Factory Scheduler ── Makefile ─────────────────────────────────
#
# Build:   make
# Run:     ./scheduler
# Clean:   make clean
#
# -pthread links libpthread and enables POSIX thread declarations.
# Required for: pthread_create/join/mutex, sem_init/wait/post, pipe().
# ─────────────────────────────────────────────────────────────────────────────

CC      = gcc
CFLAGS  = -Wall -Wextra -pthread
TARGET  = scheduler
OBJS    = main.o queue.o scheduler.o factory.o

# ── Link ─────────────────────────────────────────────────────────────────────
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

# ── Compile each translation unit ────────────────────────────────────────────
main.o: main.c process.h scheduler.h factory.h
	$(CC) $(CFLAGS) -c main.c

queue.o: queue.c queue.h
	$(CC) $(CFLAGS) -c queue.c

scheduler.o: scheduler.c scheduler.h process.h queue.h
	$(CC) $(CFLAGS) -c scheduler.c

factory.o: factory.c factory.h scheduler.h process.h queue.h
	$(CC) $(CFLAGS) -c factory.c

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	rm -f $(OBJS) $(TARGET)
