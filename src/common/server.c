/*
 * server.c - 다중 그림판 공유 캔버스 서버 (세 IPC 공통. 링크하는 전송 계층만 다르다)
 *
 * 스레드 (docs/design/README.md#스레드-설계)
 *   main       옵션, 그림판·작업 큐, 전송 계층 열기, 스레드 만들고 거두기, 체크섬·PGM·CSV, 자원 정리
 *   수신 1개    tp_recv_req → 받은 시각(t_recv) → 작업 큐에 넣기 (가득 차면 기다림 = 요청 채널에 되밀림)
 *   워커 W개    작업 큐에서 꺼냄(t_deq) → 타일 락(t_lock) → 그리기·복사 → 락 해제 → 응답(t_send)
 *   관리 1개    sigwait: SIGINT·SIGTERM(종료, 두 번째는 강제), SIGUSR1(측정 시작·끝 표시), SIGUSR2(main 이 끝낼 때)
 *
 * 공유 자원: 그림판(타일 락), 작업 큐(mutex + 조건변수 2개), 통계(워커별 칸), 접속 수·종료 상태(atomic).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "proto.h"
#include "transport.h"
#include "util.h"
#include "canvas.h"

/* ---- 설정 ---- */

static uint32_t opt_rooms = 8, opt_workers = 4, opt_tile = TILE_DEFAULT, opt_qcap = 4096, opt_maxcli = MAX_CLIENTS;
static lock_mode_t opt_lock = LOCK_RW;
static const char *opt_pgm, *opt_csv, *opt_exp = "app", *opt_expect;
static long opt_run = 1;
static bool opt_verbose;

/* ---- 작업 큐 (유한 원형 버퍼, 생산자 1 · 소비자 W) ---- */

typedef struct {
    msg_hdr_t hdr;
    union { draw_req_t d; snap_req_t s; uint8_t raw[16]; } p;
    int32_t  plen;              /* 받은 payload 길이. 너무 커서 버린 요청은 -1 */
    uint64_t t_recv;
} job_t;

static struct {
    job_t          *buf;
    uint32_t        cap, head, count;
    bool            closed;
    pthread_mutex_t mtx;
    pthread_cond_t  not_empty, not_full;
} Q = { .mtx = PTHREAD_MUTEX_INITIALIZER, .not_empty = PTHREAD_COND_INITIALIZER, .not_full = PTHREAD_COND_INITIALIZER };

static int queue_push(const job_t *j)
{
    pthread_mutex_lock(&Q.mtx);
    while (Q.count == Q.cap && !Q.closed) pthread_cond_wait(&Q.not_full, &Q.mtx);
    if (Q.closed) { pthread_mutex_unlock(&Q.mtx); return -1; }
    Q.buf[(Q.head + Q.count) % Q.cap] = *j;
    Q.count++;
    pthread_cond_signal(&Q.not_empty);
    pthread_mutex_unlock(&Q.mtx);
    return 0;
}

/* 큐가 닫혔고 비었으면 -1 (워커를 끝낸다). 닫혀도 남은 작업은 끝까지 꺼낸다 */
static int queue_pop(job_t *j)
{
    pthread_mutex_lock(&Q.mtx);
    while (Q.count == 0 && !Q.closed) pthread_cond_wait(&Q.not_empty, &Q.mtx);
    if (Q.count == 0) { pthread_mutex_unlock(&Q.mtx); return -1; }
    *j = Q.buf[Q.head];
    Q.head = (Q.head + 1) % Q.cap;
    Q.count--;
    pthread_cond_signal(&Q.not_full);
    pthread_mutex_unlock(&Q.mtx);
    return 0;
}

static void queue_close(void)
{
    pthread_mutex_lock(&Q.mtx);
    Q.closed = true;
    pthread_cond_broadcast(&Q.not_empty);
    pthread_cond_broadcast(&Q.not_full);
    pthread_mutex_unlock(&Q.mtx);
}

/* ---- 공유 상태 ---- */

