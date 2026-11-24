/*
 * server.c - 공유 캔버스 서버 (세 IPC 공통)
 *
 * 클라이언트들의 DRAW, SNAPSHOT, STATS 요청을 받아 하나의 캔버스(canvas.c)에 처리하고 응답한다.
 * 어떤 IPC 인지는 링크된 전송 계층(src/transport/)이 정한다. 이 파일은 transport.h 의 함수만 부른다.
 *
 * 스레드 (설계 명세의 스레드 표, 과제 조건: 프로세스마다 3개 이상)
 *   main    옵션, 캔버스와 채널 만들기, 스레드 만들기와 거두기, 끝날 때 PGM 저장·체크섬 출력·tp_server_close
 *   recv    tp_recv_req 로 요청을 받아 t_recv 를 찍고 작업 큐에 넣는다. -d 이면 큐 없이 직접 처리한다
 *   worker  큐에서 꺼내 t_deq 를 찍고, 락 안에서 그리거나 응답 버퍼로 복사만 한 뒤 락 밖에서 응답을 보낸다 (-w 개)
 *   admin   SIGINT·SIGTERM 을 sigwait 로 받아 tp_server_wake 로 수신 스레드를 깨운다
 *
 * 작업 큐 (설계 명세의 공유 자원 표)
 *   QUEUE_CAP 칸의 원형 버퍼 + mutex 1개 + 조건변수 2개(비지 않음, 가득 차지 않음).
 *   가득 차면 수신 스레드가 기다리므로 요청이 서버 안에 무한히 쌓이지 않는다.
 *   수신 스레드가 끝나면 큐를 닫는다. 워커는 남은 요청을 모두 처리한 뒤 끝난다.
 *
 * 측정 (docs/measurement/README.md 의 타임스탬프 위치)
 *   t_recv = tp_recv_req 가 돌아온 직후, t_deq = 큐에서 꺼낸 직후, t_lock = canvas.c 가 락을 모두 잡은 직후,
 *   t_send = tp_send_resp 호출 직전. 락이 필요 없는 요청(HELLO, STATS, 오류)은 t_lock 을 꺼낸 뒤 바로 찍는다.
 *   요청을 처리하는 경로에는 출력과 malloc 이 없다. 응답 버퍼는 워커마다 MAX_PAYLOAD 크기로 시작할 때 잡는다.
 *
 * 끝내기 (설계 명세의 종료와 자원 정리)
 *   admin 이 신호를 받음 → tp_server_wake → recv 가 ECANCELED 로 빠져나오며 큐를 닫음 → 워커가 큐를 비우고 끝남
 *   → main 이 PGM 저장과 체크섬 출력 → tp_server_close.
 *   스레드가 복구할 수 없는 오류를 만나면 오류를 적고 begin_stop 으로 같은 길을 시작한다(종료 코드 1).
 *   admin 은 첫 신호 뒤에도 신호를 기다린다. 두 번째 신호를 받으면 남은 응답을 기다리지 않고 tp_server_close 를 부른 뒤
 *   _exit(1) 한다. main 과 admin 중 먼저 close_mtx 를 잡은 쪽만 닫는다.
 *
 * DECISION: 두 번째 신호로 끝낼 때는 송신 중인 워커를 거두지 않은 채 tp_server_close 를 부르고 _exit 한다.
 *   멈춘 클라이언트(kill -STOP, Ctrl+Z)에게 보내던 워커는 돌아오지 않아 첫 신호만으로는 끝나지 않는데,
 *   transport.h 에는 막힌 송신을 깨우는 함수가 없다. "송신 스레드가 빠져나온 뒤에 닫는다"는 약속을 어기지만
 *   프로세스가 곧바로 끝나므로 닫은 자원을 다시 쓰지 않는다(client.c 의 두 번째 신호와 같은 판단).
 *   캔버스가 그리는 도중일 수 있어 PGM 과 체크섬은 쓰지 않는다. pipe 버전의 닫기는 송신 락을 기다리지 않는다.
 *   그래서 이 경로에서는 ThreadSanitizer 가 닫기(fd 닫기, 버퍼 해제)와 아직 전송 계층 안에 있는 스레드 사이의 경쟁을
 *   보고한다(pipe·mq). 정상 종료 경로에서는 보고가 없다.
 *
 * DECISION: 통계 카운터(n_draw, n_snapshot)는 응답을 보내기 전에 올린다. 보낸 뒤에 올리면, 응답을 받은 클라이언트가
 *   곧바로 보낸 STATS 를 다른 워커가 먼저 처리해 방금 처리한 요청이 빠진 값을 돌려줄 수 있다(T-16).
 *   bytes_sent 는 보내기에 성공한 응답 payload(서버 시각 포함)의 합이라 보낸 뒤에 더한다.
 * DECISION: STATS 의 n_clients 는 지금 접속해 있는 클라이언트 수다(HELLO 를 받고 BYE 를 받지 않은 번호의 수).
 *   BYE 없이 죽은 클라이언트는 같은 번호로 다시 HELLO 할 때까지 접속한 것으로 센다.
 * DECISION: BYE 에는 응답하지 않는다. 클라이언트가 BYE 의 응답을 기다리지 않고 곧 응답 채널을 닫는다(client.c).
 * DECISION: 요청 payload 는 가장 큰 응용 요청(draw_req_t, 16B)까지만 받는다. 더 큰 요청은 전송 계층이 버리고
 *   EMSGSIZE 를 돌려주며, 어느 요청인지 알 수 없어 응답하지 않고 버린 수만 센다. 응용 클라이언트는 만들지 않는 요청이다.
 *   payload 길이가 종류와 맞지 않거나, 영역이 캔버스를 벗어나거나 w·h 가 0 이거나, 모르는 종류면 RESP_ERROR 로 응답한다(F-5).
 * DECISION: -d 이면 워커를 만들지 않고 수신 스레드가 꺼낸 자리에서 처리한다. t_deq 는 t_recv 바로 뒤에 찍는다.
 * DECISION: -O 를 주면 끝날 때 서버 프로세스의 자원 사용량을 CSV 에 한 줄(stage=app, role=server) 붙인다(측정 방법의
 *   자원 사용량: 서버와 클라이언트 양쪽에서 잰다). 서버는 클라이언트의 측정 구간을 모르므로 구간은 스레드를 만들기
 *   직전부터 모두 거둔 뒤까지다(elapsed_s, CPU 시간, 컨텍스트 스위치). n 은 성공한 DRAW + SNAPSHOT 수(클라이언트의 워밍업 포함), clients 는
 *   실행 동안 HELLO 를 보낸 서로 다른 번호의 수다. 오류로 끝난 실행은 쓰지 않는다.
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

#include <sys/resource.h>

#include "canvas.h"
#include "transport.h"
#include "util.h"

#define QUEUE_CAP       256u
#define MAX_WORKERS     64u
#define WORKERS_DEFAULT 4u          /* 워크로드 명세의 기준 조건 */
#define REQ_CAP         sizeof(draw_req_t)

