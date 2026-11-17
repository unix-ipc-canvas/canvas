/*
 * bench.c - 1단계 echo 벤치마크 (세 IPC 공통)
 *
 * 한 번 실행하면 조건 하나(메시지 크기, 클라이언트 수, 동시 요청 수)를 재고 결과 CSV 에 두 줄(client, server)을 붙인다.
 * 어떤 IPC 인지는 링크된 전송 계층(src/transport/)이 정한다. 이 파일은 transport.h 의 함수만 부른다.
 *
 * 프로세스 (모두 이 실행 파일이 fork 한다)
 *   조정 프로세스  서버 1개와 클라이언트 N개를 띄우고, 측정 시작을 맞추고, 결과를 모아 CSV 에 쓴다
 *   서버          받은 메시지를 같은 크기로 그대로 돌려준다
 *   클라이언트     메시지를 보내고 응답을 받을 때까지의 왕복 시간을 잰다
 *
 * 스레드 (과제 조건: 프로세스마다 3개 이상)
 *   서버       echo(받아서 돌려주기), admin(신호 받기), stats(측정 구간의 자원 사용량 기록), main
 *   클라이언트  send, recv, stats(측정 뒤 자원 사용량 기록), main(시작 맞추기와 결과 쓰기)
 *
 * 측정 순서
 *   1. 클라이언트마다 REQ_HELLO 를 보내고 워밍업 메시지를 주고받는다. 워밍업 응답을 모두 받으면 준비 신호를 보낸다.
 *   2. 모든 클라이언트가 준비되면 조정 프로세스가 서버에 측정 시작을 알리고, 클라이언트들에게 한꺼번에 시작 신호를 보낸다.
 *   3. 클라이언트는 정해진 수만큼 재고 끝난다. 조정 프로세스가 서버를 끝내고 결과를 모은다.
 *   t0 = tp_send_req 호출 직전, t5 = tp_recv_resp 가 돌아온 직후. 왕복 지연 = t5 - t0.
 *   지연 표본은 모든 클라이언트의 것을 합쳐 통계를 낸다. 처리량은 (잰 메시지 수) / (가장 먼저 시작한 시각부터
 *   가장 늦게 끝난 시각까지)이다. 동시 요청 수(-f)가 1 이면 지연을, 크면 처리량을 보는 실행이다.
 *
 * DECISION: 서버는 받은 스레드에서 바로 돌려준다. 작업 큐와 워커를 거치면 스레드 깨우기 비용이 섞여
 *   IPC 자체의 차이가 가려진다. 큐·워커 비용은 2단계 응용에서 구간별 시간으로 따로 잰다.
 * DECISION: 벤치 전용 메시지 종류(BENCH_ECHO)는 이 파일 안에 둔다. 응용 프로토콜이 아니므로 proto.h 를 바꾸지 않는다.
 */
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "transport.h"
#include "util.h"

#define BENCH_ECHO   200u                   /* 벤치 메시지 종류. proto.h 의 번호와 겹치지 않는다 */
#define HELLO_ID     UINT64_MAX             /* HELLO 의 req_id. 측정 메시지 번호와 겹치지 않는다 */

#define SIG_READY    (SIGRTMIN)             /* 클라이언트·서버 → 조정: 준비됨 (값 = 보낸 쪽 번호) */
#define SIG_GO       (SIGRTMIN + 1)         /* 조정 → 클라이언트: 측정 시작 */
#define SIG_MARK     SIGUSR1                /* 조정 → 서버: 측정 시작, 자원 사용량 기록 */
#define SRV_TAG      1000                   /* 서버가 보낸 SIG_READY 의 값 */

#define CONNECT_SEC  10                     /* 서버가 뜰 때까지 클라이언트가 기다리는 시간 */
#define RUN_SEC      180                    /* 실행 한 번의 제한 시간. 넘으면 실패로 끝낸다 */

typedef struct {
    unsigned    clients;    /* -c 동시 클라이언트 수 */
    size_t      size;       /* -s payload 바이트 수 */
    uint64_t    n;          /* -n 잴 메시지 수 (모든 클라이언트 합) */
    uint64_t    warm;       /* -w 클라이언트마다 워밍업 메시지 수 */
    unsigned    inflight;   /* -f 클라이언트마다 동시에 띄워 둘 요청 수 */
    const char *exp;        /* -e 실험 이름 */
    long        run;        /* -r 반복 번호 */
    const char *out;        /* -o 결과 CSV */
    const char *commit;     /* -C 측정한 커밋 */
    bool        verify;     /* -V 모든 응답의 내용을 검사 (시험용, 측정에 쓰지 않는다) */
    bool        clock_mode; /* -K clock_gettime 비용만 잰다 */
} cfg_t;

/* 결과 파일. 같은 실행 파일이 쓰고 읽으므로 구조체를 그대로 쓴다 */
typedef struct {
    int      ok;
    uint64_t n;                 /* 측정 구간에 처리한 메시지 수 */
    uint64_t t_start, t_end;    /* 측정 구간 (CLOCK_MONOTONIC ns) */
    usage_t  usage;             /* 측정 구간의 자원 사용량 */
    long     vmrss_kb, vmhwm_kb;
    char     err[160];
} result_t;

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