static canvas_t C;

/* 워커마다 따로 세고 읽을 때 더한다. 캐시 줄을 다투지 않게 64B 로 맞춘다 */
typedef struct {
    _Alignas(64) atomic_ullong n_draw;
    atomic_ullong n_snap, n_err, n_other, bytes;
} wstat_t;
static wstat_t *WS;

static atomic_int  n_clients;
static atomic_bool stopping;
static atomic_ullong n_dropped;          /* 수신에서 버린 요청 (너무 큰 요청) */

typedef struct { uint64_t draw, snap, err, other, bytes; } counts_t;

static void counts_now(counts_t *c)
{
    memset(c, 0, sizeof *c);
    for (uint32_t i = 0; i < opt_workers; i++) {
        c->draw += atomic_load(&WS[i].n_draw);
        c->snap += atomic_load(&WS[i].n_snap);
        c->err += atomic_load(&WS[i].n_err);
        c->other += atomic_load(&WS[i].n_other);
        c->bytes += atomic_load(&WS[i].bytes);
    }
}

/* 측정 구간 표시 (실행기가 SIGUSR1 로 시작과 끝을 알린다). 관리 스레드만 쓰고, main 은 관리 스레드를 거둔 뒤 읽는다 */
static struct {
    int       marks;
    uint64_t  t0, t1;
    usage_t   u0, u1;
    counts_t  c0, c1;
    tp_info_t info;
    long      rss_kb, hwm_kb;
} M;

/* ---- 수신 스레드 ---- */

static void *recv_main(void *arg)
{
    (void)arg;
    job_t j;
    for (;;) {
        ssize_t n = tp_recv_req(&j.hdr, j.p.raw, sizeof j.p.raw);
        j.t_recv = now_ns();
        if (n < 0) {
            if (errno == ECANCELED) break;               /* tp_server_wake: 종료 */
            if (errno == EMSGSIZE) {                     /* 머리는 읽었다. 워커가 오류로 응답한다 */
                atomic_fetch_add(&n_dropped, 1);
                j.plen = -1;
            } else {
                char eb[128];
                fprintf(stderr, "server_%s: tp_recv_req: %s\n", tp_name(), err_str(errno, eb, sizeof eb));
                break;
            }
        } else {
            j.plen = (int32_t)n;
        }
        if (queue_push(&j) < 0) break;                   /* 큐가 닫혔다: 종료 */
    }
    return NULL;
}

/* ---- 워커 ---- */

typedef struct {
    uint32_t id;
    uint8_t *resp;              /* srv_ts_t + 최대 SNAP_MAX_SIDE² 바이트. 미리 잡아 둔다 */
} worker_t;

