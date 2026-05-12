/*
 * CS 4352 - Assignment #5
 * Scheduling Simulator
 *
 * Author: Larry To (R11615587)
   Date         : 04/16/2026
   Version      : 1.0
   C Version    : C17 (gnu17)
 *
 * Description:
 * This program reads a tab-separated input file containing process data
 * and simulates multiple CPU scheduling algorithms. For each algorithm,
 * it generates a separate output file containing the identifier of the
 * running process for each 10ms interval.
 *
 * Required algorithms:
 *   - FCFS
 *   - RR (10ms)
 *   - RR (40ms)
 *   - SPN
 *   - SRT
 *   - HRRN
 *   - Feedback (10ms)
 *
 * Notes:
 *   - Input arrival and service times are multiples of 10ms.
 *   - Output must contain exactly one line per 10ms interval.
 *   - Output "IDLE" if no process is runnable.
 *   - Continue until all processes finish.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* =========================================================
 * Constants
 * ========================================================= */

#define MAX_PROCESSES 1024
#define MAX_ID_LEN 64
#define TIME_SLICE_UNIT 10

/* Output file names required by the assignment */
#define FCFS_OUT      "fcfs.out"
#define RR10_OUT      "rr_10.out"
#define RR40_OUT      "rr_40.out"
#define SPN_OUT       "spn.out"
#define SRT_OUT       "srt.out"
#define HRRN_OUT      "hrrn.out"
#define FEEDBACK_OUT  "feedback.out"

/* =========================================================
 * Data Structures
 * ========================================================= */

typedef struct {
    char id[MAX_ID_LEN];

    int arrival_time;        /* Original arrival time in ms */
    int service_time;        /* Original total service time in ms */
    int remaining_time;      /* Remaining execution time in ms */

    int wait_time;           /* Total time spent waiting in ready state */
    int start_time;          /* First time scheduled, -1 if never run */
    int finish_time;         /* Completion time, -1 if not finished */

    int last_ready_time;     /* Useful for wait-time accounting */
    int level;               /* Useful for Feedback scheduler */
    int quantum_used;        /* Useful for RR / Feedback */

    bool arrived;            /* Has the process been admitted yet? */
    bool completed;          /* Has the process finished? */
} Process;

/*
 * Simple circular queue for ready-queue based schedulers
 * (FCFS, RR, Feedback may all benefit from this abstraction).
 */
typedef struct {
    int data[MAX_PROCESSES];
    int front;
    int rear;
    int size;
} Queue;

/* =========================================================
 * Function Prototypes
 * ========================================================= */

/* ---------- Parsing / Setup ---------- */
int  load_processes(const char *filename, Process processes[], int max_count);
void initialize_process(Process *p, const char *id, int arrival, int service);
void sort_processes_by_arrival(Process processes[], int count);
int  compare_processes_by_arrival(const void *a, const void *b);

/* ---------- Utility / Reset ---------- */
void copy_processes(Process dest[], const Process src[], int count);
void reset_runtime_fields(Process processes[], int count);
bool all_processes_completed(const Process processes[], int count);
int  count_completed_processes(const Process processes[], int count);

/* ---------- Output ---------- */
FILE *open_output_file(const char *filename);
void write_interval(FILE *fp, const char *process_id);

/* ---------- Queue Operations ---------- */
void queue_init(Queue *q);
bool queue_is_empty(const Queue *q);
bool queue_is_full(const Queue *q);
void enqueue(Queue *q, int value);
int  dequeue(Queue *q);
int  queue_peek(const Queue *q);

/* ---------- Simulation Helpers ---------- */
void admit_arrived_processes(Process processes[], int count, int current_time, Queue *ready_queue);
void update_wait_times(Process processes[], int count, int running_index, int current_time);
int  select_spn_process(const Process processes[], int count, int current_time);
int  select_srt_process(const Process processes[], int count, int current_time);
int  select_hrrn_process(const Process processes[], int count, int current_time);