static void result_path(char *buf, size_t len, const char *dir, const char *name)
{
    snprintf(buf, len, "%s/%s", dir, name);
}

/* 결과와 (있으면) 표본을 파일에 쓴다. 측정이 끝난 뒤에만 부른다 */
static void write_result(const char *dir, const char *name, const result_t *r,
                         const uint64_t *samples, uint64_t n)
{
    char path[256];
    result_path(path, sizeof path, dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(r, sizeof *r, 1, f);
    if (samples && n) fwrite(samples, sizeof *samples, n, f);
    fclose(f);
}

/* ================================================================
 * 서버
 * ================================================================ */

typedef struct {
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
    int             phase;              /* 0 시작 전, 1 측정 중, 2 끝 */
    atomic_uint_fast64_t count;         /* 돌려준 BENCH_ECHO 수 */
    uint64_t        count_start;
    usage_t         u_start, u_end;
    result_t        res;
    char           *buf;
    pid_t           driver;
} srv_t;

static void srv_error(srv_t *s, const char *what, int e)
{
    pthread_mutex_lock(&s->mtx);
    if (!s->res.err[0]) snprintf(s->res.err, sizeof s->res.err, "서버 %s: %s", what, strerror(e));
    pthread_mutex_unlock(&s->mtx);
}

static void *srv_echo_main(void *arg)
{
    srv_t *s = arg;
    msg_hdr_t h;
    for (;;) {
        ssize_t r = tp_recv_req(&h, s->buf, MAX_PAYLOAD);
        if (r < 0) {
            if (errno == ECANCELED) break;                  /* tp_server_wake */
            if (errno == EINTR || errno == EMSGSIZE) continue;
            srv_error(s, "tp_recv_req", errno);
            break;
        }
        uint32_t type = h.type;
        h.type = RESP_OK;                                   /* payload 는 받은 그대로 */
        if (tp_send_resp(&h, s->buf) < 0) {
            if (errno == EPIPE) continue;                   /* 그 클라이언트가 먼저 끝났다 */
            srv_error(s, "tp_send_resp", errno);
            break;
        }
        if (type == BENCH_ECHO) atomic_fetch_add(&s->count, 1);
    }
    return NULL;
}

/* SIG_MARK 이면 측정 시작, SIGINT·SIGTERM 이면 끝. 끝날 때 echo 스레드를 깨운다 */
static void *srv_admin_main(void *arg)
{
    srv_t *s = arg;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIG_MARK);
    for (;;) {
        int sig;
        if (sigwait(&set, &sig) != 0) continue;
        pthread_mutex_lock(&s->mtx);
        if (sig == SIG_MARK) {
            if (s->phase == 0) s->phase = 1;
            pthread_cond_broadcast(&s->cv);
            pthread_mutex_unlock(&s->mtx);
            continue;
        }
        s->phase = 2;
        pthread_cond_broadcast(&s->cv);
        pthread_mutex_unlock(&s->mtx);
        tp_server_wake();
        return NULL;
    }
}

/* 측정 시작과 끝의 자원 사용량을 기록한다. 시작을 기록하면 조정 프로세스에 알린다 */
static void *srv_stats_main(void *arg)
{
    srv_t *s = arg;
    pthread_mutex_lock(&s->mtx);
    while (s->phase == 0) pthread_cond_wait(&s->cv, &s->mtx);
    if (s->phase == 1) {
        usage_now(&s->u_start);
        s->res.t_start = now_ns();
        s->count_start = atomic_load(&s->count);
        pthread_mutex_unlock(&s->mtx);
        sigqueue(s->driver, SIG_READY, (union sigval){ .sival_int = SRV_TAG });
        pthread_mutex_lock(&s->mtx);
        while (s->phase == 1) pthread_cond_wait(&s->cv, &s->mtx);
    }
    usage_now(&s->u_end);
    s->res.t_end = now_ns();
    s->res.n = atomic_load(&s->count) - s->count_start;
    s->res.vmrss_kb = vm_rss_kb();
    s->res.vmhwm_kb = vm_hwm_kb();
    pthread_mutex_unlock(&s->mtx);
    return NULL;
}

static int run_server(const char *dir, pid_t driver)
{
    srv_t s;
    memset(&s, 0, sizeof s);
    pthread_mutex_init(&s.mtx, NULL);
    pthread_cond_init(&s.cv, NULL);
    atomic_init(&s.count, 0);
    s.driver = driver;

    /* 신호는 admin 스레드만 받는다. 스레드를 만들기 전에 막아 두면 모두 물려받는다 */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIG_MARK);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    s.buf = malloc(MAX_PAYLOAD);
    if (!s.buf) {
        snprintf(s.res.err, sizeof s.res.err, "서버 메모리 부족");
        write_result(dir, "server", &s.res, NULL, 0);
        return 1;
    }
    if (tp_server_open() < 0) {
        snprintf(s.res.err, sizeof s.res.err, "tp_server_open: %s", strerror(errno));
        write_result(dir, "server", &s.res, NULL, 0);
        return 1;
    }

    pthread_t th[3];                                    /* echo, admin, stats */
    void *(*const th_main[3])(void *) = { srv_echo_main, srv_admin_main, srv_stats_main };
    for (int i = 0; i < 3; i++) {
        int e = pthread_create(&th[i], NULL, th_main[i], &s);
        if (e) {
            /* 만든 스레드는 거두지 않는다. 자원만 지우고 프로세스째 끝낸다 (_exit 가 남은 스레드를 끝낸다) */
            srv_error(&s, "pthread_create", e);
            tp_server_wake();
            tp_server_close();
            write_result(dir, "server", &s.res, NULL, 0);
            return 1;
        }
    }
    pthread_join(th[1], NULL);                          /* admin: 종료 신호를 받으면 echo 를 깨우고 끝난다 */
    pthread_join(th[0], NULL);
    pthread_join(th[2], NULL);
    tp_server_close();

    usage_diff(&s.u_end, &s.u_start, &s.res.usage);
    s.res.ok = s.res.err[0] == '\0';
    write_result(dir, "server", &s.res, NULL, 0);
    free(s.buf);
    return s.res.ok ? 0 : 1;
}