/* 요청 하나를 처리하고 응답 payload 길이를 돌려준다. 오류면 *code 에 ERR_* */
static uint32_t handle(worker_t *w, const job_t *j, uint64_t *t_lock, uint32_t *code)
{
    wstat_t *st = &WS[w->id];
    uint8_t *out = w->resp + sizeof(srv_ts_t);
    const msg_hdr_t *h = &j->hdr;
    *code = 0;
    *t_lock = 0;

    if (j->plen < 0) { *code = ERR_SIZE; return 0; }
    switch (h->type) {
    case REQ_HELLO:
        atomic_fetch_add(&n_clients, 1);
        atomic_fetch_add(&st->n_other, 1);
        return 0;
    case REQ_BYE:
        atomic_fetch_sub(&n_clients, 1);
        atomic_fetch_add(&st->n_other, 1);
        return 0;
    case REQ_STATS: {
        counts_t c;
        counts_now(&c);
        stats_resp_t s = {
            .n_draw = c.draw, .n_snapshot = c.snap, .n_error = c.err, .bytes_sent = c.bytes,
            .n_clients = (uint32_t)atomic_load(&n_clients), .n_workers = opt_workers, .n_rooms = opt_rooms,
        };
        memcpy(out, &s, sizeof s);
        atomic_fetch_add(&st->n_other, 1);
        return sizeof s;
    }
    case REQ_DRAW: {
        if ((size_t)j->plen != sizeof(draw_req_t)) { *code = ERR_SIZE; return 0; }
        const draw_req_t *d = &j->p.d;
        int e = canvas_check(&C, h->room, d->x, d->y, d->w, d->h);
        if (e) { *code = (uint32_t)e; return 0; }
        canvas_lock(&C, h->room, d->x, d->y, d->w, d->h, true);
        *t_lock = now_ns();
        canvas_draw(&C, h->room, d->x, d->y, d->w, d->h, d->v);
        canvas_unlock(&C, h->room, d->x, d->y, d->w, d->h, true);
        atomic_fetch_add(&st->n_draw, 1);
        return 0;
    }
    case REQ_SNAPSHOT: {
        if ((size_t)j->plen != sizeof(snap_req_t)) { *code = ERR_SIZE; return 0; }
        const snap_req_t *s = &j->p.s;
        int e = canvas_check(&C, h->room, s->x, s->y, s->w, s->h);
        if (e) { *code = (uint32_t)e; return 0; }
        if (s->w > SNAP_MAX_SIDE || s->h > SNAP_MAX_SIDE) { *code = ERR_SIZE; return 0; }
        canvas_lock(&C, h->room, s->x, s->y, s->w, s->h, false);
        *t_lock = now_ns();
        canvas_copy(&C, h->room, s->x, s->y, s->w, s->h, out);
        canvas_unlock(&C, h->room, s->x, s->y, s->w, s->h, false);
        atomic_fetch_add(&st->n_snap, 1);
        return (uint32_t)s->w * s->h;
    }
    default:
        *code = ERR_TYPE;
        return 0;
    }
}

static void *worker_main(void *arg)
{
    worker_t *w = arg;
    job_t j;
    while (queue_pop(&j) == 0) {
        uint64_t t_deq = now_ns(), t_lock;
        uint32_t code;
        uint32_t dlen = handle(w, &j, &t_lock, &code);
        if (t_lock == 0) t_lock = now_ns();              /* 락을 잡지 않는 요청 */

        msg_hdr_t rh = j.hdr;
        rh.type = code ? RESP_ERROR : RESP_OK;
        if (code) {
            memcpy(w->resp + sizeof(srv_ts_t), &code, sizeof code);
            dlen = sizeof code;
            atomic_fetch_add(&WS[w->id].n_err, 1);
        }
        rh.payload_len = (uint32_t)sizeof(srv_ts_t) + dlen;
        srv_ts_t *ts = (srv_ts_t *)w->resp;
        ts->t_recv = j.t_recv;
        ts->t_deq = t_deq;
        ts->t_lock = t_lock;
        ts->t_send = now_ns();
        if (tp_send_resp(&rh, w->resp) == 0)
            atomic_fetch_add(&WS[w->id].bytes, rh.payload_len);
        /* 실패(EPIPE)는 그 클라이언트가 끝난 것이다. 그 응답만 버린다 */
    }
    return NULL;
}

/* ---- 관리 스레드 ---- */

static pthread_t mgr_th;

static void mark(void)
{
    if (M.marks == 0) {
        counts_now(&M.c0);
        usage_now(&M.u0);
        M.t0 = now_ns();
    } else {
        M.t1 = now_ns();
        usage_now(&M.u1);
        counts_now(&M.c1);
        tp_server_info(&M.info);
        M.rss_kb = vm_rss_kb();
        M.hwm_kb = vm_hwm_kb();
    }
    M.marks++;
}

