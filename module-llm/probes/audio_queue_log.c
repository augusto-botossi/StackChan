/*
 * audio_queue_log.c
 *
 * Tiny logger for probe_speech_compare.py. Polls the audio unit's queue_status (the same RPC the
 * melotts finish helper uses) about every 80 ms and prints one line each time the audio goes busy
 * or idle:
 *
 *     <milliseconds on CLOCK_MONOTONIC> busy
 *     <milliseconds on CLOCK_MONOTONIC> idle
 *
 * "busy" = something is queued or playing. Python's time.monotonic() runs on the same clock, so the
 * probe can line these events up with the moment it sent the question.
 *
 * Compile on the Module LLM (same as the finish helper):
 *   gcc /opt/audio_queue_log.c -lzmq -o /opt/audio_queue_log
 */

#include <zmq.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int query_queue_status(void *ctx, int *pending, int *running) {
    void *sock = zmq_socket(ctx, ZMQ_REQ);
    int timeout_ms = 1000;
    int linger = 0;
    zmq_setsockopt(sock, ZMQ_SNDTIMEO, &timeout_ms, sizeof(timeout_ms));
    zmq_setsockopt(sock, ZMQ_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));
    zmq_setsockopt(sock, ZMQ_LINGER, &linger, sizeof(linger));

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

int main(void) {
    void *ctx = zmq_ctx_new();
    int last = -1;
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (;;) {
        int pending = 0, running = 0;
        if (query_queue_status(ctx, &pending, &running) == 0) {
            int busy = (pending > 0) || (running == 1);
            if (busy != last) {
                printf("%ld %s\n", now_ms(), busy ? "busy" : "idle");
                last = busy;
            }
        }
        usleep(80000);
    }
    return 0;
}
