/*
=============================================================================
Title       : CS 4352 - Final Project: Elevator OS Scheduler
Description : Talks to the Python/Flask Elevator OS over HTTP and decides
              which elevator each new person should ride.
              I used three threads:
                1. Input thread  - polls GET /NextInput for new people
                2. Scheduler thread - picks an elevator for each person
                3. Output thread - sends PUT /AddPersonToElevator
Author      : Larry To (R11615587)
Date        : 05/05/2026
Version     : 1.1
C Version   : C17 (gnu17)
=============================================================================
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <curl/curl.h>

#define BAY_NAME_LEN    32
#define PERSON_ID_LEN   32
#define MAX_ELEVATORS   64
#define RESP_BUF_SIZE   1024
#define URL_BUF_SIZE    512

/* ring buffer size */
#define QUEUE_SIZE 2048

/* poll sleeps in microseconds */
#define SLEEP_NONE 50000   /* 50ms */
#define SLEEP_BUSY 1000

/* debugging */
#define DEBUG 0

/* one elevator from the bldg file */
typedef struct {
    char bay[BAY_NAME_LEN];
    int  lowest;
    int  highest;
    int  current_floor;   /* updated as we assign people so estimate stays fresh */
    int  capacity;
} Elevator;

typedef struct {
    char person_id[PERSON_ID_LEN];
    int  start_floor;
    int  end_floor;
} Person;

typedef struct {
    char person_id[PERSON_ID_LEN];
    char bay[BAY_NAME_LEN];
} Assignment;

Elevator g_elevators[MAX_ELEVATORS];
int      g_num_elevators = 0;

char g_base_url[256];

/* set to 1 when threads should drain and exit */
volatile int g_shutdown = 0;

/* input queue: input thread -> scheduler */
Person          in_buf[QUEUE_SIZE];
int             in_head = 0, in_tail = 0, in_count = 0;
pthread_mutex_t in_mutex     = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  in_not_full  = PTHREAD_COND_INITIALIZER;
pthread_cond_t  in_not_empty = PTHREAD_COND_INITIALIZER;

/* output queue: scheduler -> output thread */
Assignment      out_buf[QUEUE_SIZE];
int             out_head = 0, out_tail = 0, out_count = 0;
pthread_mutex_t out_mutex     = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  out_not_full  = PTHREAD_COND_INITIALIZER;
pthread_cond_t  out_not_empty = PTHREAD_COND_INITIALIZER;

/* count of people assigned to each elevator and estimated next free floor.
   assigned_mutex covers both arrays so we don't need two locks */
int assigned_count[MAX_ELEVATORS];
int estimated_floor[MAX_ELEVATORS];   /* where we think the elevator will be next */
pthread_mutex_t assigned_mutex = PTHREAD_MUTEX_INITIALIZER;

/* libcurl write callback */
size_t write_callback(void *data, size_t size, size_t nmemb, void *userp) {
    size_t total_bytes = size * nmemb;
    char  *buf         = (char *)userp;

    /* don't overflow - leave a byte for the terminator */
    size_t cur_len  = strlen(buf);
    size_t avail    = RESP_BUF_SIZE - cur_len - 1;
    size_t to_copy  = (total_bytes < avail) ? total_bytes : avail;

    memcpy(buf + cur_len, data, to_copy);
    buf[cur_len + to_copy] = '\0';
    return total_bytes;

    /* tried strncat first like in assignment 3 but it acted weird with the bigger
       responses here so I switched to memcpy */
}

long http_request(const char *method, const char *url, char *resp_out) {
    CURL     *curl;
    CURLcode  res;
    long      http_code = 0;

    resp_out[0] = '\0';

    curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "curl_easy_init failed\n");
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     resp_out);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS,    1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       10L);

    if (strcmp(method, "PUT") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
    } else {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "curl error on %s %s: %s\n",
                method, url, curl_easy_strerror(res));
        curl_easy_cleanup(curl);
        return -1;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    return http_code;
}

/* push a person onto the input queue, blocks if full */
void in_push(const Person *p) {
    pthread_mutex_lock(&in_mutex);
    while (in_count == QUEUE_SIZE && !g_shutdown) {
        pthread_cond_wait(&in_not_full, &in_mutex);
    }

    if (g_shutdown) {
        pthread_mutex_unlock(&in_mutex);
        return;
    }

    in_buf[in_tail] = *p;
    in_tail         = (in_tail + 1) % QUEUE_SIZE;
    in_count++;
    pthread_cond_signal(&in_not_empty);
    pthread_mutex_unlock(&in_mutex);
}

