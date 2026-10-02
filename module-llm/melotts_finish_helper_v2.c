/*
 * melotts_finish_helper_v2.c
 *
 * Fixes a real design flaw found in v1: melotts, chained to a
 * streaming LLM, plays each SENTENCE as a separate chunk - meaning
 * queue_status genuinely, momentarily reports idle BETWEEN sentences
 * too, not just at the true end of a response. v1 treated every such
 * transition as "done", which could in principle cut her off
 * mid-response.
 *
 * Fix: also subscribe to the LLM's own real-time stream (same
 * mechanism validated in test_full_discovery.c: sys -> sql_select ->
 * real out_port -> direct subscribe). Only treat a queue_status
 * busy->idle transition as GENUINE completion once the LLM has
 * already reported finish:true for the current turn - before that,
 * an idle reading just means "between sentences, more is coming".
 *
 * Compile directly on Module LLM:
 *   gcc melotts_finish_helper_v2.c -lzmq -o melotts_finish_helper_v2
 * Run in the background alongside the real app:
 *   ./melotts_finish_helper_v2 &
 */

#include <zmq.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define POLL_INTERVAL_MS 150
#define REDISCOVER_EVERY_N_POLLS 20
/* After llm reports finish:true, the audio queue must STAY empty this long before we declare
 * playback complete. Without it we fired on the first moment the queue drained - but the
 * last sentence is often still being synthesized then (confirmed in logs: two "Genuine
 * completion" lines for one turn, the first ~1s after llm finished while ~5s of speech
 * were still to come). Audio starting again inside this window cancels the pending
 * completion. Tune up if Becky is still cut off, down if follow-ups feel slow. */
#define SETTLE_MS 3500

/* --- discover a unit's work_id via plain TCP taskinfo --- */
static int discover_work_id(const char *unit_name, char *out_work_id, size_t out_size) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(10001);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    struct timeval tv = {2, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sock);
        return -1;
    }

    char req[256];
    snprintf(req, sizeof(req), "{\"request_id\":\"helper\",\"work_id\":\"%s\",\"action\":\"taskinfo\"}\n", unit_name);
    send(sock, req, strlen(req), 0);

    char buf[2048];
    int received = recv(sock, buf, sizeof(buf) - 1, 0);
    close(sock);
    if (received <= 0) return -1;
    buf[received] = '\0';

    char prefix[64];
    snprintf(prefix, sizeof(prefix), "\"%s.", unit_name);
    char *p = strstr(buf, prefix);
    if (!p) return -1;
    p += 1;
    char *end = strchr(p, '"');
    if (!end) return -1;
    size_t len = end - p;
    if (len >= out_size) len = out_size - 1;

    /* Validate: the part after "<unit_name>." must be genuinely
     * numeric, not just "whatever text happened to appear before the
     * next quote". Confirmed directly: without this check, a
     * transitional response during an app restart was accepted as a
     * real work_id ("melotts.tasklist"), which happened to be harmless
     * only by luck - it simply never matched CoreS3's real work_id, so
     * any message sent under it was silently ignored rather than acted
     * on incorrectly. */
    size_t unit_name_len = strlen(unit_name);
    if (len <= unit_name_len + 1) return -1;  /* must have at least one digit after "name." */
    char *numeric_part = p + unit_name_len + 1;
    size_t numeric_len = len - unit_name_len - 1;
    for (size_t i = 0; i < numeric_len; i++) {
        if (numeric_part[i] < '0' || numeric_part[i] > '9') return -1;
    }

    memcpy(out_work_id, p, len);
    out_work_id[len] = '\0';
    return 0;
}

/* --- discover a unit's real output_url via sys/sql_select over RPC --- */
static int discover_output_url(const char *work_id, char *out_url, size_t out_size) {
    void *ctx = zmq_ctx_new();
    void *sock = zmq_socket(ctx, ZMQ_REQ);
    int timeout_ms = 3000;
    zmq_setsockopt(sock, ZMQ_SNDTIMEO, &timeout_ms, sizeof(timeout_ms));
    zmq_setsockopt(sock, ZMQ_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));

    if (zmq_connect(sock, "ipc:///tmp/rpc.sys") != 0) {
        zmq_close(sock);
        zmq_ctx_destroy(ctx);
        return -1;
    }

    char key[256];
    snprintf(key, sizeof(key), "%s.out_port", work_id);
    const char *action = "sql_select";
    zmq_send(sock, action, strlen(action), ZMQ_SNDMORE);
    zmq_send(sock, key, strlen(key), 0);

    char buf[512];
    int received = zmq_recv(sock, buf, sizeof(buf) - 1, 0);
    zmq_close(sock);
    zmq_ctx_destroy(ctx);
    if (received <= 0) return -1;
    buf[received] = '\0';

    if ((size_t)received >= out_size) received = out_size - 1;
    memcpy(out_url, buf, received);
    out_url[received] = '\0';
    return 0;
}

