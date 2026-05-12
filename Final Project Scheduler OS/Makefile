# =============================================================================
# Makefile for CS 4352 Final Project - Elevator Operating System Scheduler
# Author: Larry To (R11615587)
# Builds the scheduler_os executable from scheduler_os.c.
# Requires libcurl and pthreads (both available on the HPCC nocona partition
# after `module load gcc`).
# =============================================================================

CC      = gcc
CFLAGS  = -std=gnu17 -Wall -O2
LDFLAGS = -lcurl -lpthread

TARGET  = scheduler_os
SRC     = scheduler_os.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