/* ---------- Scheduler Drivers ---------- */
void run_fcfs(const Process original[], int count);
void run_rr(const Process original[], int count, int quantum_ms, const char *output_filename);
void run_spn(const Process original[], int count);
void run_srt(const Process original[], int count);
void run_hrrn(const Process original[], int count);
void run_feedback(const Process original[], int count);

/* =========================================================
 * Main
 * ========================================================= */

int main(int argc, char *argv[]) {
    Process original[MAX_PROCESSES];
    int process_count;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <path_to_input_file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    process_count = load_processes(argv[1], original, MAX_PROCESSES);
    if (process_count <= 0) {
        fprintf(stderr, "Error: no processes were loaded.\n");
        return EXIT_FAILURE;
    }

    sort_processes_by_arrival(original, process_count);

    /* run all 7 schedulers back to back */
    run_fcfs(original, process_count);
    run_rr(original, process_count, 10, RR10_OUT);
    run_rr(original, process_count, 40, RR40_OUT);
    run_spn(original, process_count);
    run_srt(original, process_count);
    run_hrrn(original, process_count);
    run_feedback(original, process_count);

    return EXIT_SUCCESS;
}

/* =========================================================
 * Parsing / Setup
 * ========================================================= */

int load_processes(const char *filename, Process processes[], int max_count) {
    FILE *fp;
    char id[MAX_ID_LEN];
    int arrival, service;
    int count = 0;

    fp = fopen(filename, "r");
    if (fp == NULL) {
        fprintf(stderr, "Error: could not open input file '%s'\n", filename);
        return -1;
    }

    /*
     * Expected format per line:
     *   <process_id>\t<arrival_time>\t<service_time>
     *
     * Since the assignment says the file is well-formed, simple fscanf is okay.
     * Still, keep bounds and count checks.
     */
    while (count < max_count && fscanf(fp, "%63s\t%d\t%d", id, &arrival, &service) == 3) {
        initialize_process(&processes[count], id, arrival, service);
        count++;
    }

    fclose(fp);
    return count;
}

void initialize_process(Process *p, const char *id, int arrival, int service) {
    strncpy(p->id, id, MAX_ID_LEN - 1);
    p->id[MAX_ID_LEN - 1] = '\0';

    p->arrival_time = arrival;
    p->service_time = service;
    p->remaining_time = service;

    p->wait_time = 0;
    p->start_time = -1;
    p->finish_time = -1;

    p->last_ready_time = arrival;
    p->level = 0;
    p->quantum_used = 0;

    p->arrived = false;
    p->completed = false;
}

int compare_processes_by_arrival(const void *a, const void *b) {
    const Process *pa = (const Process *)a;
    const Process *pb = (const Process *)b;

    if (pa->arrival_time != pb->arrival_time) {
        return pa->arrival_time - pb->arrival_time;
    }

    /*
     * Deterministic tie-breaker:
     * If two processes arrive at the same time, fall back to ID order.
     * This helps keep output stable for grading.
     */
    return strcmp(pa->id, pb->id);
}

void sort_processes_by_arrival(Process processes[], int count) {
    qsort(processes, count, sizeof(Process), compare_processes_by_arrival);
}

/* =========================================================
 * Utility / Reset
 * ========================================================= */

void copy_processes(Process dest[], const Process src[], int count) {
    memcpy(dest, src, sizeof(Process) * count);
}

void reset_runtime_fields(Process processes[], int count) {
    int i;

    for (i = 0; i < count; i++) {
        processes[i].remaining_time = processes[i].service_time;
        processes[i].wait_time = 0;
        processes[i].start_time = -1;
        processes[i].finish_time = -1;
        processes[i].last_ready_time = processes[i].arrival_time;
        processes[i].level = 0;
        processes[i].quantum_used = 0;
        processes[i].arrived = false;
        processes[i].completed = false;
    }
}

bool all_processes_completed(const Process processes[], int count) {
    int i;

    for (i = 0; i < count; i++) {
        if (!processes[i].completed) {
            return false;
        }
    }

    return true;
}