/* ================================================================
 * 클라이언트
 * ================================================================ */

typedef struct {
    const cfg_t    *cfg;
    uint32_t        id;
    uint64_t        warm, n;            /* 이 클라이언트의 워밍업 수, 잴 수 */
    char           *sbuf, *rbuf, *pattern;
    _Atomic uint64_t *t0;               /* 요청 번호별 송신 시각 */
    uint8_t        *seen;               /* 요청 번호별 응답을 받았는지 */
    uint64_t       *lat;                /* 측정 표본 n 개 (ns) */
    sem_t           slots;              /* 더 띄울 수 있는 요청 수 */
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
    bool            started;            /* HELLO 를 주고받았다. 송수신 스레드가 이것을 기다렸다가 시작한다 */
    bool            warm_sent, warm_done, go, recv_done;
    atomic_bool     failed;
    result_t        res;
    usage_t         u_start, u_end;
} cli_t;

/* 실패를 기록하고 기다리는 스레드를 모두 깨운다 */
static void cli_fail(cli_t *c, const char *what, int e)
{
    pthread_mutex_lock(&c->mtx);
    if (!atomic_load(&c->failed)) {
        snprintf(c->res.err, sizeof c->res.err, "클라이언트 %u %s%s%s", c->id, what,
                 e ? ": " : "", e ? strerror(e) : "");
        atomic_store(&c->failed, true);
    }
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mtx);
    for (unsigned i = 0; i <= c->cfg->inflight; i++) sem_post(&c->slots);
}

/* main 이 HELLO 를 주고받을 때까지 기다린다. 그 전에 실패했으면 false */
static bool wait_start(cli_t *c)
{
    pthread_mutex_lock(&c->mtx);
    while (!c->started && !atomic_load(&c->failed)) pthread_cond_wait(&c->cv, &c->mtx);
    bool ok = !atomic_load(&c->failed);
    pthread_mutex_unlock(&c->mtx);
    return ok;
}

static void slot_take(cli_t *c)
{
    while (sem_wait(&c->slots) < 0 && errno == EINTR)
        ;
}

/* 응답 내용이 보낸 것과 같은지 본다. 다르면 처음 다른 위치를 *bad 에 남긴다 */
static bool payload_ok(const cli_t *c, uint64_t req_id, size_t *bad)
{
    size_t size = c->cfg->size;
    size_t from = 0;
    if (size == 0) return true;
    if (c->cfg->verify && size >= 8) {
        uint64_t got;
        memcpy(&got, c->rbuf, sizeof got);
        if (got != req_id) { *bad = 0; return false; }
        from = 8;
    }
    for (size_t k = from; k < size; k++) {
        if (c->rbuf[k] != c->pattern[k]) { *bad = k; return false; }
    }
    return true;
}

static int send_one(cli_t *c, msg_hdr_t *h, uint64_t i)
{
    slot_take(c);
    if (atomic_load(&c->failed)) return -1;
    h->req_id = i;
    if (c->cfg->verify && c->cfg->size >= 8) memcpy(c->sbuf, &i, sizeof i);
    uint64_t t = now_ns();
    atomic_store_explicit(&c->t0[i], t, memory_order_release);
    if (tp_send_req(h, c->sbuf) < 0) {
        cli_fail(c, "tp_send_req", errno);
        return -1;
    }
    return 0;
}

static void *cli_send_main(void *arg)
{
    cli_t *c = arg;
    msg_hdr_t h;
    memset(&h, 0, sizeof h);
    h.type = BENCH_ECHO;
    h.client_id = c->id;
    h.payload_len = (uint32_t)c->cfg->size;
    if (!wait_start(c)) return NULL;

    for (uint64_t i = 0; i < c->warm; i++)
        if (send_one(c, &h, i) < 0) return NULL;

    /* 시작 신호를 기다린다. recv 스레드가 워밍업 응답을 모두 받은 뒤 main 이 준비 신호를 보낸다 */
    pthread_mutex_lock(&c->mtx);
    c->warm_sent = true;
    pthread_cond_broadcast(&c->cv);
    while (!c->go && !atomic_load(&c->failed)) pthread_cond_wait(&c->cv, &c->mtx);
    pthread_mutex_unlock(&c->mtx);

    for (uint64_t i = c->warm; i < c->warm + c->n; i++)
        if (send_one(c, &h, i) < 0) return NULL;
    return NULL;
}

