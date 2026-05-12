CC     = gcc
CFLAGS = -Wall -std=c17 -O2
LIBS   = -lpthread -lm
TARGET = assignment_4

all: $(TARGET)

$(TARGET): assignment_4.c
	$(CC) $(CFLAGS) -o $(TARGET) assignment_4.c $(LIBS)

clean:
	rm -f $(TARGET) *.out