/* 큐의 칸 하나 = 받은 요청 하나 */
typedef struct {
    msg_hdr_t hdr;
    uint32_t  len;                  /* 실제로 받은 payload 바이트 */
    uint64_t  t_recv;
    union {
        draw_req_t draw;
        snap_req_t snap;
        uint8_t    raw[REQ_CAP];
    } p;
} job_t;

static struct {
    /* 설정 (스레드를 만들기 전에 정해지고 바뀌지 않는다) */
    unsigned         workers;
    lock_mode_t      lock;
    uint32_t         tile;
    bool             direct;
    const char      *pgm;
    const char      *csv, *exp, *commit;    /* -O, -e, -C */
    long             run;           /* -R */
    canvas_t        *cv[MAX_ROOMS];  /* 그림판(방)마다 캔버스 하나. 락도 방마다 따로다 */
    uint32_t         rooms;
    uint8_t        **buf;           /* 워커마다(-d 이면 수신 스레드 하나) MAX_PAYLOAD 응답 버퍼 */

    /* 작업 큐 */
    job_t            q[QUEUE_CAP];
    unsigned         head, count;
    bool             closed;
    pthread_mutex_t  qmtx;
    pthread_cond_t   not_empty, not_full;

    /* 통계. 값 하나를 더하는 연산뿐이라 원자적 변수로 둔다 */
    _Atomic uint64_t n_draw, n_snap, bytes_sent, n_err, n_drop, n_gone;
    atomic_bool      connected[MAX_CLIENTS];
    _Atomic uint32_t client_room[MAX_CLIENTS]; /* HELLO 에서 입장한 그림판(방) 번호 */
    _Atomic uint32_t n_clients;
    atomic_bool      seen_id[MAX_CLIENTS];
    _Atomic uint32_t seen;          /* HELLO 를 보낸 적 있는 번호의 수 (CSV 의 clients) */