static void *mgr_main(void *arg)
{
    (void)arg;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGUSR2);
    int nstop = 0;
    for (;;) {
        int sig;
        if (opt_verbose) {
            struct timespec ts = { 1, 0 };
            sig = sigtimedwait(&set, NULL, &ts);
            if (sig < 0) {
                counts_t c;
                counts_now(&c);
                fprintf(stderr, "server_%s: 접속 %d, DRAW %llu, SNAPSHOT %llu, 오류 %llu\n", tp_name(),
                        atomic_load(&n_clients), (unsigned long long)c.draw, (unsigned long long)c.snap,
                        (unsigned long long)c.err);
                continue;
            }
        } else if (sigwait(&set, &sig) != 0) {
            continue;
        }
        if (sig == SIGUSR2) break;                       /* main 이 모두 거뒀다 */
        if (sig == SIGUSR1) { mark(); continue; }
        if (++nstop == 1) {
            /*
             * 새 요청을 멈춘다. 수신 스레드를 깨워 끝낸다. 큐는 main 이 수신 스레드를 거둔 뒤에 닫는다.
             * 그래야 이미 받은 요청은 워커가 모두 처리하고 응답한다 (요구사항 F-8).
             */
            atomic_store(&stopping, true);
            tp_server_wake();
            fprintf(stderr, "server_%s: 종료 중 (한 번 더 보내면 기다리지 않고 끝낸다)\n", tp_name());
        } else {
            /* 응답을 받지 않는 클라이언트 때문에 워커가 돌아오지 않는다. 자원만 지우고 끝낸다 (요구사항 F-8) */
            fprintf(stderr, "server_%s: 강제 종료\n", tp_name());
            tp_server_close();
            _exit(1);
        }
    }
    return NULL;
}

/* ---- main ---- */

static void usage(const char *prog)
{
    fprintf(stderr,
            "사용법: %s [-K 그림판] [-w 워커] [-l rw|mutex|room|none] [-t 타일] [-q 큐] [-N 클라이언트]\n"
            "          [-p PGM|-] [-O CSV -e 실험 -R 반복] [-X 검증기출력] [-v]\n"
            "  -K 1~%u (8)   -w 1~64 (4)   -l rw   -t 16~1024 2의 거듭제곱 (64)   -q 큐 칸 (4096)   -N 1~%u (%u)\n",
            prog, MAX_ROOMS, MAX_CLIENTS, MAX_CLIENTS);
}

static int parse_u32(const char *s, uint32_t lo, uint32_t hi, uint32_t *out)
{
    char *end;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno || *end || v < lo || v > hi) return -1;
    *out = (uint32_t)v;
    return 0;
}

/*
 * -X 파일(검증기 출력)과 그림판별 체크섬을 비교한다. 1 = 같음, 0 = 다름, -1 = 파일 없음.
 * 측정 스크립트가 서버를 끝내기 전에 검증기를 돌려 두면 CSV 서버 줄의 checksum_ok 칸에 남는다.
 */
static long check_expected(void)
{
    if (!opt_expect) return -1;
    FILE *f = fopen(opt_expect, "r");
    if (!f) return -1;
    char line[128];
    uint32_t seen = 0;
    long ok = 1;
    while (fgets(line, sizeof line, f)) {
        unsigned r;
        unsigned long long v;
        if (sscanf(line, "checksum room=%u %llx", &r, &v) == 2) {
            if (r >= opt_rooms || canvas_checksum(&C, r) != v) ok = 0;
            seen++;
        }
    }
    fclose(f);
    return seen == opt_rooms ? ok : 0;
}