int count_completed_processes(const Process processes[], int count) {
    int i, completed = 0;

    for (i = 0; i < count; i++) {
        if (processes[i].completed) {
            completed++;
        }
    }

    return completed;
}

/* =========================================================
 * Output
 * ========================================================= */

FILE *open_output_file(const char *filename) {
    FILE *fp = fopen(filename, "w");

    if (fp == NULL) {
        fprintf(stderr, "Error: could not open output file '%s'\n", filename);
        exit(EXIT_FAILURE);
    }

    return fp;
}

void write_interval(FILE *fp, const char *process_id) {
    fprintf(fp, "%s\n", process_id);
}

/* =========================================================
 * Queue Operations
 * ========================================================= */

void queue_init(Queue *q) {
    q->front = 0;
    q->rear = 0;
    q->size = 0;
}

bool queue_is_empty(const Queue *q) {
    return q->size == 0;
}

bool queue_is_full(const Queue *q) {
    return q->size == MAX_PROCESSES;
}

void enqueue(Queue *q, int value) {
    if (queue_is_full(q)) {
        fprintf(stderr, "Error: ready queue overflow.\n");
        exit(EXIT_FAILURE);
    }

    q->data[q->rear] = value;
    q->rear = (q->rear + 1) % MAX_PROCESSES;
    q->size++;
}

int dequeue(Queue *q) {
    int value;

    if (queue_is_empty(q)) {
        fprintf(stderr, "Error: dequeue from empty queue.\n");
        exit(EXIT_FAILURE);
    }

    value = q->data[q->front];
    q->front = (q->front + 1) % MAX_PROCESSES;
    q->size--;

    return value;
}

int queue_peek(const Queue *q) {
    if (queue_is_empty(q)) {
        fprintf(stderr, "Error: peek from empty queue.\n");
        exit(EXIT_FAILURE);
    }

    return q->data[q->front];
}

/* =========================================================
 * Simulation Helpers
 * ========================================================= */

void admit_arrived_processes(Process processes[], int count, int current_time, Queue *ready_queue) {
    int i;

    /*
     * Scan processes and admit every process whose arrival_time <= current_time
     * and which has not already been admitted.
     * Since processes are sorted by arrival, we could optimize with a pointer,
     * but a simple scan works fine for this assignment size.
     */
    for (i = 0; i < count; i++) {
        if (!processes[i].arrived && processes[i].arrival_time <= current_time) {
            processes[i].arrived = true;
            processes[i].last_ready_time = current_time;
            enqueue(ready_queue, i);
        }
    }
}

void update_wait_times(Process processes[], int count, int running_index, int current_time) {
    int i;

    /*
     * Increment wait_time by TIME_SLICE_UNIT for each process that:
     *   - has arrived
     *   - is not completed
     *   - is not the currently running process
     */
    for (i = 0; i < count; i++) {
        if (processes[i].arrived && !processes[i].completed && i != running_index) {
            processes[i].wait_time += TIME_SLICE_UNIT;
        }
    }
}

/* select the arrived unfinished process with the shortest service_time */

    /*
     * Non-preemptive:
     * Among all arrived and unfinished processes, choose the one
     * with the smallest service_time.
     *
     * Return:
     *   index of selected process, or -1 if none available.
     */
     
int select_spn_process(const Process processes[], int count, int current_time) {
    int i;
    int best = -1;

    for (i = 0; i < count; i++) {
        if (processes[i].completed || processes[i].arrival_time > current_time)
            continue;

        if (best == -1) {
            best = i;
        } else if (processes[i].service_time < processes[best].service_time) {
            best = i;
        } else if (processes[i].service_time == processes[best].service_time) {
            /* tie-break: earlier arrival first */
            if (processes[i].arrival_time < processes[best].arrival_time) {
                best = i;
            } else if (processes[i].arrival_time == processes[best].arrival_time) {
                if (strcmp(processes[i].id, processes[best].id) < 0) {
                    best = i;
                }
            }
        }
    }

    return best;
}

