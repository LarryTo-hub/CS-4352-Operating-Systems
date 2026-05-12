/*
=============================================================================
Title        : assignment3.c
Description  : CS4352 Assignment 3 - HTTP API client program using libcurl.
               Connects to a locally hosted API server, creates a session,
               stores two key-value pairs, retrieves them, swaps them, then
               retrieves them again. Outputs name, R#, and 4 integer values
               in the required order.
Author       : Larry To (R11615587)
Date         : 03/14/2026
Version      : 1.0
Usage        : ./assignment_3 <port>
               Example: ./assignment_3 5000
Notes        : Requires libcurl to be available. Compile on HPCC with:
                   module load gcc/14.2.0
                   make
C Version    : C17 (gnu17)
=============================================================================
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

/* response buffer size - the API responses are short so 1024 is plenty */
#define RESP_BUF_SIZE 1024

/* session ID shouldn't be super long but giving it extra room just in case */
#define SID_BUF_SIZE  256


/*
 * write_callback - called by libcurl whenever response data arrives.
 * Appends each chunk into the buffer pointed to by userp.
 * Returns the number of bytes handled (required by libcurl).
 */
size_t write_callback(void *data, size_t size, size_t nmemb, void *userp) {
    size_t total_bytes = size * nmemb;
    char *buf = (char *)userp;

    /* strncat appends incoming data - buffer must be zeroed before first call */
    strncat(buf, (char *)data, total_bytes);

    return total_bytes;
}


/*
 * create_session - sends POST /session and extracts the session ID.
 *
 * Parameters:
 *   base_url  - e.g. "http://127.0.0.1:5000"
 *   sid_out   - buffer to write the session ID string into
 *
 * Returns 0 on success, -1 on any error.
 */
int create_session(const char *base_url, char *sid_out) {
    CURL *curl;
    CURLcode res;
    char response[RESP_BUF_SIZE];
    char url[512];

    memset(response, 0, sizeof(response));
    snprintf(url, sizeof(url), "%s/session", base_url);

    curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "curl_easy_init() failed in create_session\n");
        return -1;
    }

    /* POST /session with no body - spec says request body is None */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);  /* no progress meter on stdout */

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "POST /session curl error: %s\n", curl_easy_strerror(res));
        curl_easy_cleanup(curl);
        return -1;
    }

    /* check that the server actually returned 200 */
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) {
        fprintf(stderr, "POST /session returned HTTP %ld\n", http_code);
        curl_easy_cleanup(curl);
        return -1;
    }

    curl_easy_cleanup(curl);

    /* response is "SID=<random_string>" - find the = and grab everything after */
    char *eq_ptr = strchr(response, '=');
    if (eq_ptr == NULL) {
        fprintf(stderr, "Could not find '=' in session response: %s\n", response);
        return -1;
    }

    /* copy value after '=' into sid_out, then trim any trailing newline */
    strncpy(sid_out, eq_ptr + 1, SID_BUF_SIZE - 1);
    sid_out[SID_BUF_SIZE - 1] = '\0';
    sid_out[strcspn(sid_out, "\r\n")] = '\0';

    return 0;
}


/*
 * put_kv - sends PUT /kv with a given body and the X-Session header.
 *
 * Parameters:
 *   base_url  - base URL string
 *   sid       - session ID string
 *   body      - request body, e.g. "key=initialize;value=3360"
 *
 * Returns 0 on success, -1 on error.
 */
int put_kv(const char *base_url, const char *sid, const char *body) {
    CURL *curl;
    CURLcode res;
    char response[RESP_BUF_SIZE];
    char url[512];
    char session_header[SID_BUF_SIZE + 32];

    memset(response, 0, sizeof(response));
    snprintf(url, sizeof(url), "%s/kv", base_url);

    /* build the X-Session header string */
    snprintf(session_header, sizeof(session_header), "X-Session: %s", sid);

    curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "curl_easy_init() failed in put_kv\n");
        return -1;
    }

    /* add the required headers */
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, session_header);
    headers = curl_slist_append(headers, "Content-Type: text/plain");

    /* for PUT, use CURLOPT_CUSTOMREQUEST */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "PUT /kv curl error: %s\n", curl_easy_strerror(res));
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return -1;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) {
        fprintf(stderr, "PUT /kv returned HTTP %ld\n", http_code);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return -1;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return 0;
}


/*
 * get_kv - sends GET /kv/<key> and returns the stored integer value.
 *
 * Parameters:
 *   base_url  - base URL string
 *   sid       - session ID string
 *   key       - the key name to retrieve, e.g. "initialize" or "modify"
 *   out_value - pointer where the parsed integer will be written
 *
 * Returns 0 on success, -1 on error.
 * The parsed value is written into *out_value.
 */
