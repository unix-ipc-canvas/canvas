/*
 * server.c - 공유 캔버스 서버 (세 IPC 공통)
 *
 * 클라이언트들의 요청(DRAW, SNAPSHOT, STATS)을 받아 캔버스(canvas.c)에 그리거나 복사해 응답한다.
 * 어떤 IPC 인지는 링크된 전송 계층(src/transport/)이 정한다. 이 파일은 transport.h 의 함수만 부른다.
 * 설계의 원본은 docs/design/README.md 의 "스레드", "공유 자원과 동기화", "종료와 자원 정리" 절이다.
 *
 * 스레드 (설계 명세의 스레드 표, 과제 조건: 프로세스마다 3개 이상)
 *   main    옵션, 캔버스와 채널 만들기, 스레드 만들기와 거두기, 종료 때 PGM 저장·체크섬 출력·tp_server_close
 *   recv    tp_recv_req 로 요청을 받아 수신 시각(t_recv)을 찍고 작업 큐에 넣는다 (-d 이면 직접 처리한다)
 *   worker  작업 큐에서 꺼내(t_deq) 락을 잡고(t_lock) 그리거나 응답 버퍼로 복사한 뒤 락을 놓고, 락 밖에서 보낸다
 *   admin   SIGINT·SIGTERM 을 sigwait 로 받아 tp_server_wake 로 수신 스레드를 깨운다
 *
 * 작업 큐 (설계 명세의 공유 자원 표): 유한 버퍼. mutex 1개 + 조건변수 2개(비지 않음, 가득 차지 않음).
 *   가득 차면 수신 스레드가 기다리므로 요청이 서버 안에 무한히 쌓이지 않는다.
 *   요청은 모두 작아(payload 16B 이하) 큐 칸에 그대로 담는다. 큰 payload 는 칸에 담지 않고 오류로 응답한다.
 *
 * 종료 순서: 신호 → admin 이 tp_server_wake → recv 가 ECANCELED 로 빠져나와 큐를 닫는다 → 워커가 큐에 남은 요청을
 *   모두 처리하고 끝난다 → main 이 PGM 저장과 체크섬 출력 → tp_server_close.
 *
 * DECISION: BYE 에는 응답하지 않는다. 클라이언트가 BYE 의 응답을 기다리지 않고 바로 응답 채널을 닫기 때문이다(client.c).
 * DECISION: STATS 의 n_clients 는 HELLO 를 보낸 적 있는 서로 다른 클라이언트 번호의 수다.
 *   n_draw, n_snapshot 은 성공한 요청만 센다(워밍업의 v = 0 DRAW 포함). bytes_sent 는 보낸 응답 payload 의 합이다.
 * DECISION: -d(큐 없이 수신 스레드가 직접 처리)에서는 워커를 만들지 않으므로 STATS 의 n_workers 를 0 으로 보낸다.
 * DECISION: -O 를 주면 끝날 때 서버 프로세스의 자원 사용량을 CSV 에 한 줄(stage=app, role=server) 붙인다. 서버는 측정 구간을
 *   알 수 없으므로 구간은 준비가 끝난 때부터 수신 스레드가 끝날 때까지다(elapsed_s). n 은 성공한 DRAW + SNAPSHOT 수다.
 * DECISION: HELLO·STATS·오류 응답처럼 캔버스 락을 잡지 않는 응답은 t_lock 을 꺼낸 직후 시각으로 채운다.
 *   그래야 모든 응답의 서버 시각이 t_recv ≤ t_deq ≤ t_lock ≤ t_send 순서를 지킨다 (요구사항 F-6, T-16).
 */
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "canvas.h"
#include "transport.h"
#include "util.h"

#define QUEUE_CAP   1024u           /* 작업 큐 칸 수. 클라이언트 16 × 동시 요청 64 */
#define MAX_WORKERS 64u
#define REQ_PAYLOAD 16u             /* 큐 칸에 담는 요청 payload 의 최대 크기 = sizeof(draw_req_t) */

_Static_assert(sizeof(draw_req_t) <= REQ_PAYLOAD && sizeof(snap_req_t) <= REQ_PAYLOAD, "요청 payload 는 큐 칸에 담긴다");