/* 응답 1개를 받아 확인하고 표본을 남긴다. 성공 0 */
static int recv_one(cli_t *c)
{
    msg_hdr_t h;
    uint64_t total = c->warm + c->n;
    ssize_t r = tp_recv_resp(&h, c->rbuf, c->cfg->size);
    uint64_t t5 = now_ns();
    if (r < 0) {
        cli_fail(c, "tp_recv_resp", errno);
        return -1;
    }
    if ((size_t)r != c->cfg->size || h.type != RESP_OK || h.client_id != c->id || h.req_id >= total) {
        cli_fail(c, "응답 형식이 다름", 0);
        return -1;
    }
    if (c->seen[h.req_id]) {
        cli_fail(c, "같은 응답을 두 번 받음", 0);
        return -1;
    }
    c->seen[h.req_id] = 1;
    uint64_t t0 = atomic_load_explicit(&c->t0[h.req_id], memory_order_acquire);
    if (h.req_id >= c->warm) {
        c->lat[h.req_id - c->warm] = t5 - t0;
        if (t5 > c->res.t_end) c->res.t_end = t5;
    }
    /* 워밍업 응답은 내용을 모두 검사한다. 측정 구간에서는 -V 일 때만 */
    size_t bad = 0;
    if ((c->cfg->verify || h.req_id < c->warm) && !payload_ok(c, h.req_id, &bad)) {
        char what[96];
        snprintf(what, sizeof what, "응답 내용이 다름 (요청 %" PRIu64 ", %zu번째 바이트)", h.req_id, bad);
        cli_fail(c, what, 0);
        return -1;
    }
    sem_post(&c->slots);
    return 0;
}

static void *cli_recv_main(void *arg)
{
    cli_t *c = arg;
    bool ok = wait_start(c);
    for (uint64_t j = 0; ok && j < c->warm; j++)
        ok = recv_one(c) == 0;

    pthread_mutex_lock(&c->mtx);
    if (ok) {
        c->warm_done = true;
        pthread_cond_broadcast(&c->cv);
        while (!c->go && !atomic_load(&c->failed)) pthread_cond_wait(&c->cv, &c->mtx);
        ok = !atomic_load(&c->failed);
    }
    pthread_mutex_unlock(&c->mtx);

    for (uint64_t j = 0; ok && j < c->n; j++)
        ok = recv_one(c) == 0;

    pthread_mutex_lock(&c->mtx);
    c->recv_done = true;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mtx);
    return NULL;
}

static void *cli_stats_main(void *arg)
{
    cli_t *c = arg;
    pthread_mutex_lock(&c->mtx);
    while (!c->recv_done) pthread_cond_wait(&c->cv, &c->mtx);
    pthread_mutex_unlock(&c->mtx);
    usage_now(&c->u_end);
    c->res.vmrss_kb = vm_rss_kb();
    c->res.vmhwm_kb = vm_hwm_kb();
    return NULL;
}

static int client_fail_early(const char *dir, const char *name, result_t *res, const char *what, int e)
{
    snprintf(res->err, sizeof res->err, "%s: %s", what, strerror(e));
    write_result(dir, name, res, NULL, 0);
    return 1;
}