int get_kv(const char *base_url, const char *sid, const char *key, int *out_value) {
    CURL *curl;
    CURLcode res;
    char response[RESP_BUF_SIZE];
    char url[512];
    char session_header[SID_BUF_SIZE + 32];

    memset(response, 0, sizeof(response));
    snprintf(url, sizeof(url), "%s/kv/%s", base_url, key);
    snprintf(session_header, sizeof(session_header), "X-Session: %s", sid);

    curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "curl_easy_init() failed in get_kv\n");
        return -1;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, session_header);

    /* regular GET - no body, no CUSTOMREQUEST override needed */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "GET /kv/%s curl error: %s\n", key, curl_easy_strerror(res));
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return -1;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) {
        fprintf(stderr, "GET /kv/%s returned HTTP %ld\n", key, http_code);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return -1;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    /* response format: "key=initialize;value=3360"
       find the semicolon first, then the '=' sign in the value part */
    char *semi = strchr(response, ';');
    if (semi == NULL) {
        fprintf(stderr, "No semicolon found in GET /kv/%s response: %s\n", key, response);
        return -1;
    }

    char *eq_ptr = strchr(semi, '=');
    if (eq_ptr == NULL) {
        fprintf(stderr, "No '=' found in value part of GET /kv/%s response\n", key);
        return -1;
    }

    /* atoi okay here since we know the server gives us a valid integer string */
    *out_value = atoi(eq_ptr + 1);
    return 0;
}


/*
 * swap_values - sends POST /swap with body key1=initialize;key2=modify.
 *
 * Parameters:
 *   base_url  - base URL string
 *   sid       - session ID string
 *
 * Returns 0 on success, -1 on error.
 */
int swap_values(const char *base_url, const char *sid) {
    CURL *curl;
    CURLcode res;
    char response[RESP_BUF_SIZE];
    char url[512];
    char session_header[SID_BUF_SIZE + 32];

    memset(response, 0, sizeof(response));
    snprintf(url, sizeof(url), "%s/swap", base_url);
    snprintf(session_header, sizeof(session_header), "X-Session: %s", sid);

    curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "curl_easy_init() failed in swap_values\n");
        return -1;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, session_header);
    headers = curl_slist_append(headers, "Content-Type: text/plain");

    /* POST /swap - body is key1=initialize;key2=modify, no CUSTOMREQUEST needed */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "key1=initialize;key2=modify");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "POST /swap curl error: %s\n", curl_easy_strerror(res));
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return -1;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) {
        fprintf(stderr, "POST /swap returned HTTP %ld\n", http_code);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return -1;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return 0;
}


/* 
   main - drives the 10-step sequence required
 */
int main(int argc, char *argv[]) {

    /* must receive port number as argv[1] */
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    /* build the base URL using the port from the command line */
    char base_url[256];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%s", argv[1]);

    /* steps 1 & 2: print name and R# to stdout */
    printf("Larry To\n");
    printf("R11615587\n");

    /* set up the session ID buffer */
    char session_id[SID_BUF_SIZE];
    memset(session_id, 0, sizeof(session_id));

    /* step 3: POST /session - get our session ID */
    if (create_session(base_url, session_id) != 0) {
        fprintf(stderr, "create_session failed, exiting\n");
        return 1;
    }

    /* step 4: PUT /kv - store initialize=3360 */
    if (put_kv(base_url, session_id, "key=initialize;value=3360") != 0) {
        fprintf(stderr, "put_kv(initialize) failed, exiting\n");
        return 1;
    }

    /* step 5: PUT /kv - store modify=4 */
    if (put_kv(base_url, session_id, "key=modify;value=4") != 0) {
        fprintf(stderr, "put_kv(modify) failed, exiting\n");
        return 1;
    }

    /* step 6: GET /kv/initialize and print the value */
    int val_init = 0;
    if (get_kv(base_url, session_id, "initialize", &val_init) != 0) {
        fprintf(stderr, "get_kv(initialize) failed, exiting\n");
        return 1;
    }
    printf("%d\n", val_init);

    /* step 7: GET /kv/modify and print the value */
    int val_mod = 0;
    if (get_kv(base_url, session_id, "modify", &val_mod) != 0) {
        fprintf(stderr, "get_kv(modify) failed, exiting\n");
        return 1;
    }
    printf("%d\n", val_mod);

    /* step 8: POST /swap - swaps the two values */
    if (swap_values(base_url, session_id) != 0) {
        fprintf(stderr, "swap_values failed, exiting\n");
        return 1;
    }

    /* step 9: GET /kv/initialize again - should now be 4 after swap */
    int val_init2 = 0;
    if (get_kv(base_url, session_id, "initialize", &val_init2) != 0) {
        fprintf(stderr, "get_kv(initialize) post-swap failed, exiting\n");
        return 1;
    }
    printf("%d\n", val_init2);

    /* step 10: GET /kv/modify again - should now be 3360 after swap */
    int val_mod2 = 0;
    if (get_kv(base_url, session_id, "modify", &val_mod2) != 0) {
        fprintf(stderr, "get_kv(modify) post-swap failed, exiting\n");
        return 1;
    }
    printf("%d\n", val_mod2);

    return 0;
}
