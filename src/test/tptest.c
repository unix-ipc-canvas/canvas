/*
 * tptest.c - 전송 계층 시험 도구 (scripts/test-transport.sh 가 쓴다)
 *
 * 세 IPC 버전에 똑같이 링크되어 bin/tptest_<ipc> 가 된다. transport.h 의 함수만 부른다.
 *
 *   tptest server W              echo 서버. 수신 스레드 1개 + 워커 W 개가 작업 큐를 거쳐 동시에 tp_send_resp 한다.
 *                                열리면 stdout 에 "ready" 를 쓴다. SIGINT·SIGTERM 을 받으면 끝낸다.
 *   tptest client ID SIZE N F    echo: SIZE 바이트 요청 N 개를 동시에 F 개씩 띄워 보내고, 돌아온 내용을 req_id 별로 검사한다.
 *   tptest snap ID RESP N F      SNAPSHOT 흉내: 8B 요청으로 RESP 바이트 응답을 받는다. 응용처럼 요청은 작고 응답만 크다.
 *                                모두 맞으면 종료 코드 0. 어느 경로로 끝나든 tp_client_close 를 부른다 (transport.h 의 약속).
 *   tptest open ID [SEC]         tp_client_open 의 결과만 stdout 에 쓴다 ("open OK", "open ECONNREFUSED" 등).
 *                                열리면 SEC 초(기본 1) 붙어 있다가 닫는다.
 *
 * 응답 검사
 *   - 내용은 (클라이언트 번호, 요청 번호, 위치)로 정해지는 바이트열이다. echo 는 보낸 그대로, snap 은 서버가
 *     snap_byte() 로 만든 그대로 와야 맞다.
 *   - req_id 는 세션마다 다른 값(pid << 32)에서 시작한다. 같은 번호로 다시 접속했을 때 예전 세션의 응답이 오면
 *     stale 로 세고 버린다 (transport.h 의 약속). 그 밖에 크기·번호가 다르거나 두 번 오면 bad 로 센다.
 *
 * 종료 코드 (client)  0 성공, 2 열기 실패, 3 송신 실패, 4 수신 실패, 5 응답 내용이 틀림
 */
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "transport.h"
#include "util.h"

#define QCAP        64
#define MAX_WORKERS 64

/* snap 응답의 k 번째 바이트. 서버가 만들고 클라이언트가 확인한다 */
static char snap_byte(uint32_t id, uint64_t req_id, size_t k)
{
    return (char)((id * 31u + (uint32_t)req_id * 7u + (uint32_t)k) & 0xffu);
}

/* 시험 스크립트가 비교하는 errno 이름. 오류 문구는 언어 설정에 따라 달라서 이름으로 쓴다 */
static const char *errno_name(int e)
{
    switch (e) {
    case 0:            return "OK";
    case ECONNREFUSED: return "ECONNREFUSED";
    case EADDRINUSE:   return "EADDRINUSE";
    case EINVAL:       return "EINVAL";
    default:           return "OTHER";
    }
}

/* ================================================================
 * 서버: 수신 스레드 → 작업 큐 → 워커들 (2단계 서버와 같은 모양)
 * ================================================================ */

typedef struct { msg_hdr_t h; char *buf; } job_t;

static job_t q[QCAP];
static int qh, qt, qn;
static bool q_stop;
static pthread_mutex_t q_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_not_empty = PTHREAD_COND_INITIALIZER, q_not_full = PTHREAD_COND_INITIALIZER;
static atomic_long served, epipe;

static void *srv_recv_main(void *arg)
{
    (void)arg;
    char eb[128];
    for (;;) {
        char *buf = malloc(MAX_PAYLOAD);
        if (!buf) { fprintf(stderr, "server: 메모리 부족\n"); break; }
        msg_hdr_t h;
        ssize_t r = tp_recv_req(&h, buf, MAX_PAYLOAD);
        if (r < 0) {
            int e = errno;
            free(buf);
            if (e == ECANCELED) break;                     /* tp_server_wake */
            if (e == EMSGSIZE || e == EINTR) continue;
            fprintf(stderr, "server: tp_recv_req: %s\n", err_str(e, eb, sizeof eb));
            break;
        }
        pthread_mutex_lock(&q_mtx);
        while (qn == QCAP) pthread_cond_wait(&q_not_full, &q_mtx);
        q[qt].h = h;
        q[qt].buf = buf;
        qt = (qt + 1) % QCAP;
        qn++;
        pthread_cond_signal(&q_not_empty);
        pthread_mutex_unlock(&q_mtx);
    }
    pthread_mutex_lock(&q_mtx);
    q_stop = true;                                         /* 큐에 남은 일은 워커가 마저 보낸다 */
    pthread_cond_broadcast(&q_not_empty);
    pthread_mutex_unlock(&q_mtx);
    return NULL;
}