/* Pull "index" and "delta" out of one llm stream message (diagnostic only - naive on purpose). */
static void parse_chunk(const char *msg, int *index, char *delta, size_t delta_cap) {
    *index = -1;
    delta[0] = '\0';
    const char *p = strstr(msg, "\"index\":");
    if (p) *index = atoi(p + strlen("\"index\":"));
    p = strstr(msg, "\"delta\":\"");
    if (p) {
        p += strlen("\"delta\":\"");
        size_t n = 0;
        while (*p && !(*p == '"' && p[-1] != '\\') && n + 1 < delta_cap) delta[n++] = *p++;
        delta[n] = '\0';
    }
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int query_queue_status(void *ctx, int *pending, int *running) {
    void *sock = zmq_socket(ctx, ZMQ_REQ);
    int timeout_ms = 1000;
    zmq_setsockopt(sock, ZMQ_SNDTIMEO, &timeout_ms, sizeof(timeout_ms));
    zmq_setsockopt(sock, ZMQ_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));

    if (zmq_connect(sock, "ipc:///tmp/rpc.audio") != 0) {
        zmq_close(sock);
        return -1;
    }
    const char *action = "queue_status";
    zmq_send(sock, action, strlen(action), ZMQ_SNDMORE);
    zmq_send(sock, "", 0, 0);

    char buf[512];
    int received = zmq_recv(sock, buf, sizeof(buf) - 1, 0);
    zmq_close(sock);
    if (received == -1) return -1;
    buf[received] = '\0';

    char *p = strstr(buf, "\"pending\":");
    if (p) *pending = atoi(p + strlen("\"pending\":"));
    else *pending = -1;

    p = strstr(buf, "\"running\":");
    if (p) *running = (strncmp(p + strlen("\"running\":"), "true", 4) == 0);
    else *running = -1;

    return 0;
}

static void inject_finish_message(void *ctx, const char *work_id) {
    void *sock = zmq_socket(ctx, ZMQ_PUSH);
    if (zmq_connect(sock, "ipc:///tmp/llm/5556.sock") != 0) {
        printf("inject: connect failed: %s\n", zmq_strerror(zmq_errno()));
        zmq_close(sock);
        return;
    }
    char msg[512];
    snprintf(msg, sizeof(msg),
        "{\"created\":%ld,\"data\":{\"delta\":\"\",\"finish\":true,\"index\":0},"
        "\"error\":{\"code\":0,\"message\":\"\"},\"object\":\"sys.pcm\","
        "\"request_id\":\"synthetic\",\"work_id\":\"%s\"}\n",
        (long)time(NULL), work_id);
    zmq_send(sock, msg, strlen(msg), 0);
    zmq_close(sock);
    printf("Injected finish message for %s\n", work_id);
}