/* same idea as SPN but uses remaining_time instead of service_time */

    /*
     * Preemptive:
     * Among all arrived and unfinished processes, choose the one
     * with the smallest remaining_time.
     *
     * Return:
     *   index of selected process, or -1 if none available.
     */

int select_srt_process(const Process processes[], int count, int current_time) {
    int i;
    int best = -1;

    for (i = 0; i < count; i++) {
        if (processes[i].completed || processes[i].arrival_time > current_time)
            continue;

        if (best == -1) {
            best = i;
        } else if (processes[i].remaining_time < processes[best].remaining_time) {
            best = i;
        } else if (processes[i].remaining_time == processes[best].remaining_time) {
            if (processes[i].arrival_time < processes[best].arrival_time) {
                best = i;
            } else if (processes[i].arrival_time == processes[best].arrival_time) {
                if (strcmp(processes[i].id, processes[best].id) < 0) {
                    best = i;
                }
            }
        }
    }

    return best;
}

/* pick the process with the highest response ratio: (wait + service) / service */

    /*
     * Non-preemptive:
     * Highest Response Ratio Next.
     *
     * Response ratio:
     *   (waiting_time + service_time) / service_time
     *
     * Since this is a ratio, use double.
     *
     * Return:
     *   index of selected process, or -1 if none available.
     */
     
int select_hrrn_process(const Process processes[], int count, int current_time) {
    int i;
    int best = -1;
    double best_ratio = -1.0;

    for (i = 0; i < count; i++) {
        if (processes[i].completed || processes[i].arrival_time > current_time)
            continue;

        /* wait = how long this process has been sitting around since arrival */
        double wait = (double)(current_time - processes[i].arrival_time);
        double ratio = (wait + processes[i].service_time) / (double)processes[i].service_time;

        if (best == -1 || ratio > best_ratio) {
            best = i;
            best_ratio = ratio;
        } else if (ratio == best_ratio) {
            /* tie-break: earlier arrival first */
            if (processes[i].arrival_time < processes[best].arrival_time) {
                best = i;
                best_ratio = ratio;
            } else if (processes[i].arrival_time == processes[best].arrival_time) {
                if (strcmp(processes[i].id, processes[best].id) < 0) {
                    best = i;
                    best_ratio = ratio;
                }
            }
        }
    }

    return best;
}

/* =========================================================
 * Scheduler: FCFS
 * ========================================================= */

/* non-preemptive - once a process starts it runs until done */
void run_fcfs(const Process original[], int count) {
    Process processes[MAX_PROCESSES];
    FILE *out;
    int current_time = 0;
    int running = -1;

    copy_processes(processes, original, count);
    reset_runtime_fields(processes, count);
    out = open_output_file(FCFS_OUT);

    while (!all_processes_completed(processes, count)) {
        /* if nothing running, scan for earliest arrived unfinished */
        if (running == -1) {
            int i;
            for (i = 0; i < count; i++) {
                if (!processes[i].completed && processes[i].arrival_time <= current_time) {
                    running = i;
                    break;
                }
            }
        }
        /* If none available, write "IDLE" and advance time by 10ms */
        if (running == -1) {
            write_interval(out, "IDLE");
            current_time += TIME_SLICE_UNIT;
        /* Otherwise run selected process for one 10ms interval */
        } else {
            if (processes[running].start_time == -1) {
                processes[running].start_time = current_time;
            }

            write_interval(out, processes[running].id);
            processes[running].remaining_time -= TIME_SLICE_UNIT;
            current_time += TIME_SLICE_UNIT;
        /* If remaining_time = 0 then mark completed and set the time */
            if (processes[running].remaining_time <= 0) {
                processes[running].completed = true;
                processes[running].finish_time = current_time;
                running = -1;
            }
        }
    }

    fclose(out);
}

/* =========================================================
 * Scheduler: Round Robin
 * ========================================================= */