static void *srv_worker_main(void *arg)
{
    (void)arg;
    char eb[128];
    for (;;) {
        pthread_mutex_lock(&q_mtx);
        while (qn == 0 && !q_stop) pthread_cond_wait(&q_not_empty, &q_mtx);
        if (qn == 0) { pthread_mutex_unlock(&q_mtx); break; }
        job_t j = q[qh];
        qh = (qh + 1) % QCAP;
        qn--;
        pthread_cond_signal(&q_not_full);
        pthread_mutex_unlock(&q_mtx);

        if (j.h.type == REQ_SNAPSHOT && j.h.payload_len == sizeof(uint64_t)) {
            uint64_t want;                                 /* snap: 요청한 크기만큼 정해진 내용으로 채운다 */
            memcpy(&want, j.buf, sizeof want);
            if (want > MAX_PAYLOAD) want = 0;
            for (size_t k = 0; k < want; k++) j.buf[k] = snap_byte(j.h.client_id, j.h.req_id, k);
            j.h.payload_len = (uint32_t)want;
        }                                                  /* 그 밖(echo, HELLO)은 payload 를 받은 그대로 */
        j.h.type = RESP_OK;
        if (tp_send_resp(&j.h, j.buf) < 0) {
            if (errno == EPIPE) atomic_fetch_add(&epipe, 1);    /* 그 클라이언트가 끝났다 */
            else fprintf(stderr, "server: tp_send_resp: %s\n", err_str(errno, eb, sizeof eb));
        } else {
            atomic_fetch_add(&served, 1);
        }
        free(j.buf);
    }
    return NULL;
}

static int run_server(int workers)
{
    char eb[128];
    if (workers < 1 || workers > MAX_WORKERS) { fprintf(stderr, "server: 워커 수는 1~%d\n", MAX_WORKERS); return 2; }

    /* 종료 신호는 main 만 sigwait 로 받는다. 스레드를 만들기 전에 막아 두면 모두 물려받는다 */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    if (tp_server_open() < 0) {
        int e = errno;
        printf("server_open %s (%s)\n", errno_name(e), err_str(e, eb, sizeof eb));
        fflush(stdout);
        return 1;
    }
    pthread_t rt, wt[MAX_WORKERS];
    int made = 0, e = pthread_create(&rt, NULL, srv_recv_main, NULL);
    bool have_rt = e == 0;
    for (; e == 0 && made < workers; made++)
        if ((e = pthread_create(&wt[made], NULL, srv_worker_main, NULL)) != 0) break;
    if (e == 0) {
        printf("ready\n");
        fflush(stdout);
        int sig;
        sigwait(&set, &sig);
    } else {
        fprintf(stderr, "server: pthread_create: %s\n", err_str(e, eb, sizeof eb));
    }

    /* 종료: 수신 스레드 깨우기 → 큐 비우고 워커 종료 → 자원 삭제 (설계 명세의 종료 순서) */
    tp_server_wake();
    if (have_rt) pthread_join(rt, NULL);
    for (int i = 0; i < made; i++) pthread_join(wt[i], NULL);
    tp_server_close();
    printf("server done served=%ld epipe=%ld\n", atomic_load(&served), atomic_load(&epipe));
    return e == 0 ? 0 : 1;
}

/* ================================================================
 * 클라이언트
 * ================================================================ */

static uint32_t c_id;
static bool c_snap;                         /* snap 이면 8B 요청에 c_size 바이트 응답, 아니면 c_size 바이트 echo */
static size_t c_size;                       /* 응답 크기 */
static long c_n;
static uint64_t c_base;                     /* 이 세션의 req_id 시작값 */
static sem_t c_slots;                       /* 더 띄울 수 있는 요청 수 */
static atomic_long c_bad, c_stale;
static atomic_int c_err;                    /* 0 이 아니면 실패한 단계의 종료 코드 */