int in_pop(Person *out) {
    pthread_mutex_lock(&in_mutex);

    while (in_count == 0 && !g_shutdown) {
        pthread_cond_wait(&in_not_empty, &in_mutex);
    }

    if (in_count == 0 && g_shutdown) {
        pthread_mutex_unlock(&in_mutex);
        return 1;
    }

    *out     = in_buf[in_head];
    in_head  = (in_head + 1) % QUEUE_SIZE;
    in_count--;

    pthread_cond_signal(&in_not_full);
    pthread_mutex_unlock(&in_mutex);
    return 0;
}

void out_push(const Assignment *a) {
    pthread_mutex_lock(&out_mutex);
    while (out_count == QUEUE_SIZE && !g_shutdown) {
        pthread_cond_wait(&out_not_full, &out_mutex);
    }

    if (g_shutdown) {
        pthread_mutex_unlock(&out_mutex);
        return;
    }

    out_buf[out_tail] = *a;
    out_tail          = (out_tail + 1) % QUEUE_SIZE;
    out_count++;
    pthread_cond_signal(&out_not_empty);
    pthread_mutex_unlock(&out_mutex);
}

int out_pop(Assignment *out) {
    pthread_mutex_lock(&out_mutex);
    while (out_count == 0 && !g_shutdown) {
        pthread_cond_wait(&out_not_empty, &out_mutex);
    }

    if (out_count == 0 && g_shutdown) {
        pthread_mutex_unlock(&out_mutex);
        return 1;
    }

    *out      = out_buf[out_head];
    out_head  = (out_head + 1) % QUEUE_SIZE;
    out_count--;
    pthread_cond_signal(&out_not_full);
    pthread_mutex_unlock(&out_mutex);
    return 0;
}

/* wake every thread sleeping on a condvar so we can shut down. */
/* without this the scheduler/output threads would hang forever on join */
void wake_all(void) {
    pthread_mutex_lock(&in_mutex);
    pthread_cond_broadcast(&in_not_full);
    pthread_cond_broadcast(&in_not_empty);
    pthread_mutex_unlock(&in_mutex);

    pthread_mutex_lock(&out_mutex);
    pthread_cond_broadcast(&out_not_full);
    pthread_cond_broadcast(&out_not_empty);
    pthread_mutex_unlock(&out_mutex);
}

/* read the bldg file. each line: bay\tlowest\thighest\tcurrent\tcapacity */
int read_building_file(const char *path) {
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, "Error: cannot open building file '%s'\n", path);
        return -1;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp) != NULL) {

        /* skip blank lines */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\n' || *p == '\0') continue;

        if (g_num_elevators >= MAX_ELEVATORS) {
            fprintf(stderr, "Error: too many elevators (max %d)\n", MAX_ELEVATORS);
            fclose(fp);
            return -1;
        }

        line[strcspn(line, "\r\n")] = '\0';

        char *saveptr = NULL;
        char *bay     = strtok_r(line, "\t", &saveptr);
        char *lowest  = strtok_r(NULL, "\t", &saveptr);
        char *highest = strtok_r(NULL, "\t", &saveptr);
        char *current = strtok_r(NULL, "\t", &saveptr);
        char *cap     = strtok_r(NULL, "\t", &saveptr);

        if (!bay || !lowest || !highest || !current || !cap) {
            fprintf(stderr, "Error: bad line in bldg file: '%s'\n", line);
            fclose(fp);
            return -1;
        }

        Elevator *e = &g_elevators[g_num_elevators];
        strncpy(e->bay, bay, BAY_NAME_LEN - 1);
        e->bay[BAY_NAME_LEN - 1] = '\0';

        e->lowest        = atoi(lowest);
        e->highest       = atoi(highest);
        e->current_floor = atoi(current);
        e->capacity      = atoi(cap);

        /* seed the estimated floor to wherever the elevator starts */
        estimated_floor[g_num_elevators] = e->current_floor;

        g_num_elevators++;
    }

    fclose(fp);

    if (g_num_elevators == 0) {
        fprintf(stderr, "Error: no elevators in '%s'\n", path);
        return -1;
    }

    return 0;
}