void run_rr(const Process original[], int count, int quantum_ms, const char *output_filename) {
    Process processes[MAX_PROCESSES];
    Queue ready_queue;
    FILE *out;
    int current_time = 0;
    int running = -1;

    copy_processes(processes, original, count);
    reset_runtime_fields(processes, count);
    queue_init(&ready_queue);
    out = open_output_file(output_filename);

    while (!all_processes_completed(processes, count)) {
        /* admit new arrivals first so they go ahead of the preempted process */
        admit_arrived_processes(processes, count, current_time, &ready_queue);

        /* if current process used up its quantum, put it back in queue */
        if (running != -1 && !processes[running].completed &&
            processes[running].quantum_used >= quantum_ms) {
            enqueue(&ready_queue, running);
            running = -1;
        }

        /* grab the next process from the front of the queue */
        if (running == -1 && !queue_is_empty(&ready_queue)) {
            running = dequeue(&ready_queue);
            processes[running].quantum_used = 0;
        }

        if (running == -1) {
            write_interval(out, "IDLE");
            current_time += TIME_SLICE_UNIT;
        } else {
            if (processes[running].start_time == -1) {
                processes[running].start_time = current_time;
            }

            write_interval(out, processes[running].id);
            processes[running].remaining_time -= TIME_SLICE_UNIT;
            processes[running].quantum_used += TIME_SLICE_UNIT;
            current_time += TIME_SLICE_UNIT;

        /* check completion before quantum expiry */
            if (processes[running].remaining_time <= 0) {
                processes[running].completed = true;
                processes[running].finish_time = current_time;
                running = -1;
            }
        }
    }

    fclose(out);
}

/* =========================================================
 * Scheduler: SPN
 * ========================================================= */

/* non-preemptive - picks shortest service_time, runs to completion */
void run_spn(const Process original[], int count) {
    Process processes[MAX_PROCESSES];
    FILE *out;
    int current_time = 0;
    int running = -1;

    copy_processes(processes, original, count);
    reset_runtime_fields(processes, count);
    out = open_output_file(SPN_OUT);

    while (!all_processes_completed(processes, count)) {
        if (running == -1) {
            running = select_spn_process(processes, count, current_time);
        }

        if (running == -1) {
            write_interval(out, "IDLE");
            current_time += TIME_SLICE_UNIT;
        } else {
            if (processes[running].start_time == -1) {
                processes[running].start_time = current_time;
            }

            write_interval(out, processes[running].id);
            processes[running].remaining_time -= TIME_SLICE_UNIT;
            current_time += TIME_SLICE_UNIT;

            if (processes[running].remaining_time <= 0) {
                processes[running].completed = true;
                processes[running].finish_time = current_time;
                running = -1;
            }
        }
    }

    fclose(out);
}

/* =========================================================
 * Scheduler: SRT
 * ========================================================= */

/* preemptive - re-evaluates every tick, switches only if strictly shorter */
void run_srt(const Process original[], int count) {
    Process processes[MAX_PROCESSES];
    FILE *out;
    int current_time = 0;
    int running = -1;

    copy_processes(processes, original, count);
    reset_runtime_fields(processes, count);
    out = open_output_file(SRT_OUT);

    while (!all_processes_completed(processes, count)) {
        int best = select_srt_process(processes, count, current_time);

        /* only preempt if strictly shorter, keep current on ties */
        if (running != -1 && !processes[running].completed && best != -1) {
            if (processes[best].remaining_time < processes[running].remaining_time) {
                running = best;
            }
        } else {
            running = best;
        }

        if (running == -1) {
            write_interval(out, "IDLE");
            current_time += TIME_SLICE_UNIT;
        } else {
            if (processes[running].start_time == -1) {
                processes[running].start_time = current_time;
            }

            write_interval(out, processes[running].id);
            processes[running].remaining_time -= TIME_SLICE_UNIT;
            current_time += TIME_SLICE_UNIT;

            if (processes[running].remaining_time <= 0) {
                processes[running].completed = true;
                processes[running].finish_time = current_time;
                running = -1;
            }
        }
    }

    fclose(out);
}