/* i 번째 요청의 echo 내용, 또는 snap 응답으로 와야 할 내용을 b 에 채운다 */
static void fill(char *b, uint64_t i)
{
    if (c_snap) {
        for (size_t k = 0; k < c_size; k++) b[k] = snap_byte(c_id, c_base + i, k);
    } else {
        for (size_t k = 0; k < c_size; k++) b[k] = (char)((c_id * 97u + i * 13u + k) & 0xffu);
    }
}

static void cli_fail(int code, const char *what, int e)
{
    char eb[128];
    int zero = 0;
    if (atomic_compare_exchange_strong(&c_err, &zero, code))
        fprintf(stderr, "client %u: %s: %s\n", c_id, what, err_str(e, eb, sizeof eb));
    for (int i = 0; i < 4096; i++) sem_post(&c_slots);   /* 송신 스레드가 슬롯을 기다리고 있으면 깨운다 */
}

static void *cli_send_main(void *arg)
{
    (void)arg;
    char *b = malloc(c_size ? c_size : 1);
    if (!b) { cli_fail(3, "malloc", ENOMEM); return NULL; }
    for (long i = 0; i < c_n; i++) {
        while (sem_wait(&c_slots) < 0 && errno == EINTR)
            ;
        if (atomic_load(&c_err)) break;
        msg_hdr_t h = { .type = REQ_DRAW, .client_id = c_id, .req_id = c_base + (uint64_t)i,
                        .payload_len = (uint32_t)c_size };
        int rc;
        if (c_snap) {
            uint64_t want = c_size;
            h.type = REQ_SNAPSHOT;
            h.payload_len = sizeof want;
            rc = tp_send_req(&h, &want);
        } else {
            fill(b, (uint64_t)i);
            rc = tp_send_req(&h, b);
        }
        if (rc < 0) { cli_fail(3, "tp_send_req", errno); break; }
    }
    free(b);
    return NULL;
}

static void *cli_recv_main(void *arg)
{
    (void)arg;
    char *b = malloc(c_size ? c_size : 1), *want = malloc(c_size ? c_size : 1);
    char *seen = calloc((size_t)c_n ? (size_t)c_n : 1, 1);
    if (!b || !want || !seen) { cli_fail(4, "malloc", ENOMEM); goto out; }
    for (long got = 0; got < c_n; ) {
        msg_hdr_t h;
        ssize_t r = tp_recv_resp(&h, b, c_size);
        if (r < 0 && errno == EMSGSIZE) { atomic_fetch_add(&c_stale, 1); continue; }    /* 예전 세션의 큰 응답 */
        if (r < 0) { cli_fail(4, "tp_recv_resp", errno); break; }
        if (h.req_id < c_base || h.req_id >= c_base + (uint64_t)c_n) {
            atomic_fetch_add(&c_stale, 1);                 /* 띄워 두지 않은 req_id: 예전 세션의 응답 */
            continue;
        }
        uint64_t i = h.req_id - c_base;
        if ((size_t)r != c_size || h.type != RESP_OK || h.client_id != c_id || seen[i]) {
            atomic_fetch_add(&c_bad, 1);
        } else {
            seen[i] = 1;
            fill(want, i);
            if (memcmp(b, want, c_size) != 0) atomic_fetch_add(&c_bad, 1);
        }
        got++;
        sem_post(&c_slots);
    }
out:
    free(b);
    free(want);
    free(seen);
    return NULL;
}