static int run_client(const cfg_t *cfg, uint32_t id, uint64_t n, const char *dir, pid_t driver)
{
    char name[32];
    snprintf(name, sizeof name, "client%u", id);
    cli_t c;
    memset(&c, 0, sizeof c);
    c.cfg = cfg;
    c.id = id;
    c.warm = cfg->warm;
    c.n = n;
    atomic_init(&c.failed, false);

    /* 서버가 뜰 때까지 다시 시도한다 */
    uint64_t deadline = now_ns() + (uint64_t)CONNECT_SEC * 1000000000ull;
    while (tp_client_open(id) < 0) {
        if (errno != ECONNREFUSED || now_ns() > deadline)
            return client_fail_early(dir, name, &c.res, "tp_client_open", errno);
        sleep_ms(2);
    }

    /* 버퍼는 모두 미리 잡는다. 측정 구간에서는 malloc 을 부르지 않는다 */
    size_t bsz = cfg->size ? cfg->size : 1;
    uint64_t total = c.warm + c.n;
    c.sbuf = malloc(bsz);
    c.rbuf = malloc(bsz);
    c.pattern = malloc(bsz);
    c.t0 = calloc(total ? total : 1, sizeof *c.t0);
    c.seen = calloc(total ? total : 1, 1);
    c.lat = calloc(c.n ? c.n : 1, sizeof *c.lat);
    if (!c.sbuf || !c.rbuf || !c.pattern || !c.t0 || !c.seen || !c.lat) {
        tp_client_close();
        return client_fail_early(dir, name, &c.res, "메모리", ENOMEM);
    }
    for (size_t k = 0; k < bsz; k++) c.pattern[k] = (char)((id * 131u + k * 7u) & 0xffu);
    memcpy(c.sbuf, c.pattern, bsz);
    memset(c.rbuf, 0, bsz);

    sem_init(&c.slots, 0, cfg->inflight);
    pthread_mutex_init(&c.mtx, NULL);
    pthread_cond_init(&c.cv, NULL);

    /*
     * 스레드를 먼저 만들고, HELLO 를 주고받은 뒤에 출발시킨다. 스레드를 만들다 실패해도
     * 전송 계층 안에서 막혀 있는 스레드가 없으므로 모두 거두고 tp_client_close 로 닫을 수 있다.
     */
    pthread_t th[3];
    void *(*const th_main[3])(void *) = { cli_recv_main, cli_send_main, cli_stats_main };
    unsigned made = 0;
    for (; made < 3; made++) {
        int e = pthread_create(&th[made], NULL, th_main[made], &c);
        if (e) { cli_fail(&c, "pthread_create", e); break; }
    }

    /*
     * 첫 요청은 HELLO 다 (transport.h 의 약속). 송수신 스레드가 출발하기 전이라 main 만 송수신한다.
     * HELLO 도 측정 메시지와 같은 크기로 보낸다. pipe 버전은 PIPE_BUF 이하 요청을 잠금 없이 쓰므로,
     * 24B HELLO 가 다른 클라이언트의 큰 요청 사이에 끼어들어 요청 스트림이 섞인다 (-V 검사로 확인).
     */
    if (made == 3) {
        msg_hdr_t h;
        memset(&h, 0, sizeof h);
        h.type = REQ_HELLO;
        h.client_id = id;
        h.req_id = HELLO_ID;
        h.payload_len = (uint32_t)cfg->size;
        if (tp_send_req(&h, c.sbuf) < 0) {
            cli_fail(&c, "HELLO 보내기", errno);
        } else {
            errno = 0;
            if (tp_recv_resp(&h, c.rbuf, cfg->size) < 0 || h.req_id != HELLO_ID)
                cli_fail(&c, "HELLO 응답", errno ? errno : EPROTO);
        }
    }
    pthread_mutex_lock(&c.mtx);
    if (!atomic_load(&c.failed)) c.started = true;
    pthread_cond_broadcast(&c.cv);
    pthread_mutex_unlock(&c.mtx);

    /* 워밍업 응답을 모두 받으면 준비 신호를 보내고 시작 신호를 기다린다 */
    pthread_mutex_lock(&c.mtx);
    while (!(c.warm_done && c.warm_sent) && !atomic_load(&c.failed)) pthread_cond_wait(&c.cv, &c.mtx);
    pthread_mutex_unlock(&c.mtx);

    if (!atomic_load(&c.failed)) {
        sigqueue(driver, SIG_READY, (union sigval){ .sival_int = (int)id });
        sigset_t go;
        sigemptyset(&go);
        sigaddset(&go, SIG_GO);
        struct timespec to = { RUN_SEC, 0 };
        if (sigtimedwait(&go, NULL, &to) != SIG_GO) {
            cli_fail(&c, "시작 신호를 받지 못함", 0);
        } else {
            usage_now(&c.u_start);
            c.res.t_start = now_ns();
            pthread_mutex_lock(&c.mtx);
            c.go = true;
            pthread_cond_broadcast(&c.cv);
            pthread_mutex_unlock(&c.mtx);
        }
    }

    for (unsigned i = 0; i < made; i++) pthread_join(th[i], NULL);
    tp_client_close();

    usage_diff(&c.u_end, &c.u_start, &c.res.usage);
    c.res.ok = !atomic_load(&c.failed);
    c.res.n = c.res.ok ? c.n : 0;
    write_result(dir, name, &c.res, c.lat, c.res.n);
    return c.res.ok ? 0 : 1;
}

/* ================================================================
 * 조정 프로세스
 * ================================================================ */

typedef struct {
    pid_t pid;          /* fork 하지 못했으면 0. 0 이나 -1 로 kill 하면 다른 프로세스까지 죽으므로 늘 확인한다 */
    bool  done;
    int   status;
} child_t;

/* 끝난 자식을 모두 거둔다 */
static void reap(child_t *ch, unsigned cnt)
{
    int st;
    pid_t p;
    while ((p = waitpid(-1, &st, WNOHANG)) > 0) {
        for (unsigned i = 0; i < cnt; i++) {
            if (ch[i].pid == p) { ch[i].done = true; ch[i].status = st; }
        }
    }
}

static bool child_ok(const child_t *c)
{
    return c->pid > 0 && c->done && WIFEXITED(c->status) && WEXITSTATUS(c->status) == 0;
}

/* ch[0] 은 서버, ch[1..] 은 클라이언트다. 모두 끝낸다 (클라이언트는 강제로, 서버는 정리할 기회를 준다) */
static void stop_all(child_t *ch, unsigned cnt)
{
    for (unsigned i = 1; i < cnt; i++) if (ch[i].pid > 0 && !ch[i].done) kill(ch[i].pid, SIGKILL);
    for (unsigned i = 1; i < cnt; i++) {
        if (ch[i].pid > 0 && !ch[i].done) { waitpid(ch[i].pid, &ch[i].status, 0); ch[i].done = true; }
    }
    if (ch[0].pid > 0 && !ch[0].done) {
        kill(ch[0].pid, SIGTERM);
        for (int k = 0; k < 500 && !ch[0].done; k++) { reap(ch, cnt); if (!ch[0].done) sleep_ms(10); }
        if (!ch[0].done) {
            kill(ch[0].pid, SIGKILL);
            waitpid(ch[0].pid, &ch[0].status, 0);
            ch[0].done = true;
        }
    }
}