    /* 끝내기 */
    pthread_mutex_t  err_mtx;
    char             err[256];
    atomic_int       signo;
    atomic_bool      stopping;      /* 종료 절차를 시작했다 */
    atomic_bool      forced;        /* 두 번째 신호로 닫는 중이다. 워커는 더 보내지 않는다 */
    pthread_mutex_t  close_mtx;     /* tp_server_close 를 한 번만 부른다 (main 과 admin) */
    bool             tp_closed;
} S = {
    .qmtx = PTHREAD_MUTEX_INITIALIZER,
    .not_empty = PTHREAD_COND_INITIALIZER,
    .not_full = PTHREAD_COND_INITIALIZER,
    .err_mtx = PTHREAD_MUTEX_INITIALIZER,
    .close_mtx = PTHREAD_MUTEX_INITIALIZER,
};

/* 종료 절차를 시작한다: 수신 스레드를 깨운다. 여러 번 불러도 한 번만 한다 */
static void begin_stop(void)
{
    if (!atomic_exchange(&S.stopping, true)) tp_server_wake();     /* 그 뒤의 tp_recv_req 는 모두 ECANCELED */
}

/* 처음 생긴 오류만 적고 종료 절차를 시작한다 */
static void fatal(const char *what, int e)
{
    char eb[128];
    pthread_mutex_lock(&S.err_mtx);
    if (!S.err[0]) snprintf(S.err, sizeof S.err, "%s: %s", what, err_str(e, eb, sizeof eb));
    pthread_mutex_unlock(&S.err_mtx);
    begin_stop();
}

/* ---- 작업 큐 ---- */

static void queue_put(const job_t *j)
{
    pthread_mutex_lock(&S.qmtx);
    while (S.count == QUEUE_CAP && !S.closed) pthread_cond_wait(&S.not_full, &S.qmtx);
    if (!S.closed) {
        S.q[(S.head + S.count) % QUEUE_CAP] = *j;
        S.count++;
        pthread_cond_signal(&S.not_empty);
    }
    pthread_mutex_unlock(&S.qmtx);
}

/* 하나 꺼낸다. 큐가 닫혔고 비었으면 false */
static bool queue_get(job_t *j)
{
    pthread_mutex_lock(&S.qmtx);
    while (S.count == 0 && !S.closed) pthread_cond_wait(&S.not_empty, &S.qmtx);
    if (S.count == 0) {
        pthread_mutex_unlock(&S.qmtx);
        return false;
    }
    *j = S.q[S.head];
    S.head = (S.head + 1) % QUEUE_CAP;
    S.count--;
    pthread_cond_signal(&S.not_full);
    pthread_mutex_unlock(&S.qmtx);
    return true;
}

static void queue_close(void)
{
    pthread_mutex_lock(&S.qmtx);
    S.closed = true;
    pthread_cond_broadcast(&S.not_empty);
    pthread_cond_broadcast(&S.not_full);
    pthread_mutex_unlock(&S.qmtx);
}

/* ---- 요청 처리 ---- */