/* 작업 큐의 칸 하나 = 받은 요청 하나 */
typedef struct {
    msg_hdr_t hdr;
    uint8_t   payload[REQ_PAYLOAD];
    bool      too_big;              /* payload 가 칸보다 커서 담지 못했다 → 오류로 응답 */
    uint64_t  t_recv;
} job_t;

typedef struct {
    job_t           *buf;
    unsigned         head, count;
    bool             closed;        /* 수신 스레드가 끝났다. 남은 것만 처리하고 워커는 끝난다 */
    pthread_mutex_t  mtx;
    pthread_cond_t   not_empty, not_full;
} jobq_t;

static struct {
    /* 옵션 */
    unsigned    workers;
    lock_mode_t lock;
    uint32_t    tile;
    bool        direct;             /* -d */
    const char *pgm;
    const char *csv, *exp, *commit;   /* -O, -e, -C */
    long        run;                  /* -R */

    canvas_t   *canvas;
    jobq_t      q;

    /* STATS (C11 원자적 변수: 값 하나를 더하는 연산뿐이라 락이 필요 없다) */
    _Atomic uint64_t n_draw, n_snap, bytes_sent;
    _Atomic uint32_t seen;          /* HELLO 를 보낸 클라이언트 번호의 비트 집합 */

    pthread_t   admin_th;
} S;

/* ---- 작업 큐 ---- */

static int jobq_init(jobq_t *q)
{
    q->buf = calloc(QUEUE_CAP, sizeof *q->buf);
    if (!q->buf) return -1;
    q->head = q->count = 0;
    q->closed = false;
    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    return 0;
}

static void jobq_destroy(jobq_t *q)
{
    pthread_mutex_destroy(&q->mtx);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
    free(q->buf);
}

/* 가득 차면 빈 칸이 날 때까지 기다린다. 수신 스레드 하나만 부른다 */
static void jobq_put(jobq_t *q, const job_t *j)
{
    pthread_mutex_lock(&q->mtx);
    while (q->count == QUEUE_CAP) pthread_cond_wait(&q->not_full, &q->mtx);
    q->buf[(q->head + q->count) % QUEUE_CAP] = *j;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mtx);
}

/* 꺼낼 것이 있으면 true. 비었고 닫혔으면 false (워커가 끝날 때) */
static bool jobq_get(jobq_t *q, job_t *out)
{
    pthread_mutex_lock(&q->mtx);
    while (q->count == 0 && !q->closed) pthread_cond_wait(&q->not_empty, &q->mtx);
    if (q->count == 0) {
        pthread_mutex_unlock(&q->mtx);
        return false;
    }
    *out = q->buf[q->head];
    q->head = (q->head + 1) % QUEUE_CAP;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mtx);
    return true;
}

static void jobq_close(jobq_t *q)
{
    pthread_mutex_lock(&q->mtx);
    q->closed = true;
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->mtx);
}

/* ---- 요청 하나 처리 ---- */

/*
 * 요청 하나를 처리하고 응답한다. buf 는 MAX_PAYLOAD 바이트의 응답 버퍼(워커마다 하나, 시작할 때 잡는다).
 * t_deq 는 작업 큐에서 꺼낸 시각이다.
 */