static int read_result(const char *dir, const char *name, result_t *r, uint64_t **samples)
{
    char path[256];
    result_path(path, sizeof path, dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int rc = fread(r, sizeof *r, 1, f) == 1 ? 0 : -1;
    if (rc == 0 && samples) {
        *samples = NULL;
        if (r->n) {
            *samples = malloc(r->n * sizeof **samples);
            if (!*samples || fread(*samples, sizeof **samples, r->n, f) != r->n) rc = -1;
        }
    }
    fclose(f);
    unlink(path);
    return rc;
}

static void remove_dir(const char *dir, unsigned clients)
{
    char path[256], name[32];
    result_path(path, sizeof path, dir, "server");
    unlink(path);
    for (unsigned i = 0; i < clients; i++) {
        snprintf(name, sizeof name, "client%u", i);
        result_path(path, sizeof path, dir, name);
        unlink(path);
    }
    rmdir(dir);
}

static void fill_common(csv_row_t *row, const cfg_t *cfg, const char *role)
{
    csv_row_init(row);
    row->stage = "bench";
    row->ipc = tp_name();
    row->role = role;
    row->experiment = cfg->exp;
    row->commit = cfg->commit;
    row->run = cfg->run;
    row->msg_size = (long)cfg->size;
    row->clients = cfg->clients;
    row->inflight = cfg->inflight;
}

/* 결과를 모아 CSV 두 줄을 쓰고 한 줄 요약을 stderr 에 낸다 */
static int aggregate(const cfg_t *cfg, const char *dir)
{
    result_t sr, cr;
    if (read_result(dir, "server", &sr, NULL) < 0) { fprintf(stderr, "bench: 서버 결과가 없다\n"); return -1; }

    uint64_t *all = malloc((cfg->n ? cfg->n : 1) * sizeof *all);
    if (!all) return -1;
    uint64_t got = 0, t_start = UINT64_MAX, t_end = 0;
    usage_t cu = { 0, 0, 0, 0 };
    long rss = -1, hwm = -1;
    int rc = 0;

    for (unsigned i = 0; i < cfg->clients; i++) {
        char name[32];
        uint64_t *s = NULL;
        snprintf(name, sizeof name, "client%u", i);
        if (read_result(dir, name, &cr, &s) < 0 || !cr.ok) {
            fprintf(stderr, "bench: 클라이언트 %u 결과가 없다\n", i);
            free(s);
            rc = -1;
            continue;
        }
        if (cr.n) {
            if (got + cr.n > cfg->n) { free(s); rc = -1; continue; }
            memcpy(all + got, s, cr.n * sizeof *s);
            got += cr.n;
            if (cr.t_start < t_start) t_start = cr.t_start;
            if (cr.t_end > t_end) t_end = cr.t_end;
        }
        free(s);
        cu.utime_s += cr.usage.utime_s;
        cu.stime_s += cr.usage.stime_s;
        cu.nvcsw += cr.usage.nvcsw;
        cu.nivcsw += cr.usage.nivcsw;
        if (cr.vmrss_kb > rss) rss = cr.vmrss_kb;
        if (cr.vmhwm_kb > hwm) hwm = cr.vmhwm_kb;
    }
    if (rc < 0 || got != cfg->n || !sr.ok) {
        if (!sr.ok && sr.err[0]) fprintf(stderr, "bench: %s\n", sr.err);
        free(all);
        return -1;
    }

    lat_stats_t st;
    lat_stats(all, got, &st);
    double elapsed = got ? (double)(t_end - t_start) / 1e9 : 0;
    double thr = elapsed > 0 ? (double)got / elapsed : 0;

    csv_row_t row;
    fill_common(&row, cfg, "client");
    row.n = (long)got;
    row.elapsed_s = elapsed;
    row.lat_mean_us = st.mean / 1e3;
    row.lat_p50_us = (double)st.p50 / 1e3;
    row.lat_p99_us = (double)st.p99 / 1e3;
    row.lat_max_us = (double)st.max / 1e3;
    row.thr_msg_s = thr;
    row.thr_mb_s = thr * (double)cfg->size / 1e6;
    row.utime_s = cu.utime_s;
    row.stime_s = cu.stime_s;
    row.nvcsw = cu.nvcsw;
    row.nivcsw = cu.nivcsw;
    row.vmrss_kb = rss;
    row.vmhwm_kb = hwm;

    csv_row_t srow;
    fill_common(&srow, cfg, "server");
    srow.n = (long)sr.n;
    srow.elapsed_s = (double)(sr.t_end - sr.t_start) / 1e9;
    srow.utime_s = sr.usage.utime_s;
    srow.stime_s = sr.usage.stime_s;
    srow.nvcsw = sr.usage.nvcsw;
    srow.nivcsw = sr.usage.nivcsw;
    srow.vmrss_kb = sr.vmrss_kb;
    srow.vmhwm_kb = sr.vmhwm_kb;

    if (cfg->out && (csv_append(cfg->out, &row) < 0 || csv_append(cfg->out, &srow) < 0)) {
        fprintf(stderr, "bench: %s 에 쓰지 못함: %s\n", cfg->out, strerror(errno));
        rc = -1;
    }
    fprintf(stderr, "%-4s %-10s s=%-7zu c=%u f=%-2u n=%-6" PRIu64 " mean=%.2fus p50=%.2fus p99=%.2fus max=%.2fus"
            " thr=%.0f msg/s %.1f MB/s\n",
            tp_name(), cfg->exp, cfg->size, cfg->clients, cfg->inflight, got,
            row.lat_mean_us, row.lat_p50_us, row.lat_p99_us, row.lat_max_us, row.thr_msg_s, row.thr_mb_s);
    free(all);
    return rc;
}

/* SIG_READY 를 기다린다. want_srv 이면 서버의 값, 아니면 클라이언트 번호를 ready[] 에 표시한다 */
static int wait_ready(child_t *ch, unsigned cnt, bool *ready, unsigned clients, bool want_srv, int sec)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIG_READY);
    sigaddset(&set, SIGCHLD);
    uint64_t deadline = now_ns() + (uint64_t)sec * 1000000000ull;

    for (;;) {
        bool all = true;
        if (want_srv) all = ready[clients];
        else for (unsigned i = 0; i < clients; i++) all = all && ready[i];
        if (all) return 0;

        reap(ch, cnt);
        for (unsigned i = 0; i < cnt; i++) if (ch[i].done) return -1;   /* 측정 전에 끝난 프로세스가 있다 */
        if (now_ns() > deadline) return -1;

        siginfo_t info;
        struct timespec to = { 0, 100000000L };
        int sig = sigtimedwait(&set, &info, &to);
        if (sig == SIG_READY) {
            int v = info.si_value.sival_int;
            if (v == SRV_TAG) ready[clients] = true;
            else if (v >= 0 && (unsigned)v < clients) ready[v] = true;
        }
    }
}