static int run_client(bool snap, uint32_t id, size_t size, long n, unsigned f)
{
    char eb[128];
    if (size > MAX_PAYLOAD || n < 0 || f < 1) { fprintf(stderr, "client: 잘못된 인자\n"); return 2; }
    c_snap = snap;
    c_id = id;
    c_size = size;
    c_n = n;
    c_base = (uint64_t)getpid() << 32;

    /* 서버가 뜰 때까지 5초 동안 다시 시도한다 */
    int rc = -1;
    for (int t = 0; t < 500 && (rc = tp_client_open(id)) < 0 && errno == ECONNREFUSED; t++) {
        struct timespec ts = { 0, 10000000L };
        nanosleep(&ts, NULL);
    }
    if (rc < 0) {
        fprintf(stderr, "client %u: tp_client_open: %s\n", id, err_str(errno, eb, sizeof eb));
        return 2;
    }

    /*
     * 첫 요청은 HELLO 다. 크기가 PIPE_BUF 를 넘는 실행에서는 보내지 않는다: pipe 버전은 작은 요청을 잠금 없이
     * 쓰므로 다른 클라이언트의 큰 요청 사이에 HELLO 가 끼어들 수 있다 (transport.h 의 약속). 이 시험의 클라이언트는
     * 처음 접속이거나, 같은 번호의 예전 클라이언트가 응답을 받기 전에 죽은 경우라서 HELLO 없이도 응답 채널이 맞다.
     */
    size_t req_size = snap ? sizeof(uint64_t) : size;
    if (req_size + sizeof(msg_hdr_t) <= 4096) {
        msg_hdr_t h = { .type = REQ_HELLO, .client_id = id, .req_id = c_base - 1, .payload_len = 0 };
        char d[1];
        ssize_t r;
        if (tp_send_req(&h, d) < 0) {
            cli_fail(3, "HELLO 보내기", errno);
        } else {
            /* 예전 세션의 응답이 먼저 올 수 있다. HELLO 의 응답이 올 때까지 버린다 */
            char *junk = malloc(MAX_PAYLOAD);
            do {
                r = tp_recv_resp(&h, junk, junk ? MAX_PAYLOAD : 0);
            } while ((r >= 0 && h.req_id != c_base - 1) || (r < 0 && errno == EMSGSIZE));
            free(junk);
            if (r < 0) cli_fail(4, "HELLO 응답", errno);
        }
    }

    sem_init(&c_slots, 0, f);
    pthread_t st, rt;
    bool have_s = false, have_r = false;
    if (!atomic_load(&c_err)) {
        int e = pthread_create(&rt, NULL, cli_recv_main, NULL);
        if (e) cli_fail(4, "pthread_create", e); else have_r = true;
    }
    if (!atomic_load(&c_err)) {
        int e = pthread_create(&st, NULL, cli_send_main, NULL);
        if (e) cli_fail(3, "pthread_create", e); else have_s = true;
    }
    if (have_s) pthread_join(st, NULL);
    /* 송신이 먼저 실패했으면 띄워 둔 응답이 오지 않을 수 있다. 서버가 끝났으면 수신도 곧 실패로 빠져나온다 */
    if (have_r) pthread_join(rt, NULL);
    tp_client_close();                                    /* 어느 경로든 닫는다 */

    long bad = atomic_load(&c_bad);
    printf("client %u done bad=%ld stale=%ld\n", id, bad, atomic_load(&c_stale));
    if (atomic_load(&c_err)) return atomic_load(&c_err);
    return bad ? 5 : 0;
}

/* ================================================================
 * open
 * ================================================================ */

static int run_open(uint32_t id, unsigned sec)
{
    int rc = tp_client_open(id);
    printf("open %s\n", errno_name(rc == 0 ? 0 : errno));
    fflush(stdout);
    if (rc == 0) {
        sleep(sec);
        tp_client_close();
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "server") == 0)
        return run_server(argc > 2 ? atoi(argv[2]) : 4);
    if (argc >= 6 && (strcmp(argv[1], "client") == 0 || strcmp(argv[1], "snap") == 0))
        return run_client(strcmp(argv[1], "snap") == 0, (uint32_t)strtoul(argv[2], NULL, 10),
                          (size_t)strtoul(argv[3], NULL, 10), strtol(argv[4], NULL, 10),
                          (unsigned)strtoul(argv[5], NULL, 10));
    if (argc >= 3 && strcmp(argv[1], "open") == 0)
        return run_open((uint32_t)strtoul(argv[2], NULL, 10), argc > 3 ? (unsigned)strtoul(argv[3], NULL, 10) : 1);
    fprintf(stderr,
            "사용법: %s server [워커수]\n"
            "        %s client 번호 크기 요청수 동시요청수\n"
            "        %s snap 번호 응답크기 요청수 동시요청수\n"
            "        %s open 번호 [초]\n", argv[0], argv[0], argv[0], argv[0]);
    return 2;
}