static void handle(const job_t *j, uint64_t t_deq, uint8_t *buf)
{
    const msg_hdr_t *in = &j->hdr;
    msg_hdr_t out = { .type = RESP_OK, .client_id = in->client_id, .req_id = in->req_id };
    srv_ts_t ts = { .t_recv = j->t_recv, .t_deq = t_deq };
    uint32_t len = sizeof ts;       /* 모든 응답 payload 는 서버 시각으로 시작한다 */
    bool locked = false;

    switch (in->type) {
    case REQ_HELLO:
        if (in->client_id < MAX_CLIENTS) atomic_fetch_or(&S.seen, 1u << in->client_id);
        break;
    case REQ_DRAW:
        if (j->too_big || in->payload_len != sizeof(draw_req_t)) { out.type = RESP_ERROR; break; }
        if (canvas_draw(S.canvas, (const draw_req_t *)j->payload, &ts.t_lock) < 0) out.type = RESP_ERROR;
        else atomic_fetch_add(&S.n_draw, 1);
        locked = true;
        break;
    case REQ_SNAPSHOT: {
        if (j->too_big || in->payload_len != sizeof(snap_req_t)) { out.type = RESP_ERROR; break; }
        const snap_req_t *s = (const snap_req_t *)j->payload;
        if (canvas_snapshot(S.canvas, s, buf + sizeof ts, &ts.t_lock) < 0) {
            out.type = RESP_ERROR;
        } else {
            len += (uint32_t)s->w * s->h;
            atomic_fetch_add(&S.n_snap, 1);
        }
        locked = true;
        break;
    }
    case REQ_STATS: {
        stats_resp_t st = {
            .n_draw = atomic_load(&S.n_draw),
            .n_snapshot = atomic_load(&S.n_snap),
            .bytes_sent = atomic_load(&S.bytes_sent),
            .n_clients = (uint32_t)__builtin_popcount(atomic_load(&S.seen)),
            .n_workers = S.direct ? 0 : S.workers,
        };
        memcpy(buf + sizeof ts, &st, sizeof st);
        len += sizeof st;
        break;
    }
    case REQ_BYE:
        return;                     /* 응답하지 않는다 (DECISION) */
    default:
        out.type = RESP_ERROR;
        break;
    }

    if (!locked) ts.t_lock = now_ns();
    if (out.type != RESP_OK) len = sizeof ts;   /* 오류 응답은 서버 시각만 */
    out.payload_len = len;
    ts.t_send = now_ns();
    memcpy(buf, &ts, sizeof ts);
    if (tp_send_resp(&out, buf) == 0) atomic_fetch_add(&S.bytes_sent, len);
    /* 실패(EPIPE: 그 클라이언트가 끝났다)는 그 응답만 버리고 계속한다 (transport.h 의 약속) */
}

/* ---- 스레드 ---- */

static void *worker_main(void *arg)
{
    uint8_t *buf = arg;
    job_t j;
    while (jobq_get(&S.q, &j)) handle(&j, now_ns(), buf);
    return NULL;
}

static void *recv_main(void *arg)
{
    uint8_t *direct_buf = arg;      /* -d 일 때 응답 버퍼 */
    uint8_t *in = malloc(MAX_PAYLOAD);
    if (!in) {
        fprintf(stderr, "server_%s: 메모리가 부족하다\n", tp_name());
        jobq_close(&S.q);
        pthread_kill(S.admin_th, SIGUSR1);
        return NULL;
    }
    for (;;) {
        job_t j;
        ssize_t r = tp_recv_req(&j.hdr, in, MAX_PAYLOAD);
        uint64_t t = now_ns();
        if (r < 0) {
            if (errno == EMSGSIZE || errno == EINTR) continue;
            if (errno != ECANCELED) {
                char eb[128];
                fprintf(stderr, "server_%s: 요청을 받지 못해 끝낸다: %s\n", tp_name(), err_str(errno, eb, sizeof eb));
                pthread_kill(S.admin_th, SIGUSR1);  /* admin 도 끝나게 한다 */
            }
            break;
        }
        j.t_recv = t;
        j.too_big = (size_t)r > REQ_PAYLOAD;
        memset(j.payload, 0, sizeof j.payload);
        if (!j.too_big) memcpy(j.payload, in, (size_t)r);
        if (S.direct) handle(&j, now_ns(), direct_buf);
        else jobq_put(&S.q, &j);
    }
    free(in);
    jobq_close(&S.q);
    return NULL;
}

static void *admin_main(void *arg)
{
    (void)arg;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGUSR1);
    for (;;) {
        int sig;
        if (sigwait(&set, &sig) != 0) continue;
        if (sig == SIGUSR1) break;      /* 수신 스레드가 스스로 끝났다 */
        fprintf(stderr, "server_%s: %s 를 받아 끝낸다\n", tp_name(), sig == SIGINT ? "SIGINT" : "SIGTERM");
        tp_server_wake();
        break;
    }
    return NULL;
}

/* ---- main ---- */