/* 요청 하나를 처리하고 응답한다. buf 는 이 스레드의 MAX_PAYLOAD 응답 버퍼다 */
static void handle(const job_t *j, uint64_t t_deq, uint8_t *buf)
{
    uint32_t id = j->hdr.client_id;
    if (id >= MAX_CLIENTS) {        /* 응답을 보낼 곳이 없다 */
        atomic_fetch_add(&S.n_drop, 1);
        return;
    }

    srv_ts_t ts = { .t_recv = j->t_recv, .t_deq = t_deq, .t_lock = 0, .t_send = 0 };
    msg_hdr_t h = j->hdr;
    h.type = RESP_OK;
    h.payload_len = sizeof ts;
    h.reserved = 0;
    bool ok = true;

    switch (j->hdr.type) {
    case REQ_HELLO:
        if (j->len != 0 || j->hdr.reserved >= S.rooms) {
            ok = false;
            break;
        }
        atomic_store(&S.client_room[id], j->hdr.reserved);
        if (!atomic_exchange(&S.connected[id], true)) atomic_fetch_add(&S.n_clients, 1);
        if (!atomic_exchange(&S.seen_id[id], true)) atomic_fetch_add(&S.seen, 1);
        ts.t_lock = now_ns();
        break;

    case REQ_DRAW:
        if (!atomic_load(&S.connected[id]) || j->hdr.reserved >= S.rooms ||
            j->hdr.reserved != atomic_load(&S.client_room[id]) || j->len != sizeof(draw_req_t) ||
            canvas_draw(S.cv[j->hdr.reserved], &j->p.draw, &ts.t_lock) < 0) {
            ok = false;
            break;
        }
        atomic_fetch_add(&S.n_draw, 1);
        break;

    case REQ_SNAPSHOT: {
        const snap_req_t *q = &j->p.snap;
        if (!atomic_load(&S.connected[id]) || j->hdr.reserved >= S.rooms ||
            j->hdr.reserved != atomic_load(&S.client_room[id]) || j->len != sizeof(snap_req_t) ||
            !canvas_rect_ok(q->x, q->y, q->w, q->h) ||
            canvas_snapshot(S.cv[j->hdr.reserved], q, buf + sizeof ts, &ts.t_lock) < 0) {
            ok = false;
            break;
        }
        h.payload_len += (uint32_t)q->w * q->h;
        atomic_fetch_add(&S.n_snap, 1);
        break;
    }

    case REQ_STATS: {
        if (!atomic_load(&S.connected[id]) || j->len != 0) {
            ok = false;
            break;
        }
        ts.t_lock = now_ns();
        stats_resp_t st = {
            .n_draw = atomic_load(&S.n_draw),
            .n_snapshot = atomic_load(&S.n_snap),
            .bytes_sent = atomic_load(&S.bytes_sent),
            .n_clients = atomic_load(&S.n_clients),
            .n_workers = S.direct ? 0 : S.workers,
        };
        memcpy(buf + sizeof ts, &st, sizeof st);
        h.payload_len += sizeof st;
        break;
    }

    case REQ_BYE:                   /* 응답하지 않는다 (맨 위의 DECISION) */
        if (atomic_exchange(&S.connected[id], false)) atomic_fetch_sub(&S.n_clients, 1);
        return;

    default:
        ok = false;
        break;
    }

    if (!ok) {
        h.type = RESP_ERROR;
        h.payload_len = sizeof ts;
        if (ts.t_lock < t_deq) ts.t_lock = now_ns();    /* 락을 잡기 전에 걸러졌다 */
        atomic_fetch_add(&S.n_err, 1);
    }

    if (atomic_load(&S.forced)) return;     /* 두 번째 신호로 닫는 중이다 */
    ts.t_send = now_ns();
    memcpy(buf, &ts, sizeof ts);
    if (tp_send_resp(&h, buf) < 0) {
        if (errno == EPIPE) {       /* 그 클라이언트가 먼저 끝났다. 이 응답만 버린다 */
            atomic_fetch_add(&S.n_gone, 1);
            return;
        }
        fatal("응답 보내기", errno);
        return;
    }
    atomic_fetch_add(&S.bytes_sent, h.payload_len);
}

/* ---- 스레드 ---- */

static void *recv_main(void *arg)
{
    (void)arg;
    job_t j;
    for (;;) {
        ssize_t r = tp_recv_req(&j.hdr, j.p.raw, REQ_CAP);
        uint64_t t = now_ns();
        if (r < 0) {
            if (errno == ECANCELED) break;              /* tp_server_wake: 끝낸다 */
            if (errno == EINTR) continue;
            if (errno == EMSGSIZE) {                    /* 큰 요청은 전송 계층이 버렸다 */
                atomic_fetch_add(&S.n_drop, 1);
                continue;
            }
            fatal("요청 받기", errno);
            break;
        }
        j.len = (uint32_t)r;
        j.t_recv = t;
        if (S.direct) handle(&j, now_ns(), S.buf[0]);
        else queue_put(&j);
    }
    queue_close();
    return NULL;
}