static int run_driver(const cfg_t *cfg)
{
    char dir[] = "/tmp/canvas_bench_XXXXXX";
    if (!mkdtemp(dir)) { perror("bench: mkdtemp"); return 1; }

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIG_READY);
    sigaddset(&set, SIG_GO);
    sigaddset(&set, SIG_MARK);
    sigaddset(&set, SIGCHLD);
    sigprocmask(SIG_BLOCK, &set, NULL);         /* 자식들도 막힌 채로 시작한다 */

    pid_t driver = getpid();
    unsigned cnt = cfg->clients + 1;
    child_t *ch = calloc(cnt, sizeof *ch);
    bool *ready = calloc(cfg->clients + 1, sizeof *ready);
    if (!ch || !ready) { fprintf(stderr, "bench: 메모리 부족\n"); return 1; }
    fflush(NULL);

    /* ch[0] 은 서버, ch[1..] 은 클라이언트다. fork 가 실패하면 나머지는 띄우지 않고 (pid 0) 실패로 끝낸다 */
    int rc = 0;
    for (unsigned i = 0; i < cnt; i++) {
        pid_t p = fork();
        if (p < 0) {
            perror("bench: fork");
            rc = -1;
            break;
        }
        if (p == 0) {
            if (i == 0) _exit(run_server(dir, driver));
            uint64_t n = cfg->n / cfg->clients + (i - 1 < cfg->n % cfg->clients ? 1 : 0);
            _exit(run_client(cfg, i - 1, n, dir, driver));
        }
        ch[i].pid = p;
    }

    if (rc == 0 && wait_ready(ch, cnt, ready, cfg->clients, false, RUN_SEC) < 0) {
        fprintf(stderr, "bench: 클라이언트가 준비되지 못했다\n");
        rc = -1;
    }
    if (rc == 0) {
        kill(ch[0].pid, SIG_MARK);
        if (wait_ready(ch, cnt, ready, cfg->clients, true, 10) < 0) {
            fprintf(stderr, "bench: 서버가 측정 시작을 기록하지 못했다\n");
            rc = -1;
        }
    }
    if (rc == 0) {
        for (unsigned i = 1; i < cnt; i++) kill(ch[i].pid, SIG_GO);
        /* 측정 중에는 SIGCHLD 가 올 때까지 잠들어 있는다. 조정 프로세스가 CPU 를 쓰지 않게 */
        sigset_t chld;
        sigemptyset(&chld);
        sigaddset(&chld, SIGCHLD);
        uint64_t deadline = now_ns() + (uint64_t)RUN_SEC * 1000000000ull;
        for (;;) {
            reap(ch, cnt);
            bool all = true;
            for (unsigned i = 1; i < cnt; i++) all = all && ch[i].done;
            if (all) break;
            if (ch[0].done || now_ns() > deadline) { rc = -1; break; }
            struct timespec to = { 1, 0 };
            sigtimedwait(&chld, NULL, &to);
        }
        if (rc < 0) fprintf(stderr, "bench: 제한 시간 안에 끝나지 않았다\n");
    }
    stop_all(ch, cnt);

    for (unsigned i = 0; i < cnt; i++) {
        if (!child_ok(&ch[i])) {
            rc = -1;
            if (i > 0) {
                result_t r;
                char name[32];
                snprintf(name, sizeof name, "client%u", i - 1);
                if (read_result(dir, name, &r, NULL) == 0 && r.err[0]) fprintf(stderr, "bench: %s\n", r.err);
            } else {
                result_t r;
                if (read_result(dir, "server", &r, NULL) == 0 && r.err[0]) fprintf(stderr, "bench: %s\n", r.err);
            }
        }
    }
    if (rc == 0 && aggregate(cfg, dir) < 0) rc = -1;

    remove_dir(dir, cfg->clients);
    free(ch);
    free(ready);
    return rc == 0 ? 0 : 1;
}

