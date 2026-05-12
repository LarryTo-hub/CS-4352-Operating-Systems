/*
=============================================================================
Title        : CS 4352 - Assignment 4: Multithreading
Description  : Reads signed 32-bit integers from an input file one per line,
               and writes each integer to whichever output file(s) it qualifies for:
               even.out     - even integers
               odd.out      - odd integers
               positive.out - integers > 0
               negative.out - integers < 0
               square.out   - perfect squares (0, 1, 4, 9, ...)
               cube.out     - perfect cubes (..., -8, -1, 0, 1, 8, ...) 
               Uses two threads:
               1. Reader thread  - reads integers from file into a shared ring                        buffer
               2. Processor thread - pulls integers from the buffer and writes                        output
Author       : Larry To (R11615587)
Date         : 04/07/2026
Version      : 1.0
A mutex + two condition variables protect the shared buffer.
C Version    : C17 (gnu17)
=============================================================================
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <math.h>

/* Size of the shared ring buffer (power of 2 for easy wrap-around) */
#define BUFFER_SIZE 4096

/* Shared buffer and synchronization state (global for simplicity) */
int shared_buf[BUFFER_SIZE];
int buf_head  = 0;   /* processor reads from here */
int buf_tail  = 0;   /* reader writes here */
int buf_count = 0;   /* how many items are currently in the buffer */
int reader_done = 0; /* set to 1 when reader has finished reading */

pthread_mutex_t buf_mutex    = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  cond_not_empty = PTHREAD_COND_INITIALIZER;
pthread_cond_t  cond_not_full  = PTHREAD_COND_INITIALIZER;

/* Output file handles (global so both threads can reach them) */
FILE *f_even, *f_odd, *f_positive, *f_negative, *f_square, *f_cube;

/* Input filename passed to reader thread */
typedef struct {
    const char *filename;
} ReaderArgs;

/* Returns 1 if n is a perfect square, 0 otherwise. */
/* A perfect square must be >= 0. */
int is_perfect_square(int n) {
    if (n < 0) return 0;
    if (n == 0) return 1;

    long long r = (long long)sqrt((double)n);

    /* Check r-1, r, r+1 to guard against floating-point rounding */
    long long lo = (r > 0) ? r - 1 : 0;
    long long hi = r + 1;
    for (long long i = lo; i <= hi; i++) {
        if (i * i == (long long)n) return 1;
    }
    return 0;
}

/* Returns 1 if n is a perfect cube, 0 otherwise. */
/* Works for negative numbers too (-8 = (-2)^3). */
int is_perfect_cube(int n) {
    if (n == 0) return 1;

    /* cbrt() handles negative values correctly */
    long long r = (long long)round(cbrt((double)n));

    /* Check a small range around r to handle floating-point imprecision */
    for (long long i = r - 2; i <= r + 2; i++) {
        if (i * i * i == (long long)n) return 1;
    }
    return 0;
}

/* Reader thread: reads the input file and feeds integers into the */
/* shared ring buffer, blocking when the buffer is full. */
void* reader_thread(void* arg) {
    ReaderArgs *rargs = (ReaderArgs *)arg;

    FILE *infile = fopen(rargs->filename, "r");
    if (infile == NULL) {
        fprintf(stderr, "Error: cannot open input file '%s'\n", rargs->filename);
        /* Signal the processor so it doesn't wait forever */
        pthread_mutex_lock(&buf_mutex);
        reader_done = 1;
        pthread_cond_broadcast(&cond_not_empty);
        pthread_mutex_unlock(&buf_mutex);
        pthread_exit(NULL);
    }

    /* Use a large read buffer to cut down on disk I/O overhead */
    setvbuf(infile, NULL, _IOFBF, 1 << 20); /* 1 MB */

    char line[32]; /* max digits in a 32-bit int with sign + newline */
    while (fgets(line, sizeof(line), infile) != NULL) {
        int val = (int)strtol(line, NULL, 10);

        pthread_mutex_lock(&buf_mutex);

        /* Wait if the buffer is full */
        while (buf_count == BUFFER_SIZE) {
            pthread_cond_wait(&cond_not_full, &buf_mutex);
        }

        shared_buf[buf_tail] = val;
        buf_tail = (buf_tail + 1) % BUFFER_SIZE;
        buf_count++;

        /* Tell the processor there's something new to consume */
        pthread_cond_signal(&cond_not_empty);
        pthread_mutex_unlock(&buf_mutex);
    }

    fclose(infile);

    /* Mark reading as complete and wake the processor if it's sleeping */
    pthread_mutex_lock(&buf_mutex);
    reader_done = 1;
    pthread_cond_broadcast(&cond_not_empty);
    pthread_mutex_unlock(&buf_mutex);

    return NULL;
}