static void *worker_main(void *arg)
{
    uint8_t *buf = arg;
    job_t j;
    while (queue_get(&j)) {
        uint64_t t_deq = now_ns();
        handle(&j, t_deq, buf);
    }
    return NULL;
}

static void *admin_main(void *arg)
{
    const sigset_t *set = arg;
    int sig;
    while (sigwait(set, &sig) != 0)
        ;
    atomic_store(&S.signo, sig);
    begin_stop();
    fprintf(stderr, "server_%s: 끝내는 중. 받은 요청을 마저 처리한다 (한 번 더 보내면 기다리지 않고 바로 끝낸다)\n",
            tp_name());

    /* 두 번째 신호: 남은 응답을 기다리지 않고 닫는다 (맨 위의 DECISION) */
    while (sigwait(set, &sig) != 0)
        ;
    pthread_mutex_lock(&S.close_mtx);
    if (S.tp_closed) {                 /* main 이 이미 정상 절차로 닫았다 */
        pthread_mutex_unlock(&S.close_mtx);
        return NULL;
    }
    S.tp_closed = true;
    atomic_store(&S.forced, true);
    fprintf(stderr, "server_%s: 두 번째 신호. 남은 응답을 기다리지 않고 끝낸다 (PGM 저장과 체크섬 출력 없음)\n",
            tp_name());
    tp_server_close();
    _exit(1);
}

/* ---- main ---- */

/*
 * 클라이언트가 많으면(pipe 는 응답 FIFO 를 클라이언트마다 하나씩 연다) 열린 파일 수 기본 한도(1024)를 넘는다.
 * 이 프로세스의 soft 한도를 hard 한도 안에서 올린다. 관리자 권한이나 시스템 설정은 바꾸지 않는다.
 */
static void raise_fd_limit(void)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) < 0) return;
    rlim_t want = (rlim_t)MAX_CLIENTS * 2 + 256;
    if (rl.rlim_max != RLIM_INFINITY && want > rl.rlim_max) want = rl.rlim_max;
    if (rl.rlim_cur < want) { rl.rlim_cur = want; setrlimit(RLIMIT_NOFILE, &rl); }
}

static void usage(void)
{
    fprintf(stderr,
            "usage: server_%s [-w 워커수] [-l global|tile|tile-rw|none] [-t 타일] [-K 방수] [-d] [-p PGM경로]\n"
            "       -K N   그림판(방) 수 1~%u (1). 요청 머리의 reserved 가 방 번호다. PGM 은 방 0 만 저장한다\n"
            "       [-O CSV -e 실험 -R 반복 -C 커밋]   끝날 때 서버 자원 사용량을 CSV 에 한 줄 (커밋 기본: BENCH_COMMIT)\n", tp_name(), MAX_ROOMS);
}