/* =========================================================
 * Scheduler: HRRN
 * ========================================================= */

/* non-preemptive - highest response ratio gets the CPU when it's free */
void run_hrrn(const Process original[], int count) {
    Process processes[MAX_PROCESSES];
    FILE *out;
    int current_time = 0;
    int running = -1;

    copy_processes(processes, original, count);
    reset_runtime_fields(processes, count);
    out = open_output_file(HRRN_OUT);

    while (!all_processes_completed(processes, count)) {
        if (running == -1) {
            running = select_hrrn_process(processes, count, current_time);
        }

        if (running == -1) {
            write_interval(out, "IDLE");
            current_time += TIME_SLICE_UNIT;
        } else {
            if (processes[running].start_time == -1) {
                processes[running].start_time = current_time;
            }

            write_interval(out, processes[running].id);
            processes[running].remaining_time -= TIME_SLICE_UNIT;
            current_time += TIME_SLICE_UNIT;

            if (processes[running].remaining_time <= 0) {
                processes[running].completed = true;
                processes[running].finish_time = current_time;
                running = -1;
            }
        }
    }

    fclose(out);
}

/* =========================================================
 * Scheduler: Feedback
 * ========================================================= */

/* multilevel feedback queue - 128 levels, 10ms quantum, lower index = higher priority */
void run_feedback(const Process original[], int count) {
    Process processes[MAX_PROCESSES];
    FILE *out;
    int current_time = 0;
    int running = -1;

    copy_processes(processes, original, count);
    reset_runtime_fields(processes, count);
    out = open_output_file(FEEDBACK_OUT);

    int max_levels = 128;
    Queue queues[128];
    int lv;
    int quantum_ms = TIME_SLICE_UNIT;

    for (lv = 0; lv < max_levels; lv++) {
        queue_init(&queues[lv]);
    }

    while (!all_processes_completed(processes, count)) {
        int i;

        /* admit new arrivals into the highest priority queue (level 0) */
        for (i = 0; i < count; i++) {
            if (!processes[i].arrived && processes[i].arrival_time <= current_time) {
                processes[i].arrived = true;
                processes[i].level = 0;
                enqueue(&queues[0], i);
            }
        }

        /* check if a higher-priority arrival should preempt the running process */
        if (running != -1 && !processes[running].completed) {
            int highest = -1;
            for (lv = 0; lv < max_levels; lv++) {
                if (!queue_is_empty(&queues[lv])) {
                    highest = lv;
                    break;
                }
            }

            /* preempted process goes back to its SAME level - no demotion */
            if (highest != -1 && highest < processes[running].level) {
                enqueue(&queues[processes[running].level], running);
                processes[running].quantum_used = 0;
                running = -1;
            }
        }

        /* pick from the highest priority non-empty queue */
        if (running == -1) {
            for (lv = 0; lv < max_levels; lv++) {
                if (!queue_is_empty(&queues[lv])) {
                    running = dequeue(&queues[lv]);
                    processes[running].quantum_used = 0;
                    break;
                }
            }
        }

        if (running == -1) {
            write_interval(out, "IDLE");
            current_time += TIME_SLICE_UNIT;
        } else {
            if (processes[running].start_time == -1) {
                processes[running].start_time = current_time;
            }

            write_interval(out, processes[running].id);
            processes[running].remaining_time -= TIME_SLICE_UNIT;
            processes[running].quantum_used += TIME_SLICE_UNIT;
            current_time += TIME_SLICE_UNIT;

            if (processes[running].remaining_time <= 0) {
                processes[running].completed = true;
                processes[running].finish_time = current_time;
                running = -1;
            }
            /* quantum used up - demote one level and put at back of that queue */
            else if (processes[running].quantum_used >= quantum_ms) {
                processes[running].level++;
                enqueue(&queues[processes[running].level], running);
                running = -1;
            }
        }
    }

    fclose(out);
}