int api_start_simulation(void) {
    char url[URL_BUF_SIZE];
    char resp[RESP_BUF_SIZE];

    snprintf(url, sizeof(url), "%s/Simulation/start", g_base_url);
    long code = http_request("PUT", url, resp);
    // 202 = started, 200 = already running
    if (code == 202 || code == 200) {
        printf("Simulation start: %ld %s\n", code, resp);
        return 0;
    }

    fprintf(stderr, "Simulation start failed: code=%ld body=%s\n", code, resp);
    return -1;
}

int api_simulation_complete(void) {
    char url[URL_BUF_SIZE];
    char resp[RESP_BUF_SIZE];

    snprintf(url, sizeof(url), "%s/Simulation/check", g_base_url);
    long code = http_request("GET", url, resp);

    if (code != 200) return 0;

    if (strstr(resp, "complete") != NULL) return 1;
    if (strstr(resp, "stopped")  != NULL) return 1;
    return 0;
}

/* turn P1 | 3 || 10 into a Person. */
/* returns 0 on success, -1 if it's NONE or just bad */
int parse_person_line(const char *body, Person *out) {
    if (body == NULL || body[0] == '\0') return -1;
    if (strstr(body, "NONE") != NULL)    return -1;

    char tmp[RESP_BUF_SIZE];
    strncpy(tmp, body, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char *saveptr = NULL;
    char *id      = strtok_r(tmp, "|", &saveptr);
    char *start_s = strtok_r(NULL, "|", &saveptr);
    char *end_s   = strtok_r(NULL, "|", &saveptr);

    if (!id || !start_s || !end_s) return -1;

    /* my first ttry used sscanf with "%s | %d | %d" but the spaces around
       the pipes kept eating the wrong things, so strtok_r it is */

    /* trim whitespace around the id */
    while (*id == ' ' || *id == '\t') id++;
    char *q = id + strlen(id) - 1;
    while (q >= id && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) {
        *q = '\0';
        q--;
    }

    strncpy(out->person_id, id, PERSON_ID_LEN - 1);
    out->person_id[PERSON_ID_LEN - 1] = '\0';
    out->start_floor = atoi(start_s);
    out->end_floor   = atoi(end_s);

    if (out->person_id[0] == '\0') return -1;
    return 0;
}

/* find the closest elevator that can serve both floors, prefer less loaded
   on ties. update estimated position after so it don't pile onto one bay */
int choose_elevator(const Person *p) {
    int best_idx  = -1;
    int best_dist = 0;
    int best_load = 0;

    pthread_mutex_lock(&assigned_mutex);
    for (int i = 0; i < g_num_elevators; i++) {
        Elevator *e = &g_elevators[i];

        if (p->start_floor < e->lowest  || p->start_floor > e->highest) continue;
        if (p->end_floor   < e->lowest  || p->end_floor   > e->highest) continue;

        /* score against estimated_floor instead of current_floor so a busy
           elevator that's already halfway across the building doesnt look
           like a good pick just because it started nearby */
        int dist = estimated_floor[i] - p->start_floor;
        if (dist < 0) dist = -dist;
        int load = assigned_count[i];

        if (best_idx == -1 ||
            dist < best_dist ||
            (dist == best_dist && load < best_load)) {
            best_idx  = i;
            best_dist = dist;
            best_load = load;
        }
    }

    if (best_idx != -1) {
        assigned_count[best_idx]++;
        /* move estimated position to the end floor of this trip so
           subsequent assignments don't assume the elevator is still here */
        estimated_floor[best_idx] = p->end_floor;
    }

    pthread_mutex_unlock(&assigned_mutex);

    return best_idx;
}

/* input thread - hammers /NextInput, parses, and pushes onto the input queue.
   every 10 polls it checks /Simulation/check to see if the sim is over */
void* input_thread(void* arg) {
    (void)arg;

    char url[URL_BUF_SIZE];
    char resp[RESP_BUF_SIZE];
    snprintf(url, sizeof(url), "%s/NextInput", g_base_url);

    int check_counter = 0;

    while (!g_shutdown) {
        long code = http_request("GET", url, resp);

        if (code == 200) {
            Person p;
            if (parse_person_line(resp, &p) == 0) {
#if DEBUG
                printf("[input] got %s %d->%d\n", p.person_id, p.start_floor, p.end_floor);
#endif
                in_push(&p);
                usleep(SLEEP_BUSY);
            } else {
                /* NONE response, sleep longer */
                usleep(SLEEP_NONE);
            }

        } else {
            usleep(SLEEP_NONE);
        }

        // first version checked /Simulation/check after every poll, that was
        // way too many requests. ten polls per check seems to work fine.
        check_counter++;
        if (check_counter >= 10) {
            check_counter = 0;
            if (api_simulation_complete()) {
                printf("Input thread: simulation complete - shutting down.\n");
                g_shutdown = 1;
                wake_all();
                break;
            }
        }
    }

    return NULL;
}

void* scheduler_thread(void* arg) {
    (void)arg;

    while (1) {
        Person p;
        if (in_pop(&p) != 0) break;

        int idx = choose_elevator(&p);
        if (idx < 0) {
            /* shouldn't happen for a valid bldg file but so dont crash */
            fprintf(stderr,
                    "Scheduler: no elevator for person %s (%d -> %d)\n",
                    p.person_id, p.start_floor, p.end_floor);
            continue;
        }

        Assignment a;
        memset(&a, 0, sizeof(a));
        snprintf(a.person_id, PERSON_ID_LEN, "%s", p.person_id);
        snprintf(a.bay,       BAY_NAME_LEN,  "%s", g_elevators[idx].bay);

#if DEBUG
        printf("[sched] %s -> %s\n", a.person_id, a.bay);
#endif

        out_push(&a);
    }

    return NULL;
}

void* output_thread(void* arg) {
    (void)arg;

    while (1) {
        Assignment a;
        if (out_pop(&a) != 0) break;

        char url[URL_BUF_SIZE];
        char resp[RESP_BUF_SIZE];
        snprintf(url, sizeof(url),
                 "%s/AddPersonToElevator/%s/%s",
                 g_base_url, a.person_id, a.bay);

        long code = http_request("PUT", url, resp);
        if (code != 200) {
            fprintf(stderr,
                    "Assign person=%s bay=%s failed: code=%ld body=%s\n",
                    a.person_id, a.bay, code, resp);
        }
    }

    return NULL;
}

int main(int argc, char *argv[]) {

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <building_file> <port>\n",
                argv[0]);
        return 1;
    }

    const char *bldg_path = argv[1];
    const char *port_str  = argv[2];

    int port = atoi(port_str);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Error: invalid port number '%s'\n", port_str);
        return 1;
    }

    /* base URL from the port */
    snprintf(g_base_url, sizeof(g_base_url), "http://127.0.0.1:%d", port);

    if (read_building_file(bldg_path) != 0) {
        return 1;
    }

    printf("Loaded %d elevators from %s\n", g_num_elevators, bldg_path);
    for (int i = 0; i < g_num_elevators; i++) {
        printf("  bay=%s low=%d high=%d cur=%d cap=%d\n",
               g_elevators[i].bay,
               g_elevators[i].lowest,
               g_elevators[i].highest,
               g_elevators[i].current_floor,
               g_elevators[i].capacity);
    }

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        fprintf(stderr, "Error: curl_global_init failed\n");
        return 1;
    }

    if (api_start_simulation() != 0) {
        curl_global_cleanup();
        return 1;
    }

    pthread_t in_tid, sched_tid, out_tid;

    if (pthread_create(&in_tid,    NULL, input_thread,     NULL) != 0 ||
        pthread_create(&sched_tid, NULL, scheduler_thread, NULL) != 0 ||
        pthread_create(&out_tid,   NULL, output_thread,    NULL) != 0) {
        fprintf(stderr, "Error: failed to create one or more threads\n");
        g_shutdown = 1;
        wake_all();
        curl_global_cleanup();
        return 1;
    }

    /* join input thread first since it controls the shutdown flag */
    pthread_join(in_tid, NULL);

    wake_all();
    pthread_join(sched_tid, NULL);
    pthread_join(out_tid,   NULL);

    pthread_mutex_destroy(&in_mutex);
    pthread_mutex_destroy(&out_mutex);
    pthread_mutex_destroy(&assigned_mutex);
    pthread_cond_destroy(&in_not_full);
    pthread_cond_destroy(&in_not_empty);
    pthread_cond_destroy(&out_not_full);
    pthread_cond_destroy(&out_not_empty);

    curl_global_cleanup();

    printf("scheduler_os exiting cleanly.\n");
    return 0;
}