static void write_csv(long checksum_ok)
{
    app_row_t r;
    app_row_init(&r);
    r.ipc = tp_name();
    r.role = "server";
    r.experiment = opt_exp;
    r.commit = getenv("BENCH_COMMIT");
    r.run = opt_run;
    r.rooms = opt_rooms;
    r.workers = opt_workers;
    r.lock = lock_mode_name(opt_lock);
    r.tile = opt_tile;
    r.env = getenv("OSP_ENV_NOTE");
    r.checksum_ok = checksum_ok;
    /* 클라이언트 쪽 조건은 서버가 모르므로 측정 스크립트가 환경 변수로 알려 준 값을 그대로 적는다 (줄을 짝짓는 데만 쓴다) */
    const char *e;
    if ((e = getenv("OSP_CLIENTS"))) r.clients = atol(e);
    if ((e = getenv("OSP_INFLIGHT"))) r.inflight = atol(e);
    if ((e = getenv("OSP_RATE"))) r.rate_hz = atof(e);
    if ((e = getenv("OSP_DRAW_PCT"))) r.draw_pct = atol(e);
    if ((e = getenv("OSP_RECT"))) r.rect = atol(e);
    if ((e = getenv("OSP_SNAP"))) r.snap = atol(e);
    if (M.marks >= 2) {
        usage_t u;
        usage_diff(&M.u1, &M.u0, &u);
        r.elapsed_s = (double)(M.t1 - M.t0) / 1e9;
        r.n = (long)((M.c1.draw + M.c1.snap + M.c1.other) - (M.c0.draw + M.c0.snap + M.c0.other));
        r.errors = (long)(M.c1.err - M.c0.err);
        r.draw_n = (long)(M.c1.draw - M.c0.draw);
        r.snap_n = (long)(M.c1.snap - M.c0.snap);
        if (r.elapsed_s > 0) {
            r.thr_req_s = (double)r.n / r.elapsed_s;
            r.thr_mb_s = (double)(M.c1.bytes - M.c0.bytes) / r.elapsed_s / 1e6;
        }
        r.utime_s = u.utime_s;
        r.stime_s = u.stime_s;
        r.nvcsw = u.nvcsw;
        r.nivcsw = u.nivcsw;
        r.vmrss_kb = M.rss_kb;
        r.vmhwm_kb = M.hwm_kb;
        r.ipc_cap_kb = (long)(M.info.cap_bytes / 1024);
        r.ipc_rss_kb = (long)(M.info.rss_bytes / 1024);
        r.ipc_min_chan = M.info.min_chan;
    }
    if (app_csv_append(opt_csv, &r) < 0) {
        char eb[128];
        fprintf(stderr, "server_%s: CSV %s: %s\n", tp_name(), opt_csv, err_str(errno, eb, sizeof eb));
    }
}