int main(void) {
    void *ctx = zmq_ctx_new();
    char melotts_work_id[64] = {0};
    char llm_work_id[64] = {0};
    char llm_output_url[256] = {0};

    printf("Discovering melotts's work_id...\n");
    if (discover_work_id("melotts", melotts_work_id, sizeof(melotts_work_id)) != 0) {
        printf("Could not discover melotts's work_id - is the real app running?\n");
        return 1;
    }
    printf("Found melotts: %s\n", melotts_work_id);

    printf("Discovering llm's work_id...\n");
    if (discover_work_id("llm", llm_work_id, sizeof(llm_work_id)) != 0) {
        printf("Could not discover llm's work_id\n");
        return 1;
    }
    printf("Found llm: %s\n", llm_work_id);

    if (discover_output_url(llm_work_id, llm_output_url, sizeof(llm_output_url)) != 0) {
        printf("Could not discover llm's output_url\n");
        return 1;
    }
    printf("llm output_url: %s\n", llm_output_url);

    void *llm_sub = zmq_socket(ctx, ZMQ_SUB);
    zmq_setsockopt(llm_sub, ZMQ_SUBSCRIBE, "", 0);
    if (zmq_connect(llm_sub, llm_output_url) != 0) {
        printf("Could not subscribe to llm's stream: %s\n", zmq_strerror(zmq_errno()));
        return 1;
    }

    printf("Polling queue_status every %dms, watching llm's own stream for real completion...\n", POLL_INTERVAL_MS);

    int poll_count = 0;
    int was_busy = 0;
    int llm_done_this_turn = 0;  /* only true once llm reports finish:true for the CURRENT turn */
    long llm_done_at_ms = -1;    /* when that finish:true arrived */
    long idle_since_ms = -1;     /* when the audio queue last became empty (-1 = busy / unknown) */
    int  last_idx = -1;          /* diagnostic: previous chunk's index ... */
    char last_delta[128] = "";   /* ... and text, to spot a repeated word */

    while (1) {
        /* Drain everything pending on llm's own stream */
        char lbuf[4096];
        int lr;
        while ((lr = zmq_recv(llm_sub, lbuf, sizeof(lbuf) - 1, ZMQ_DONTWAIT)) > 0) {
            lbuf[lr] = '\0';
            int idx; char delta[128];
            parse_chunk(lbuf, &idx, delta, sizeof(delta));
            if (strstr(lbuf, "\"index\":0") && strstr(lbuf, "\"finish\":false")) {
                /* first chunk of a new turn - reset */
                llm_done_this_turn = 0;
                llm_done_at_ms = -1;
                idle_since_ms = -1;
                last_idx = -1;
                last_delta[0] = '\0';
            }
            /* Diagnostic for the audible "known for for" stutter: is one chunk delivered twice
             * (transport) or is the same word generated twice (model)? */
            if (delta[0] != '\0' && strcmp(delta, last_delta) == 0 && strlen(delta) <= 12) {
                if (idx == last_idx)
                    printf("DUPLICATE CHUNK: index %d '%s' was delivered twice (transport)\n", idx, delta);
                else
                    printf("REPEATED WORD: '%s' generated twice in a row, index %d then %d (model)\n", delta, last_idx, idx);
            }
            if (delta[0] != '\0') { strncpy(last_delta, delta, sizeof(last_delta) - 1); last_delta[sizeof(last_delta) - 1] = '\0'; }
            last_idx = idx;
            if (strstr(lbuf, "\"finish\":true")) {
                llm_done_this_turn = 1;
                llm_done_at_ms = now_ms();
                printf("LLM reported finish:true for current turn\n");
            }
        }

        int pending = -1, running = -1;
        if (query_queue_status(ctx, &pending, &running) == 0) {
            int is_busy = (pending > 0) || (running == 1);
            long t = now_ms();
            if (is_busy) {
                idle_since_ms = -1;  /* audio is queued/playing: any pending completion is cancelled */
            } else if (was_busy) {
                idle_since_ms = t;   /* the queue just drained */
                if (!llm_done_this_turn) {
                    printf("Idle between sentences (llm still generating) - ignoring\n");
                }
            }
            /* Declare completion only once llm is done AND the queue has stayed empty for
             * SETTLE_MS, counted from the later of "queue drained" and "llm finished". */
            if (!is_busy && llm_done_this_turn && idle_since_ms >= 0) {
                long quiet_since = (idle_since_ms > llm_done_at_ms) ? idle_since_ms : llm_done_at_ms;
                if (t - quiet_since >= SETTLE_MS) {
                    printf("Genuine completion (llm done, queue empty for %dms)\n", SETTLE_MS);
                    inject_finish_message(ctx, melotts_work_id);
                    llm_done_this_turn = 0;  /* once per turn */
                    idle_since_ms = -1;
                }
            }
            was_busy = is_busy;
        }

        poll_count++;
        if (poll_count >= REDISCOVER_EVERY_N_POLLS) {
            poll_count = 0;
            char refreshed[64];
            if (discover_work_id("melotts", refreshed, sizeof(refreshed)) == 0 &&
                strcmp(refreshed, melotts_work_id) != 0) {
                printf("melotts's work_id changed: %s -> %s\n", melotts_work_id, refreshed);
                strncpy(melotts_work_id, refreshed, sizeof(melotts_work_id) - 1);
            }
        }

        usleep(POLL_INTERVAL_MS * 1000);
    }

    zmq_ctx_destroy(ctx);
    return 0;
}