/* ================================================================
 * clock_gettime 비용
 * ================================================================ */

static int run_clock(const cfg_t *cfg)
{
    uint64_t *v = malloc(cfg->n * sizeof *v);
    if (!v) return 1;
    clock_cost_samples(v, cfg->n);
    lat_stats_t st;
    lat_stats(v, cfg->n, &st);

    csv_row_t row;
    csv_row_init(&row);
    row.stage = "bench";
    row.role = "client";
    row.experiment = "clock";
    row.commit = cfg->commit;
    row.run = cfg->run;
    row.n = (long)cfg->n;
    row.lat_mean_us = st.mean / 1e3;
    row.lat_p50_us = (double)st.p50 / 1e3;
    row.lat_p99_us = (double)st.p99 / 1e3;
    row.lat_max_us = (double)st.max / 1e3;
    int rc = 0;
    if (cfg->out && csv_append(cfg->out, &row) < 0) { perror("bench: csv"); rc = 1; }
    fprintf(stderr, "clock_gettime n=%" PRIu64 " mean=%.1fns p50=%" PRIu64 "ns p99=%" PRIu64 "ns max=%" PRIu64 "ns\n",
            cfg->n, st.mean, st.p50, st.p99, st.max);
    free(v);
    return rc;
}

/* ================================================================
 * main
 * ================================================================ */

static void usage(const char *prog)
{
    fprintf(stderr,
            "사용법: %s [-c 클라이언트수] [-s 바이트] [-n 잴수] [-w 워밍업] [-f 동시요청] [-e 실험] [-r 반복]\n"
            "          [-o 결과.csv] [-C 커밋] [-V] [-K]\n"
            "  -c N   동시 클라이언트 수 (1~%u, 기본 1)\n"
            "  -s B   메시지 payload 바이트 (0~%u, 기본 64)\n"
            "  -n N   잴 메시지 수, 모든 클라이언트 합 (기본 10000)\n"
            "  -w N   클라이언트마다 워밍업 메시지 수 (기본 100)\n"
            "  -f N   클라이언트마다 동시에 띄워 둘 요청 수. 1 이면 지연, 크면 처리량 (기본 1)\n"
            "  -e S   실험 이름 (기본 latency 또는 throughput)\n"
            "  -r N   반복 번호 (기본 1)\n"
            "  -o F   결과 CSV 에 두 줄(client, server)을 덧붙인다\n"
            "  -C S   측정한 커밋 (기본: 환경 변수 BENCH_COMMIT)\n"
            "  -V     모든 응답의 내용을 검사한다 (시험용)\n"
            "  -K     clock_gettime 비용만 잰다 (-n 개 표본)\n",
            prog, MAX_CLIENTS, MAX_PAYLOAD);
}

static int parse_u64(const char *s, uint64_t *out)
{
    char *end;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno || end == s || *end || s[0] == '-') return -1;
    *out = v;
    return 0;
}

int main(int argc, char **argv)
{
    cfg_t cfg = {
        .clients = 1, .size = 64, .n = 10000, .warm = 100, .inflight = 1,
        .exp = NULL, .run = 1, .out = NULL, .commit = getenv("BENCH_COMMIT"),
        .verify = false, .clock_mode = false,
    };
    uint64_t v;
    int opt;
    while ((opt = getopt(argc, argv, "c:s:n:w:f:e:r:o:C:VKh")) != -1) {
        switch (opt) {
        case 'c': if (parse_u64(optarg, &v) < 0 || v < 1 || v > MAX_CLIENTS) goto bad; cfg.clients = (unsigned)v; break;
        case 's': if (parse_u64(optarg, &v) < 0 || v > MAX_PAYLOAD) goto bad; cfg.size = (size_t)v; break;
        case 'n': if (parse_u64(optarg, &v) < 0 || v < 1 || v > 100000000ull) goto bad; cfg.n = v; break;
        case 'w': if (parse_u64(optarg, &v) < 0 || v > 100000000ull) goto bad; cfg.warm = v; break;
        case 'f': if (parse_u64(optarg, &v) < 0 || v < 1 || v > 4096) goto bad; cfg.inflight = (unsigned)v; break;
        case 'e': cfg.exp = optarg; break;
        case 'r': if (parse_u64(optarg, &v) < 0 || v > 1000000) goto bad; cfg.run = (long)v; break;
        case 'o': cfg.out = optarg; break;
        case 'C': cfg.commit = optarg; break;
        case 'V': cfg.verify = true; break;
        case 'K': cfg.clock_mode = true; break;
        case 'h': usage(argv[0]); return 0;
        default: goto bad;
        }
    }
    if (optind != argc) goto bad;
    if (!cfg.exp) cfg.exp = cfg.clock_mode ? "clock" : cfg.inflight == 1 ? "latency" : "throughput";
    if (cfg.clock_mode) return run_clock(&cfg);
    return run_driver(&cfg);

bad:
    usage(argv[0]);
    return 2;
}