static bool parse_uint(const char *s, unsigned long lo, unsigned long hi, unsigned long *out)
{
    char *end;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno || end == s || *end || s[0] == '-' || v < lo || v > hi) return false;
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
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    char pgm_default[64];
    snprintf(pgm_default, sizeof pgm_default, "canvas_%s.pgm", tp_name());
    S.workers = WORKERS_DEFAULT;
    S.lock = LOCK_GLOBAL;
    S.tile = TILE_DEFAULT;
    S.pgm = pgm_default;
    S.run = 1;
    S.rooms = 1;
    S.commit = getenv("BENCH_COMMIT");

    int opt;
    unsigned long v;
    while ((opt = getopt(argc, argv, "w:l:t:K:dp:O:e:R:C:h")) != -1) {
        switch (opt) {
        case 'w':
            if (!parse_uint(optarg, 1, MAX_WORKERS, &v)) {
                fprintf(stderr, "server: -w 는 1~%u 이다\n", MAX_WORKERS);
                return 1;
            }
            S.workers = (unsigned)v;
            break;
        case 'l':
            if (lock_mode_parse(optarg, &S.lock) < 0) {
                fprintf(stderr, "server: -l 은 global, tile, tile-rw, none 중 하나다\n");
                return 1;
            }
            break;
        case 't':
            if (!parse_uint(optarg, TILE_MIN, TILE_MAX, &v) || (v & (v - 1))) {
                fprintf(stderr, "server: -t 는 %u~%u 의 2의 거듭제곱이다\n", TILE_MIN, TILE_MAX);
                return 1;
            }
            S.tile = (uint32_t)v;
            break;
        case 'K':
            if (!parse_uint(optarg, 1, MAX_ROOMS, &v)) {
                fprintf(stderr, "server: -K 는 1~%u 이다\n", MAX_ROOMS);
                return 1;
            }
            S.rooms = (uint32_t)v;
            break;
        case 'd': S.direct = true; break;
        case 'p': S.pgm = optarg; break;
        case 'O': S.csv = optarg; break;
        case 'e': S.exp = optarg; break;
        case 'R':
            if (!parse_uint(optarg, 0, 1000000, &v)) {
                fprintf(stderr, "server: -R 은 0~1000000 이다\n");
                return 1;
            }
            S.run = (long)v;
            break;
        case 'C': S.commit = optarg; break;
        case 'h': usage(); return 0;
        default: usage(); return 1;
        }
    }
    if (optind < argc) {
        usage();
        return 1;
    }

    char eb[128];
    unsigned nbuf = S.direct ? 1 : S.workers;
    raise_fd_limit();
    bool mem_ok = true;
    for (uint32_t r = 0; r < S.rooms; r++) mem_ok = mem_ok && (S.cv[r] = canvas_create(S.lock, S.tile)) != NULL;
    S.buf = calloc(nbuf, sizeof *S.buf);
    mem_ok = mem_ok && S.buf;
    for (unsigned i = 0; mem_ok && i < nbuf; i++) mem_ok = (S.buf[i] = malloc(MAX_PAYLOAD)) != NULL;
    if (!mem_ok) {
        fprintf(stderr, "server: 메모리 부족\n");
        return 1;
    }

    usage_t u0, u1, u;
    usage_now(&u0);
    if (tp_server_open() < 0) {
        if (errno == EADDRINUSE) fprintf(stderr, "server_%s: 서버가 이미 떠 있다\n", tp_name());
        else fprintf(stderr, "server_%s: 채널을 만들지 못함: %s\n", tp_name(), err_str(errno, eb, sizeof eb));
        return 1;
    }

    /*
     * admin 을 먼저 만든다. 그 뒤의 스레드를 만들지 못하면 오류를 적고 종료 절차를 시작해, 만든 스레드만 같은 길로 끝낸다.
     * 수신 스레드가 없으면 큐를 닫아 줄 쪽이 없으므로 직접 닫는다.
     * admin 은 거두지 않는다(detach). 끝까지 두 번째 신호를 기다리다 main 이 돌아오면 프로세스와 함께 끝난다.
     */
    pthread_t admin, rx, *wk = calloc(S.workers, sizeof *wk);
    if (!wk || pthread_create(&admin, NULL, admin_main, &set) != 0) {
        fprintf(stderr, "server_%s: 스레드를 만들지 못함\n", tp_name());
        tp_server_close();
        return 1;
    }
    pthread_detach(admin);
    unsigned nwk = 0;
    int e = 0;
    uint64_t t_start = now_ns();
    if (!S.direct) {
        for (; nwk < S.workers; nwk++)
            if ((e = pthread_create(&wk[nwk], NULL, worker_main, S.buf[nwk])) != 0) break;
    }
    bool have_recv = false;
    if (e == 0) have_recv = (e = pthread_create(&rx, NULL, recv_main, NULL)) == 0;
    if (e != 0) {
        queue_close();
        fatal("pthread_create", e);
    }

    fprintf(stderr, "server_%s: 시작 (워커 %u%s, 락 %s, 타일 %u, 방 %u). Ctrl+C 로 끝낸다\n", tp_name(),
            S.direct ? 0 : S.workers, S.direct ? ", -d 직접 처리" : "", lock_mode_name(S.lock), S.tile, S.rooms);

    if (have_recv) pthread_join(rx, NULL);
    for (unsigned i = 0; i < nwk; i++) pthread_join(wk[i], NULL);
    uint64_t t_stop = now_ns();

    /*
     * 송수신하던 스레드가 모두 끝났다. PGM 저장과 체크섬 출력 → tp_server_close (설계 명세의 종료 순서).
     * close_mtx 를 쥐고 하므로 그사이 두 번째 신호가 와도 admin 은 기다렸다가 아무것도 하지 않는다.
     */
    pthread_mutex_lock(&S.close_mtx);
    S.tp_closed = true;
    usage_now(&u1);
    usage_diff(&u1, &u0, &u);
    long rss = vm_rss_kb(), hwm = vm_hwm_kb();
    int rc = S.err[0] ? 1 : 0;
    uint64_t room_sum[MAX_ROOMS];
    for (uint32_t r = 0; r < S.rooms; r++) room_sum[r] = canvas_checksum(S.cv[r]);
    uint64_t sum = canvas_checksum_rooms(room_sum, S.rooms);    /* 방 하나면 그 캔버스의 체크섬 그대로 */
    if (canvas_save_pgm(S.cv[0], S.pgm) < 0) {
        fprintf(stderr, "server_%s: %s 에 저장하지 못함: %s\n", tp_name(), S.pgm, err_str(errno, eb, sizeof eb));
        rc = 1;
    }
    tp_server_close();
    pthread_mutex_unlock(&S.close_mtx);

    for (uint32_t r = 0; S.rooms > 1 && r < S.rooms; r++) printf("room %u checksum " CANVAS_CHECKSUM_FMT "\n", r, room_sum[r]);
    printf("checksum " CANVAS_CHECKSUM_FMT "\n", sum);
    printf("server_%s: DRAW %" PRIu64 ", SNAPSHOT %" PRIu64 ", 오류 응답 %" PRIu64 ", 보낸 바이트 %" PRIu64
           ", 버린 요청 %" PRIu64 ", 받는 쪽이 끝난 응답 %" PRIu64 "\n",
           tp_name(), atomic_load(&S.n_draw), atomic_load(&S.n_snap), atomic_load(&S.n_err),
           atomic_load(&S.bytes_sent), atomic_load(&S.n_drop), atomic_load(&S.n_gone));
    printf("  CPU 사용자 %.3fs, 커널 %.3fs, 컨텍스트 스위치 자발 %ld, 비자발 %ld, VmRSS %ld kB, VmHWM %ld kB\n",
           u.utime_s, u.stime_s, u.nvcsw, u.nivcsw, rss, hwm);
    fflush(stdout);

    if (S.csv && rc == 0) {
        csv_row_t row;
        csv_row_init(&row);
        row.stage = "app";
        row.ipc = tp_name();
        row.role = "server";
        row.experiment = S.exp;
        row.commit = S.commit;
        row.run = S.run;
        row.clients = (long)atomic_load(&S.seen);
        row.workers = S.direct ? 0 : (long)S.workers;
        row.lock = lock_mode_name(S.lock);
        row.tile = S.tile;
        row.n = (long)(atomic_load(&S.n_draw) + atomic_load(&S.n_snap));
        row.elapsed_s = (double)(t_stop - t_start) / 1e9;
        row.utime_s = u.utime_s;
        row.stime_s = u.stime_s;
        row.nvcsw = u.nvcsw;
        row.nivcsw = u.nivcsw;
        row.vmrss_kb = rss;
        row.vmhwm_kb = hwm;
        if (csv_append(S.csv, &row) < 0) {
            fprintf(stderr, "server_%s: %s 에 쓰지 못함: %s\n", tp_name(), S.csv, err_str(errno, eb, sizeof eb));
            rc = 1;
        }
    }

    if (S.err[0]) fprintf(stderr, "server_%s: %s\n", tp_name(), S.err);
    else fprintf(stderr, "server_%s: %s 로 끝남. 캔버스를 %s 에 저장했다\n", tp_name(),
                 atomic_load(&S.signo) == SIGINT ? "SIGINT" : "SIGTERM", S.pgm);

    for (unsigned i = 0; i < nbuf; i++) free(S.buf[i]);
    free(S.buf);
    free(wk);
    for (uint32_t r = 0; r < S.rooms; r++) canvas_destroy(S.cv[r]);
    return rc;
}
