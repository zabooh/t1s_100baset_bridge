/*******************************************************************************
  bandwidth_ramp_client.c - reference TCP echo bandwidth-ramp client

  Same test as test_bandwidth_ramp.py, deliberately re-implemented in plain C
  with Winsock2 and a single select()-driven loop instead of Python threads -
  written specifically because the Python version's numbers were not trusted
  (FALLSTRICKE.md, 2026-08-27: identical-looking data loss showed up talking
  directly to the bridge's own testserver, no T1S/PLCA involved at all, which
  could point at the Python client's receiver-thread timing just as easily as
  at the C server). No threads here - one loop, one select() call, nothing to
  race - so if this ALSO shows loss, the server is the problem; if it does
  not, the Python client was.

  Talks to firmware/src/testserver.c's TCP echo server: paces sends at a
  target rate, the server echoes back unmodified, this measures what actually
  came back. Repeated at increasing rates until the achieved rate falls
  meaningfully short of the target.

  Build (MinGW-w64 gcc, confirmed present on this machine):
    gcc -O2 -Wall -o bandwidth_ramp_client.exe bandwidth_ramp_client.c -lws2_32

  Usage:
    bandwidth_ramp_client.exe <host> <port> [start_bps] [max_bps] [factor] [duration_s]
    bandwidth_ramp_client.exe 192.168.0.202 5566
    bandwidth_ramp_client.exe 192.168.0.210 5566 10000 20000000 1.6 3.0
*******************************************************************************/

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#pragma comment(lib, "ws2_32.lib")

#define CHUNK_SIZE 1024
#define DEFAULT_DURATION_S 3.0
#define DRAIN_GRACE_S 2.0
#define SUCCESS_RATIO 0.90
#define CONSECUTIVE_FAILS_TO_STOP 2

static FILE *g_log = NULL;
static double g_t0 = 0.0;

static double now_s(void) {
    static LARGE_INTEGER freq;
    static int have_freq = 0;
    LARGE_INTEGER count;
    if (!have_freq) {
        QueryPerformanceFrequency(&freq);
        have_freq = 1;
    }
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart / (double)freq.QuadPart;
}

static void logmsg(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    double t = now_s() - g_t0;
    printf("[t+%7.2fs] %s\n", t, buf);
    if (g_log) {
        fprintf(g_log, "[t+%7.2fs] %s\n", t, buf);
        fflush(g_log);
    }
}

static const char *human_bps(double bps, char *out, size_t outlen) {
    if (bps >= 1000000.0) {
        snprintf(out, outlen, "%.2f Mbps", bps / 1000000.0);
    } else if (bps >= 1000.0) {
        snprintf(out, outlen, "%.1f Kbps", bps / 1000.0);
    } else {
        snprintf(out, outlen, "%.0f bps", bps);
    }
    return out;
}

typedef struct {
    long sent_bytes;
    long received_bytes;
    double achieved_bps;
} step_result_t;

/* Single-threaded: one select() call per loop iteration decides whether the
 * socket is ready to send (and it's time to) and/or has data to drain. No
 * background thread, so there is nothing for the OS scheduler to delay or
 * reorder relative to this loop - the entire point of this file. */
