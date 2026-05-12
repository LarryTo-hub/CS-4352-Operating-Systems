# CS 4352 - Assignment 5 Makefile
CC = gcc
CFLAGS = -Wall -std=c11

all: assignment_5

assignment_5: main.o
	$(CC) $(CFLAGS) -o assignment_5 main.o

main.o: main.c
	$(CC) $(CFLAGS) -c main.c

clean:
	rm -f *.o assignment_5 *.out