int main(int argc, char **argv)
{
    int c;
    while ((c = getopt(argc, argv, "K:w:l:t:q:N:p:O:e:R:X:vh")) != -1) {
        switch (c) {
        case 'K': if (parse_u32(optarg, 1, MAX_ROOMS, &opt_rooms) < 0) goto bad; break;
        case 'w': if (parse_u32(optarg, 1, 64, &opt_workers) < 0) goto bad; break;
        case 'l': if (lock_mode_parse(optarg, &opt_lock) < 0) goto bad; break;
        case 't': if (parse_u32(optarg, TILE_MIN, TILE_MAX, &opt_tile) < 0 || (opt_tile & (opt_tile - 1))) goto bad; break;
        case 'q': if (parse_u32(optarg, 1, 1u << 20, &opt_qcap) < 0) goto bad; break;
        case 'N': if (parse_u32(optarg, 1, MAX_CLIENTS, &opt_maxcli) < 0) goto bad; break;
        case 'p': opt_pgm = optarg; break;
        case 'O': opt_csv = optarg; break;
        case 'e': opt_exp = optarg; break;
        case 'R': opt_run = strtol(optarg, NULL, 10); break;
        case 'X': opt_expect = optarg; break;
        case 'v': opt_verbose = true; break;
        case 'h': usage(argv[0]); return 0;
        default: goto bad;
        }
    }
    if (optind != argc) goto bad;

    char pgm_default[64];
    if (!opt_pgm) { snprintf(pgm_default, sizeof pgm_default, "canvas_%s.pgm", tp_name()); opt_pgm = pgm_default; }

    if (canvas_init(&C, opt_rooms, opt_tile, opt_lock) < 0) { perror("server: 그림판"); return 1; }
    Q.cap = opt_qcap;
    Q.buf = calloc(opt_qcap, sizeof *Q.buf);
    WS = aligned_alloc(64, sizeof *WS * opt_workers);
    worker_t *wk = calloc(opt_workers, sizeof *wk);
    if (!Q.buf || !WS || !wk) { fprintf(stderr, "server: 메모리 부족\n"); return 1; }
    memset(WS, 0, sizeof *WS * opt_workers);
    for (uint32_t i = 0; i < opt_workers; i++) {
        wk[i].id = i;
        wk[i].resp = malloc(sizeof(srv_ts_t) + SNAP_MAX_SIDE * SNAP_MAX_SIDE);
        if (!wk[i].resp) { fprintf(stderr, "server: 메모리 부족\n"); return 1; }
    }

    /* 신호는 관리 스레드만 받는다. 만들기 전에 막아 두면 모든 스레드가 막힌 채로 시작한다 */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    if (tp_set_limits(opt_maxcli, APP_MAX_REQ, APP_MAX_RESP) < 0 || tp_server_open() < 0) {
        char eb[128];
        if (errno == EADDRINUSE) fprintf(stderr, "server_%s: 서버가 이미 떠 있다\n", tp_name());
        else fprintf(stderr, "server_%s: 전송 계층 열기: %s\n", tp_name(), err_str(errno, eb, sizeof eb));
        return 1;
    }

    pthread_t rth, *wth = calloc(opt_workers, sizeof *wth);
    int rc = pthread_create(&mgr_th, NULL, mgr_main, NULL);
    if (rc == 0) rc = pthread_create(&rth, NULL, recv_main, NULL);
    for (uint32_t i = 0; rc == 0 && i < opt_workers; i++) rc = pthread_create(&wth[i], NULL, worker_main, &wk[i]);
    if (rc != 0) {
        fprintf(stderr, "server_%s: 스레드 만들기 실패\n", tp_name());
        tp_server_close();
        return 1;
    }
    fprintf(stderr, "server_%s: 시작 (pid %d, 그림판 %u, 워커 %u, 락 %s, 타일 %u, 클라이언트 %u)\n", tp_name(),
            (int)getpid(), opt_rooms, opt_workers, lock_mode_name(opt_lock), opt_tile, opt_maxcli);

    pthread_join(rth, NULL);
    queue_close();                                       /* 받은 요청은 남김없이 처리한 뒤 워커가 끝난다 */
    for (uint32_t i = 0; i < opt_workers; i++) pthread_join(wth[i], NULL);
    pthread_kill(mgr_th, SIGUSR2);
    pthread_join(mgr_th, NULL);

    /* 모든 스레드가 끝났다. 결과를 쓰고 자원을 지운다 */
    counts_t ct;
    counts_now(&ct);
    for (uint32_t r = 0; r < opt_rooms; r++)
        printf("checksum room=%u %016llx\n", r, (unsigned long long)canvas_checksum(&C, r));
    printf("checksum %016llx\n", (unsigned long long)canvas_checksum_all(&C));
    printf("처리: DRAW %llu, SNAPSHOT %llu, 오류 %llu, 기타 %llu, 버린 요청 %llu\n", (unsigned long long)ct.draw,
           (unsigned long long)ct.snap, (unsigned long long)ct.err, (unsigned long long)ct.other,
           (unsigned long long)atomic_load(&n_dropped));
    fflush(stdout);
    if (strcmp(opt_pgm, "-") != 0 && canvas_save_pgm(&C, 0, opt_pgm) < 0)
        fprintf(stderr, "server_%s: PGM %s 저장 실패\n", tp_name(), opt_pgm);
    long cok = check_expected();
    if (cok >= 0) printf("검증기와 비교: %s\n", cok ? "같음" : "다름");
    fflush(stdout);
    if (opt_csv) write_csv(cok);
    tp_server_close();
    fprintf(stderr, "server_%s: 끝\n", tp_name());

    for (uint32_t i = 0; i < opt_workers; i++) free(wk[i].resp);
    free(wk);
    free(wth);
    free(WS);
    free(Q.buf);
    canvas_free(&C);
    return 0;

bad:
    usage(argv[0]);
    return 2;
}