static void run_step(SOCKET sock, double target_bps, double duration_s, step_result_t *result) {
    static uint8_t pattern[CHUNK_SIZE];
    static int pattern_ready = 0;
    if (!pattern_ready) {
        for (int i = 0; i < CHUNK_SIZE; i++) pattern[i] = (uint8_t)(i % 256);
        pattern_ready = 1;
    }

    double interval = (CHUNK_SIZE * 8.0) / target_bps;
    double start = now_s();
    double stop_at = start + duration_s;
    double next_send = start;
    long sent = 0, received = 0;
    double last_recv_time = -1.0;
    static uint8_t recvbuf[65536];

    int sending_done = 0;
    double grace_deadline = 0.0;

    for (;;) {
        double t = now_s();

        if (!sending_done && t >= stop_at) {
            sending_done = 1;
            double grace = (DRAIN_GRACE_S > interval * 3.0) ? DRAIN_GRACE_S : interval * 3.0;
            grace_deadline = t + grace;
        }
        if (sending_done) {
            if (received >= sent) break;
            if (t >= grace_deadline) break;
            if (last_recv_time >= 0.0 && (t - last_recv_time) > 1.0) break;
        }

        fd_set readfds, writefds;
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);
        FD_SET(sock, &readfds);
        int want_write = (!sending_done && t >= next_send);
        if (want_write) {
            FD_SET(sock, &writefds);
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 20000; /* 20ms poll granularity */
        int nfds = select(0, &readfds, want_write ? &writefds : NULL, NULL, &tv);
        if (nfds == SOCKET_ERROR) {
            logmsg("  select error: %d", WSAGetLastError());
            break;
        }

        if (want_write && FD_ISSET(sock, &writefds)) {
            int n = send(sock, (const char *)pattern, CHUNK_SIZE, 0);
            if (n == SOCKET_ERROR) {
                logmsg("  send error: %d", WSAGetLastError());
                break;
            }
            sent += n;
            next_send += interval;
        }
        if (FD_ISSET(sock, &readfds)) {
            int n = recv(sock, (char *)recvbuf, sizeof(recvbuf), 0);
            if (n > 0) {
                received += n;
                last_recv_time = now_s();
            } else if (n == 0) {
                logmsg("  connection closed by peer");
                break;
            } else {
                int err = WSAGetLastError();
                if (err != WSAEWOULDBLOCK) {
                    logmsg("  recv error: %d", err);
                    break;
                }
            }
        }
    }

    double end_time = (last_recv_time >= 0.0) ? last_recv_time : now_s();
    double elapsed = end_time - start;
    if (elapsed < 0.001) elapsed = 0.001;

    result->sent_bytes = sent;
    result->received_bytes = received;
    result->achieved_bps = (received * 8.0) / elapsed;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <host> <port> [start_bps] [max_bps] [factor] [duration_s]\n", argv[0]);
        return 2;
    }
    const char *host = argv[1];
    const char *port = argv[2];
    double start_bps = (argc > 3) ? atof(argv[3]) : 10000.0;
    double max_bps = (argc > 4) ? atof(argv[4]) : 20000000.0;
    double factor = (argc > 5) ? atof(argv[5]) : 1.6;
    double duration_s = (argc > 6) ? atof(argv[6]) : DEFAULT_DURATION_S;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    char out_path[256];
    snprintf(out_path, sizeof(out_path), "bandwidth_ramp_c_%04d%02d%02d_%02d%02d%02d.txt",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    g_log = fopen(out_path, "w");
    if (!g_log) {
        fprintf(stderr, "could not open log file %s\n", out_path);
    }
    g_t0 = now_s();
    printf("Logging to %s\n", out_path);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(host, port, &hints, &res) != 0) {
        fprintf(stderr, "getaddrinfo failed for %s:%s\n", host, port);
        return 1;
    }

    logmsg("connecting to %s:%s ...", host, port);
    SOCKET sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock == INVALID_SOCKET) {
        fprintf(stderr, "socket() failed: %d\n", WSAGetLastError());
        return 1;
    }
    if (connect(sock, res->ai_addr, (int)res->ai_addrlen) == SOCKET_ERROR) {
        fprintf(stderr, "connect() failed: %d\n", WSAGetLastError());
        return 1;
    }
    freeaddrinfo(res);
    logmsg("connected");

    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&flag, sizeof(flag));
    u_long nonblocking = 1;
    ioctlsocket(sock, FIONBIO, &nonblocking);

    double target = start_bps;
    int consecutive_fails = 0;
    double best_good_bps = 0.0;
    double ceiling_bps = -1.0;
    char hb1[32], hb2[32];

    while (target <= max_bps) {
        logmsg("step: target=%s duration=%.1fs", human_bps(target, hb1, sizeof(hb1)), duration_s);
        step_result_t r;
        run_step(sock, target, duration_s, &r);
        double ratio = (target > 0.0) ? (r.achieved_bps / target) : 0.0;
        int ok = ratio >= SUCCESS_RATIO;
        logmsg("  sent=%ld bytes  received=%ld bytes  achieved=%s (%.0f%% of target)  %s",
               r.sent_bytes, r.received_bytes, human_bps(r.achieved_bps, hb1, sizeof(hb1)),
               ratio * 100.0, ok ? "OK" : "SHORTFALL");

        if (ok) {
            if (r.achieved_bps > best_good_bps) best_good_bps = r.achieved_bps;
            consecutive_fails = 0;
        } else {
            consecutive_fails++;
            if (ceiling_bps < 0.0) ceiling_bps = best_good_bps;
            if (consecutive_fails >= CONSECUTIVE_FAILS_TO_STOP) {
                logmsg("stopping: %d consecutive shortfalls - ceiling reached", consecutive_fails);
                break;
            }
        }
        target *= factor;
    }
    if (target > max_bps) {
        logmsg("reached max_bps (%s) without a clear ceiling", human_bps(max_bps, hb1, sizeof(hb1)));
    }

    logmsg("=== RESULT === best sustained rate: %s", human_bps(best_good_bps, hb1, sizeof(hb1)));
    if (ceiling_bps >= 0.0) {
        logmsg("=== RESULT === ceiling first seen around: %s", human_bps(ceiling_bps, hb2, sizeof(hb2)));
    }

    closesocket(sock);
    WSACleanup();
    if (g_log) fclose(g_log);
    return 0;
}