static void usage(const char *prog)
{
    fprintf(stderr,
            "사용법: %s [-w 워커수] [-l global|tile|tile-rw|none] [-t 타일한변] [-d] [-o 캔버스.pgm]\n"
            "  -w N   워커 스레드 수, 1~%u (4)\n"
            "  -l S   락 방식 (global)\n"
            "  -t N   타일 한 변, %u~%u 의 2의 거듭제곱 (%u). -l tile, tile-rw 에서만 뜻이 있다\n"
            "  -d     작업 큐 없이 수신 스레드가 직접 처리한다 (큐 비용 확인용)\n"
            "  -o F   종료할 때 캔버스를 저장할 PGM 파일 (canvas_<ipc>.pgm)\n"
            "  -O F   끝날 때 서버의 자원 사용량을 CSV F 에 한 줄 붙인다. -e 실험 이름, -R 반복 번호, -C 커밋(BENCH_COMMIT)\n",
            prog, MAX_WORKERS, TILE_MIN, TILE_MAX, TILE_DEFAULT);
}

static bool parse_uint(const char *s, unsigned long lo, unsigned long hi, unsigned long *out)
{
    char *end;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno || end == s || *end || v < lo || v > hi || s[0] == '-') return false;
    *out = v;
    return true;
}

int main(int argc, char **argv)
{
    /*
     * 신호는 가장 먼저 막는다. 이후에 만드는 스레드(전송 계층 안의 감시 스레드 포함)가 모두 막힌 채 시작하고,
     * admin 스레드만 sigwait 로 받는다.
     */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    S.workers = 4;
    S.lock = LOCK_GLOBAL;
    S.tile = TILE_DEFAULT;
    char pgm_default[64];
    snprintf(pgm_default, sizeof pgm_default, "canvas_%s.pgm", tp_name());
    S.pgm = pgm_default;
    S.run = 1;
    S.commit = getenv("BENCH_COMMIT");

    int opt;
    unsigned long v;
    while ((opt = getopt(argc, argv, "w:l:t:do:O:e:R:C:h")) != -1) {
        switch (opt) {
        case 'w': if (!parse_uint(optarg, 1, MAX_WORKERS, &v)) goto bad; S.workers = (unsigned)v; break;
        case 'l': if (lock_mode_parse(optarg, &S.lock) < 0) goto bad; break;
        case 't': if (!parse_uint(optarg, TILE_MIN, TILE_MAX, &v) || !tile_size_ok((uint32_t)v)) goto bad;
                  S.tile = (uint32_t)v; break;
        case 'd': S.direct = true; break;
        case 'o': S.pgm = optarg; break;
        case 'O': S.csv = optarg; break;
        case 'e': S.exp = optarg; break;
        case 'R': if (!parse_uint(optarg, 0, 1000000, &v)) goto bad; S.run = (long)v; break;
        case 'C': S.commit = optarg; break;
        case 'h': usage(argv[0]); return 0;
        default:  goto bad;
        }
    }
    if (optind != argc) goto bad;

    S.canvas = canvas_create(S.lock, S.tile);
    if (!S.canvas || jobq_init(&S.q) < 0) {
        fprintf(stderr, "server_%s: 캔버스를 만들지 못함: %s\n", tp_name(), strerror(errno));
        return 1;
    }

    unsigned nbuf = S.direct ? 1 : S.workers;
    uint8_t **bufs = calloc(nbuf, sizeof *bufs);    /* 워커마다 응답 버퍼 (설계 명세의 "락을 쥔 채 전송") */
    for (unsigned i = 0; bufs && i < nbuf; i++) {
        bufs[i] = malloc(MAX_PAYLOAD);
        if (!bufs[i]) { fprintf(stderr, "server_%s: 메모리가 부족하다\n", tp_name()); return 1; }
    }
    if (!bufs) { fprintf(stderr, "server_%s: 메모리가 부족하다\n", tp_name()); return 1; }

    if (tp_server_open() < 0) {
        int e = errno;
        if (e == EADDRINUSE)
            fprintf(stderr, "server_%s: 이미 실행 중인 서버가 있다\n", tp_name());
        else
            fprintf(stderr, "server_%s: 요청 채널을 만들지 못함: %s\n", tp_name(), strerror(e));
        return 1;
    }

    pthread_t recv_th, *work_th = calloc(S.workers, sizeof *work_th);
    unsigned started = 0;
    bool admin_ok = work_th && pthread_create(&S.admin_th, NULL, admin_main, NULL) == 0;
    bool ok = admin_ok;
    if (ok && !S.direct) {
        for (; started < S.workers; started++)
            if (pthread_create(&work_th[started], NULL, worker_main, bufs[started]) != 0) { ok = false; break; }
    }
    bool recv_ok = ok && pthread_create(&recv_th, NULL, recv_main, S.direct ? bufs[0] : NULL) == 0;
    usage_t u0, u1, ud;
    uint64_t t_start = now_ns(), t_stop;
    usage_now(&u0);
    if (recv_ok) {
        fprintf(stderr, "server_%s: 준비 (pid %d, 워커 %u%s, 락 %s, 타일 %u)\n", tp_name(), (int)getpid(),
                S.direct ? 0 : S.workers, S.direct ? ", 큐 없음" : "", lock_mode_name(S.lock), S.tile);
        pthread_join(recv_th, NULL);            /* 신호를 받거나 수신이 실패하면 끝난다. 큐는 recv 가 닫는다 */
    } else {
        fprintf(stderr, "server_%s: 스레드를 만들지 못함\n", tp_name());
        if (admin_ok) pthread_kill(S.admin_th, SIGUSR1);
        jobq_close(&S.q);
    }
    if (admin_ok) pthread_join(S.admin_th, NULL);
    for (unsigned i = 0; i < started; i++) pthread_join(work_th[i], NULL);
    t_stop = now_ns();
    usage_now(&u1);
    usage_diff(&u1, &u0, &ud);

    /* 여기서는 그리는 스레드가 모두 끝났다. 락 없이 저장하고 체크섬을 낸다 (F-9) */
    int rc = recv_ok ? 0 : 1;
    if (canvas_save_pgm(S.canvas, S.pgm) < 0) {
        fprintf(stderr, "server_%s: %s 에 저장하지 못함: %s\n", tp_name(), S.pgm, strerror(errno));
        rc = 1;
    }
    printf("checksum " CANVAS_CHECKSUM_FMT "\n", canvas_checksum(S.canvas));
    fflush(stdout);
    fprintf(stderr, "server_%s: DRAW %" PRIu64 ", SNAPSHOT %" PRIu64 ", 캔버스 %s\n", tp_name(),
            atomic_load(&S.n_draw), atomic_load(&S.n_snap), S.pgm);

    if (S.csv && recv_ok) {
        csv_row_t row;
        csv_row_init(&row);
        row.stage = "app";
        row.ipc = tp_name();
        row.role = "server";
        row.experiment = S.exp;
        row.commit = S.commit;
        row.run = S.run;
        row.clients = __builtin_popcount(atomic_load(&S.seen));
        row.workers = S.direct ? 0 : (long)S.workers;
        row.lock = lock_mode_name(S.lock);
        row.tile = S.tile;
        row.n = (long)(atomic_load(&S.n_draw) + atomic_load(&S.n_snap));
        row.elapsed_s = (double)(t_stop - t_start) / 1e9;
        row.utime_s = ud.utime_s;
        row.stime_s = ud.stime_s;
        row.nvcsw = ud.nvcsw;
        row.nivcsw = ud.nivcsw;
        row.vmrss_kb = vm_rss_kb();
        row.vmhwm_kb = vm_hwm_kb();
        if (csv_append(S.csv, &row) < 0) {
            fprintf(stderr, "server_%s: %s 에 쓰지 못함: %s\n", tp_name(), S.csv, strerror(errno));
            rc = 1;
        }
    }

    tp_server_close();
    for (unsigned i = 0; i < nbuf; i++) free(bufs[i]);
    free(bufs);
    free(work_th);
    jobq_destroy(&S.q);
    canvas_destroy(S.canvas);
    return rc;

bad:
    fprintf(stderr, "server: 잘못된 옵션이다 (도움말: %s -h)\n", argv[0]);
    return 1;
}