/* Processor thread: pulls integers off the shared buffer and writes  */
/* each one to the applicable output file(s). */
void* processor_thread(void* arg) {
    (void)arg; /* not used */

    while (1) {
        pthread_mutex_lock(&buf_mutex);

        /* Sleep while the buffer is empty and reader hasn't finished */
        while (buf_count == 0 && !reader_done) {
            pthread_cond_wait(&cond_not_empty, &buf_mutex);
        }

        /* If the buffer is empty AND the reader is done, we're finished */
        if (buf_count == 0 && reader_done) {
            pthread_mutex_unlock(&buf_mutex);
            break;
        }

        /* Grab the next integer from the head of the ring buffer */
        int val = shared_buf[buf_head];
        buf_head = (buf_head + 1) % BUFFER_SIZE;
        buf_count--;

        /* Let the reader know there's room again */
        pthread_cond_signal(&cond_not_full);
        pthread_mutex_unlock(&buf_mutex);

        /* Classify and write to output files */

        /* Even / odd (works correctly for negative numbers in C99+) */
        if (val % 2 == 0)
            fprintf(f_even, "%d\n", val);
        else
            fprintf(f_odd, "%d\n", val);

        /* Positive / negative (0 goes in neither) */
        if (val > 0)
            fprintf(f_positive, "%d\n", val);
        if (val < 0)
            fprintf(f_negative, "%d\n", val);

        /* Perfect square */
        if (is_perfect_square(val))
            fprintf(f_square, "%d\n", val);

        /* Perfect cube */
        if (is_perfect_cube(val))
            fprintf(f_cube, "%d\n", val);
    }

    return NULL;
}

/* main */
int main(int argc, char *argv[]) {
    /* Require exactly one argument: the input file path */
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <path_to_source_file>\n", argv[0]);
        return 1;
    }

    const char *input_path = argv[1];

    /* Make sure the file actually exists before doing anything else */
    FILE *check = fopen(input_path, "r");
    if (check == NULL) {
        fprintf(stderr, "Error: input file '%s' not found or cannot be opened\n",
                input_path);
        return 1;
    }
    fclose(check);

    /* Open all output files (created empty even if nothing is written) */
    f_even     = fopen("even.out",     "w");
    f_odd      = fopen("odd.out",      "w");
    f_positive = fopen("positive.out", "w");
    f_negative = fopen("negative.out", "w");
    f_square   = fopen("square.out",   "w");
    f_cube     = fopen("cube.out",     "w");

    if (!f_even || !f_odd || !f_positive || !f_negative || !f_square || !f_cube) {
        fprintf(stderr, "Error: failed to open one or more output files\n");
        return 1;
    }

    /* Use large write buffers to keep disk writes efficient */
    setvbuf(f_even,     NULL, _IOFBF, 1 << 20);
    setvbuf(f_odd,      NULL, _IOFBF, 1 << 20);
    setvbuf(f_positive, NULL, _IOFBF, 1 << 20);
    setvbuf(f_negative, NULL, _IOFBF, 1 << 20);
    setvbuf(f_square,   NULL, _IOFBF, 1 << 20);
    setvbuf(f_cube,     NULL, _IOFBF, 1 << 20);

    /* Pass the filename to the reader thread */
    ReaderArgs rargs;
    rargs.filename = input_path;

    /* Create both threads - from the slide examples */
    pthread_t reader_tid, processor_tid;

    pthread_create(&reader_tid,    NULL, reader_thread,    &rargs);
    pthread_create(&processor_tid, NULL, processor_thread, NULL);

    /* Wait for both threads to finish before cleaning up */
    pthread_join(reader_tid,    NULL);
    pthread_join(processor_tid, NULL);

    /* Clean up synchronization primitives */
    pthread_mutex_destroy(&buf_mutex);
    pthread_cond_destroy(&cond_not_empty);
    pthread_cond_destroy(&cond_not_full);

    /* Close output files (flushes any remaining buffered data) */
    fclose(f_even);
    fclose(f_odd);
    fclose(f_positive);
    fclose(f_negative);
    fclose(f_square);
    fclose(f_cube);

    return 0;
}
