# ===========================================================================
# Makefile for CS4352 Assignment 3
# Author : Larry To (R11615587)
# Usage  : make           (builds the assignment_3 executable)
#          make clean     (removes executable and object files)
# Notes  : Requires gcc 14.2.0 on HPCC. Load with: module load gcc/14.2.0
#          Requires libcurl to be available on the system.
# ===========================================================================

CC      = gcc
CFLAGS  = -Wall -Wextra -g
LIBS    = -lcurl
TARGET  = assignment_3
SRCS    = assignment3.c

# default target: build the executable
$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LIBS)

# clean up built files
clean:
	rm -f $(TARGET) *.o
