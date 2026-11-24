/*
 * client.c - 공유 캔버스 클라이언트 (세 IPC 공통)
 *
 * 시드로 요청열(workload.c)을 만들어 서버에 보내고, 요청마다 왕복 지연과 구간별 시간을 잰다.
 * 끝나면 통계를 stdout 에 쓰고 -o 의 CSV 에 한 줄(stage=app, role=client)을 붙인다.
 * 어떤 IPC 인지는 링크된 전송 계층(src/transport/)이 정한다. 이 파일은 transport.h 의 함수만 부른다.
 *
 * 스레드 (설계 명세의 스레드 표, 과제 조건: 프로세스마다 3개 이상)
 *   main    옵션, 연결, HELLO 주고받기, 스레드 만들기와 거두기, tp_client_close
 *   send    요청을 만들어 요청 시각 표에 적고 보낸다. 워밍업 → 측정 → STATS → BYE 순서
 *   recv    응답을 받아 요청 시각 표에서 찾고, 검사하고, 구간별 시간을 기록한다
 *   stats   수신이 끝나면 기록을 모아 통계를 내고 출력하고 CSV 에 쓴다
 *   signal  SIGINT·SIGTERM 을 sigwait 로 받아 새 요청을 멈추게 한다. -G 의 시작 신호 SIGUSR2 도 받는다
 *
 * 요청 시각 표 (설계 명세의 공유 자원 표)
 *   동시 요청 수(-f)만큼의 슬롯 + 세마포어(빈 슬롯 수) + mutex. 송신 스레드는 빈 슬롯을 세마포어로 얻은 뒤
 *   mutex 안에서 슬롯에 req_id 와 기대하는 응답 길이를 적고, mutex 밖에서 t0 를 찍어 보낸다.
 *   수신 스레드는 mutex 안에서 req_id 로 슬롯을 찾아 비우고 세마포어를 돌려준다.
 *   표에 없는 req_id 의 응답(같은 번호로 먼저 접속했던 세션의 것)은 버린다 (transport.h 의 약속).
 *
 * 측정 (docs/measurement/README.md 의 타임스탬프 위치)
 *   t0 = tp_send_req 호출 직전, t5 = tp_recv_resp 가 돌아온 직후, 그 사이의 서버 시각은 응답 앞의 srv_ts_t.
 *   측정 구간 = 워밍업 응답을 모두 받은 뒤 첫 측정 요청을 보내기 직전부터 마지막 측정 응답을 받을 때까지.
 *   측정 구간 안에서는 출력과 malloc 을 하지 않는다. 기록 배열과 수신 버퍼는 시작할 때 잡는다.
 *
 * 끝내기
 *   요청을 다 보냈거나 신호를 받으면, 띄워 둔 요청의 응답을 모두 받은 뒤 STATS 를 보낸다. 수신 스레드는
 *   STATS 응답을 마지막 응답으로 알고 끝난다. 그 뒤 BYE 를 보내고, 모든 스레드를 거둔 다음 tp_client_close 를 부른다.
 *   서버가 끝나면 전송 계층이 EPIPE·ECONNRESET 을 돌려주므로 송수신 스레드가 빠져나온다.
 *   두 번째 신호를 받으면 남은 응답을 기다리지 않고 신호 스레드가 tp_client_close 를 부른 뒤 바로 끝낸다(종료 코드 1).
 *
 * DECISION: 두 번째 신호로 끝낼 때는 막혀 있는 송수신 스레드를 거두지 않은 채 tp_client_close 를 부르고 _exit 한다.
 *   서버가 멈춘 채 살아 있으면 tp_recv_resp 가 돌아오지 않는데, transport.h 에는 그것을 깨우는 함수가 없다.
 *   "송수신 스레드가 빠져나온 뒤에 닫는다"는 약속을 어기지만 프로세스가 곧바로 끝나므로 닫은 자원을 다시 쓰지 않는다.
 *   그래서 이 경로에서는 ThreadSanitizer 가 pipe·mq 버전의 닫기(fd 닫기, 버퍼 해제)와 막힌 송수신 사이의 경쟁을 보고한다.
 *   tp_client_close 가 클라이언트 쪽 자원(pipe 버전의 응답 FIFO)을 지우므로 kill -9 와 달리 아무것도 남지 않는다.
 *   main 의 정상 닫기와 겹치지 않게 close_mtx 로 한 번만 닫는다.
 *
 * DECISION: 워밍업 요청은 측정 요청열과 다른 시드(seed ^ WARM_SEED_MIX)의 같은 생성기로 만들고, DRAW 의 v 를 0 으로
 *   바꿔 보낸다. 0 을 더하는 DRAW 는 캔버스를 바꾸지 않으므로 검증기는 워밍업을 몰라도 같은 체크섬을 내고,
 *   서버는 측정 요청과 같은 경로(큐, 락, 그리기, 복사)를 거친다.
 * DECISION: 신호로 멈춘 실행과 응답 검사에 실패한 실행은 CSV 에 쓰지 않고 종료 코드 1 로 끝낸다. 통계는 stdout 에 쓴다.
 *   다 재지 못한 실행이 원자료에 섞이지 않게 하려는 것이다.
 * DECISION: CSV 의 workers 칸은 끝날 때 보내는 STATS 응답의 n_workers 로 채운다. 서버의 락 방식과 타일 크기는
 *   프로토콜로 알 수 없어서, clients·lock·tile 칸은 -c, -L, -T 로 받은 값을 그대로 적는다(동작은 바꾸지 않는다).
 * DECISION: -G 를 주면 워밍업 응답을 모두 받은 뒤 stderr 에 '준비' 를 쓰고 SIGUSR2 를 기다린다. 측정 스크립트가 모든
 *   클라이언트의 준비를 확인한 뒤 한꺼번에 보내, 1단계처럼 측정 구간이 같은 시각에 시작한다. 그래서 전체 처리량을
 *   (모든 클라이언트의 요청 수) / (가장 긴 elapsed_s) 로 낼 수 있다. 기다리는 동안 SIGINT 를 받으면 재지 않고 끝낸다.
 * DECISION: BYE 는 응답을 기다리지 않는다. 서버가 응답하더라도 받는 쪽이 없으면 전송 계층이 버린다.
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
#include <unistd.h>

#include "transport.h"
#include "util.h"
#include "workload.h"

#define WARM_SEED_MIX   0x5741524D55505F31ull   /* 워밍업 요청열의 시드를 측정 요청열과 다르게 한다 */
#define MAX_INFLIGHT    1024u
#define MAX_REQUESTS    10000000ull
#define MAX_WARM        1000000ull
#define STATS_RESP_LEN  (sizeof(srv_ts_t) + sizeof(stats_resp_t))

typedef struct {
    uint32_t    id, room;
    uint64_t    seed, n, warm;
    bool        gate;       /* -G: 워밍업 뒤 SIGUSR2 를 받아야 측정을 시작한다 */
    uint32_t    draw_pct, rect, snap, inflight;
    const char *out, *exp, *commit, *lock;
    long        run, clients, tile;
} cfg_t;

/* 보낸 요청 하나의 정보. 응답을 검사할 때 쓴다 */
typedef struct {
    bool     measured;              /* 측정 요청인가 (워밍업, STATS 가 아님) */
    uint32_t type;
    uint64_t req_id;
    size_t   resp_len;              /* 기대하는 응답 payload 바이트 */
} req_info_t;

/* 요청 시각 표의 칸 하나. t0 밖의 칸은 mtx 로 지킨다 */
typedef struct {
    bool             used;
    bool             measured;
    uint32_t         type;
    uint64_t         req_id;
    size_t           resp_len;
    _Atomic uint64_t t0;            /* mtx 밖에서 찍으므로 원자적으로 쓴다 */
} slot_t;

static struct {
    cfg_t            cfg;
    uint64_t         base;          /* req_id 시작값. 세션마다 다르다 */
    uint64_t         next_id;       /* 송신 스레드만 쓴다 */

    /* 요청 시각 표 */
    slot_t          *slots;
    sem_t            free_slots;    /* 빈 슬롯 수 */
    pthread_mutex_t  mtx;
    pthread_cond_t   cv;            /* inflight 가 0 이 됨, 실패 */
    unsigned         inflight;

    /*
     * 지연 기록. 측정 중에는 수신 스레드만 쓰고, 송신·수신 스레드가 모두 끝났다고 알린 뒤(send_done, recv_done)
     * 통계 스레드가 읽는다. 그래서 측정 구간 안에서는 기록에 잠금을 걸지 않는다.
     */
    pthread_mutex_t  rec_mtx;
    pthread_cond_t   rec_cv;
    bool             send_done, recv_done;
    uint64_t        *rtt;           /* 측정 응답의 왕복 지연 (ns) */
    size_t           n_rec;
    uint64_t         n_draw, n_snap;
    uint64_t         sum_req, sum_queue, sum_lock, sum_work, sum_resp;
    uint64_t         resp_bytes;
    uint64_t         t_end;         /* 마지막 측정 응답의 t5 */
    uint64_t         n_err, n_stale;
    uint64_t         err_req;       /* 처음 검사에 실패한 요청의 순번 (req_id - base) */
    const char      *err_what;
    stats_resp_t     srv;
    bool             have_srv;
    char            *rbuf;          /* 수신 버퍼 MAX_PAYLOAD */

    /* 송신 스레드가 적는다 (측정 구간 앞뒤) */
    uint64_t         t_start;
    bool             measured_all;  /* 측정 요청을 모두 보내고 응답을 모두 받았다 */
    usage_t          u_start, u_end;
    long             rss_kb, hwm_kb;

    atomic_bool      stop;          /* 신호를 받았다. 새 요청을 보내지 않는다 */
    pthread_mutex_t  gate_mtx;      /* -G: 측정 시작 신호 (SIGUSR2) 를 기다린다 */
    pthread_cond_t   gate_cv;
    bool             go;
    atomic_bool      failed;        /* 전송 계층이 실패했다 */
    atomic_bool      quit_sig;      /* 신호 스레드를 끝낸다 */
    pthread_mutex_t  close_mtx;     /* tp_client_close 를 한 번만 부른다 (main 과 신호 스레드) */
    bool             closed;
    const char      *fail_what;     /* 처음 실패. fail() 이 한 번만 적는다 */
    int              fail_errno;
    int              signo;
    int              exit_code;
} C;

/* ---- 실패와 대기 ---- */

/* 전송 계층의 실패를 적고, 기다리는 송신 스레드를 깨운다. 출력은 끝난 뒤 main 이 한다 */
static void fail(const char *what, int e)
{
    pthread_mutex_lock(&C.mtx);
    if (!atomic_load(&C.failed)) {
        C.fail_what = what;
        C.fail_errno = e;
        atomic_store(&C.failed, true);
    }
    pthread_cond_broadcast(&C.cv);
    pthread_mutex_unlock(&C.mtx);
    sem_post(&C.free_slots);
}

static bool done_sending(void)
{
    return atomic_load(&C.stop) || atomic_load(&C.failed);
}

/* 띄워 둔 요청의 응답이 모두 올 때까지 기다린다. 실패하면 false */
static bool wait_idle(void)
{
    pthread_mutex_lock(&C.mtx);
    while (C.inflight > 0 && !atomic_load(&C.failed)) pthread_cond_wait(&C.cv, &C.mtx);
    pthread_mutex_unlock(&C.mtx);
    return !atomic_load(&C.failed);
}

/* 송신·수신 스레드가 끝났음을 통계 스레드에 알린다 */
static void mark_done(bool *flag)
{
    pthread_mutex_lock(&C.rec_mtx);
    *flag = true;
    pthread_cond_broadcast(&C.rec_cv);
    pthread_mutex_unlock(&C.rec_mtx);
}

/* ---- 송신 ---- */

/*
 * 요청 하나를 슬롯에 적고 보낸다. 성공 0. 신호를 받았거나 실패했으면 보내지 않고 -1.
 * honor_stop 이 false 이면 신호를 받은 뒤에도 보낸다 (마지막 STATS).
 */
static int send_one(uint32_t type, const void *payload, uint32_t len, size_t resp_len, bool measured,
                    bool honor_stop)
{
    slot_t *s = NULL;
    for (;;) {
        while (sem_wait(&C.free_slots) < 0 && errno == EINTR)
            ;
        if (atomic_load(&C.failed) || (honor_stop && atomic_load(&C.stop))) return -1;
        pthread_mutex_lock(&C.mtx);
        for (unsigned k = 0; k < C.cfg.inflight; k++) {
            if (!C.slots[k].used) { s = &C.slots[k]; break; }
        }
        if (s) break;
        /* 신호 스레드나 fail() 이 세마포어를 더 올려 빈 슬롯 없이 깨어났다. 다음 응답을 기다린다 */
        pthread_mutex_unlock(&C.mtx);
    }
    uint64_t id = C.next_id++;
    s->used = true;
    s->measured = measured;
    s->type = type;
    s->req_id = id;
    s->resp_len = resp_len;
    C.inflight++;
    pthread_mutex_unlock(&C.mtx);

    msg_hdr_t h;
    memset(&h, 0, sizeof h);
    h.type = type;
    h.client_id = C.cfg.id;
    h.reserved = C.cfg.room;        /* 그림판(방) 번호 */
    h.req_id = id;
    h.payload_len = len;

    uint64_t t0 = now_ns();
    atomic_store_explicit(&s->t0, t0, memory_order_release);
    if (tp_send_req(&h, payload) < 0) {
        int e = errno;
        pthread_mutex_lock(&C.mtx);
        s->used = false;
        C.inflight--;
        pthread_mutex_unlock(&C.mtx);
        fail("요청 보내기", e);
        return -1;
    }
    return 0;
}

static void *send_main(void *arg)
{
    (void)arg;
    const cfg_t *cfg = &C.cfg;
    workload_t wl, warm;
    wl_req_t r;
    workload_init(&wl, cfg->seed, cfg->draw_pct, cfg->rect, cfg->snap);
    workload_init(&warm, cfg->seed ^ WARM_SEED_MIX, cfg->draw_pct, cfg->rect, cfg->snap);

    /* 워밍업: v = 0 인 DRAW 는 캔버스를 바꾸지 않는다 (맨 위의 DECISION) */
    for (uint64_t i = 0; i < cfg->warm && !done_sending(); i++) {
        workload_next(&warm, &r);
        if (r.type == REQ_DRAW) r.p.draw.v = 0;
        if (send_one(r.type, &r.p, r.len, workload_resp_len(&r), false, true) < 0) break;
    }

    /* 측정 구간: 워밍업 응답을 모두 받은 뒤 시작한다. -G 이면 준비를 알리고 시작 신호(SIGUSR2)를 기다린다 (맨 위의 DECISION) */
    bool idle0 = wait_idle();
    if (idle0 && cfg->gate && !atomic_load(&C.stop)) {
        fprintf(stderr, "client_%s %u: 준비 (시작 신호 SIGUSR2 를 기다린다)\n", tp_name(), cfg->id);
        pthread_mutex_lock(&C.gate_mtx);
        while (!C.go && !atomic_load(&C.stop)) pthread_cond_wait(&C.gate_cv, &C.gate_mtx);
        pthread_mutex_unlock(&C.gate_mtx);
    }
    if (idle0 && !atomic_load(&C.stop)) {
        usage_now(&C.u_start);
        C.t_start = now_ns();
        uint64_t sent = 0;
        for (; sent < cfg->n && !done_sending(); sent++) {
            workload_next(&wl, &r);
            if (send_one(r.type, &r.p, r.len, workload_resp_len(&r), true, true) < 0) break;
        }
        bool idle = wait_idle();
        usage_now(&C.u_end);
        C.rss_kb = vm_rss_kb();
        C.hwm_kb = vm_hwm_kb();
        C.measured_all = idle && sent == cfg->n;
    }

    /*
     * 마지막 요청은 STATS 다. 수신 스레드는 이 응답을 받고 끝난다. 신호를 받았어도 보낸다.
     * 실패했으면 서버가 끝난 것이라 수신 스레드도 ECONNRESET 으로 끝난다.
     */
    if (wait_idle()) send_one(REQ_STATS, NULL, 0, STATS_RESP_LEN, false, false);
    if (wait_idle()) {
        msg_hdr_t h;
        memset(&h, 0, sizeof h);
        h.type = REQ_BYE;
        h.client_id = cfg->id;
        h.reserved = cfg->room;
        h.req_id = C.next_id++;
        if (tp_send_req(&h, NULL) < 0 && errno != EPIPE) fail("BYE 보내기", errno);
    }
    mark_done(&C.send_done);
    return NULL;
}

/* ---- 수신 ---- */

static void bad_resp(uint64_t req_id, const char *what)
{
    if (C.n_err++ == 0) {
        C.err_req = req_id - C.base;
        C.err_what = what;
    }
}

/* 응답 하나를 검사하고, 측정 응답이면 기록한다. 수신 스레드만 부른다 */
static void check_resp(const msg_hdr_t *h, ssize_t r, const req_info_t *s, uint64_t t0, uint64_t t5)
{
    if (h->type != RESP_OK) { bad_resp(s->req_id, "응답이 RESP_OK 가 아니다"); return; }
    if (h->client_id != C.cfg.id) { bad_resp(s->req_id, "응답의 클라이언트 번호가 다르다"); return; }
    if ((size_t)r != s->resp_len) { bad_resp(s->req_id, "응답 payload 길이가 요청과 맞지 않는다"); return; }

    if (s->type == REQ_STATS) {
        memcpy(&C.srv, C.rbuf + sizeof(srv_ts_t), sizeof C.srv);
        C.have_srv = true;
        return;
    }

    /* DRAW, SNAPSHOT: 서버 시각이 수신 ≤ 큐 꺼냄 ≤ 락 획득 ≤ 송신 순서여야 한다 (T-16). 앞뒤의 t0, t5 도 본다 */
    srv_ts_t ts;
    memcpy(&ts, C.rbuf, sizeof ts);
    if (!(t0 <= ts.t_recv && ts.t_recv <= ts.t_deq && ts.t_deq <= ts.t_lock &&
          ts.t_lock <= ts.t_send && ts.t_send <= t5)) {
        bad_resp(s->req_id, "서버 시각 순서가 맞지 않는다");
        return;
    }
    if (!s->measured) return;

    C.rtt[C.n_rec++] = t5 - t0;
    C.sum_req += ts.t_recv - t0;
    C.sum_queue += ts.t_deq - ts.t_recv;
    C.sum_lock += ts.t_lock - ts.t_deq;
    C.sum_work += ts.t_send - ts.t_lock;
    C.sum_resp += t5 - ts.t_send;
    C.resp_bytes += (uint64_t)r;
    if (s->type == REQ_DRAW) C.n_draw++; else C.n_snap++;
    C.t_end = t5;
}

static void *recv_main(void *arg)
{
    (void)arg;
    for (;;) {
        msg_hdr_t h;
        ssize_t r = tp_recv_resp(&h, C.rbuf, MAX_PAYLOAD);
        uint64_t t5 = now_ns();
        if (r < 0) {
            /* cap 이 MAX_PAYLOAD 라 EMSGSIZE 도 정상 응답이 아니다. 어느 요청의 것인지 몰라 기다릴 수 없다 */
            fail("응답 받기", errno);
            break;
        }

        pthread_mutex_lock(&C.mtx);
        slot_t *s = NULL;
        for (unsigned k = 0; k < C.cfg.inflight; k++) {
            if (C.slots[k].used && C.slots[k].req_id == h.req_id) { s = &C.slots[k]; break; }
        }
        if (!s) {
            /* 띄워 두지 않은 req_id: 같은 번호의 예전 세션에 갈 응답이었다. 버린다 */
            pthread_mutex_unlock(&C.mtx);
            C.n_stale++;
            continue;
        }
        req_info_t got = { s->measured, s->type, s->req_id, s->resp_len };
        uint64_t t0 = atomic_load_explicit(&s->t0, memory_order_acquire);
        s->used = false;
        if (--C.inflight == 0) pthread_cond_broadcast(&C.cv);
        pthread_mutex_unlock(&C.mtx);
        sem_post(&C.free_slots);

        check_resp(&h, r, &got, t0, t5);
        if (got.type == REQ_STATS) break;   /* 송신 스레드가 마지막으로 보낸 요청이다 */
    }
    mark_done(&C.recv_done);
    return NULL;
}

/* ---- 통계 ---- */

static double avg_us(uint64_t sum, size_t n)
{
    return n ? (double)sum / (double)n / 1e3 : -1;
}

static void *stats_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&C.rec_mtx);
    while (!(C.send_done && C.recv_done)) pthread_cond_wait(&C.rec_cv, &C.rec_mtx);
    pthread_mutex_unlock(&C.rec_mtx);

    const cfg_t *cfg = &C.cfg;
    size_t n = C.n_rec;
    lat_stats_t st = { 0, 0, 0, 0, 0 };
    if (n) lat_stats(C.rtt, n, &st);
    double elapsed = n && C.t_end > C.t_start ? (double)(C.t_end - C.t_start) / 1e9 : 0;
    double thr = elapsed > 0 ? (double)n / elapsed : 0;
    double mbs = elapsed > 0 ? (double)C.resp_bytes / elapsed / 1e6 : 0;
    usage_t u = { 0, 0, 0, 0 };
    if (n) usage_diff(&C.u_end, &C.u_start, &u);

    printf("client_%s %u: 요청 %zu개 (DRAW %" PRIu64 ", SNAPSHOT %" PRIu64 "), %.3fs, %.0f msg/s, %.2f MB/s\n",
           tp_name(), cfg->id, n, C.n_draw, C.n_snap, elapsed, thr, mbs);
    if (n) {
        printf("  왕복 지연 us: 평균 %.2f, 중앙값 %.2f, 상위 99%% %.2f, 최대 %.2f\n",
               st.mean / 1e3, (double)st.p50 / 1e3, (double)st.p99 / 1e3, (double)st.max / 1e3);
        printf("  구간 평균 us: 요청 전송 %.2f, 큐 대기 %.2f, 락 대기 %.2f, 처리 %.2f, 응답 전송 %.2f\n",
               avg_us(C.sum_req, n), avg_us(C.sum_queue, n), avg_us(C.sum_lock, n),
               avg_us(C.sum_work, n), avg_us(C.sum_resp, n));
        printf("  CPU 사용자 %.3fs, 커널 %.3fs, 컨텍스트 스위치 자발 %ld, 비자발 %ld, VmRSS %ld kB, VmHWM %ld kB\n",
               u.utime_s, u.stime_s, u.nvcsw, u.nivcsw, C.rss_kb, C.hwm_kb);
    }
    if (C.have_srv)
        printf("  서버: DRAW %" PRIu64 ", SNAPSHOT %" PRIu64 ", 보낸 바이트 %" PRIu64 ", 클라이언트 %u, 워커 %u\n",
               C.srv.n_draw, C.srv.n_snapshot, C.srv.bytes_sent, C.srv.n_clients, C.srv.n_workers);
    if (C.n_stale) printf("  버린 응답 (예전 세션의 것) %" PRIu64 "개\n", C.n_stale);
    fflush(stdout);

    bool complete = C.measured_all && !atomic_load(&C.failed) && !atomic_load(&C.stop) && C.n_err == 0;
    if (!complete || !cfg->out) {
        C.exit_code = complete ? 0 : 1;
        return NULL;
    }

    csv_row_t row;
    csv_row_init(&row);
    row.stage = "app";
    row.ipc = tp_name();
    row.role = "client";
    row.experiment = cfg->exp;
    row.commit = cfg->commit;
    row.run = cfg->run;
    row.clients = cfg->clients;
    row.inflight = cfg->inflight;
    row.workers = C.have_srv ? (long)C.srv.n_workers : -1;
    row.lock = cfg->lock;
    row.tile = cfg->tile;
    row.draw_pct = cfg->draw_pct;
    row.rect = cfg->rect;
    row.snap = cfg->snap;
    row.n = (long)n;
    row.elapsed_s = elapsed;
    row.lat_mean_us = st.mean / 1e3;
    row.lat_p50_us = (double)st.p50 / 1e3;
    row.lat_p99_us = (double)st.p99 / 1e3;
    row.lat_max_us = (double)st.max / 1e3;
    row.thr_msg_s = thr;
    row.thr_mb_s = mbs;
    row.req_us = avg_us(C.sum_req, n);
    row.queue_us = avg_us(C.sum_queue, n);
    row.lock_us = avg_us(C.sum_lock, n);
    row.work_us = avg_us(C.sum_work, n);
    row.resp_us = avg_us(C.sum_resp, n);
    row.utime_s = u.utime_s;
    row.stime_s = u.stime_s;
    row.nvcsw = u.nvcsw;
    row.nivcsw = u.nivcsw;
    row.vmrss_kb = C.rss_kb;
    row.vmhwm_kb = C.hwm_kb;
    if (csv_append(cfg->out, &row) < 0) {
        char eb[128];
        fprintf(stderr, "client: %s 에 쓰지 못함: %s\n", cfg->out,
                errno == EBADMSG ? "머리글이 지금의 열과 다르다" : err_str(errno, eb, sizeof eb));
        C.exit_code = 1;
        return NULL;
    }
    C.exit_code = 0;
    return NULL;
}

/* ---- 신호 ---- */

/* tp_client_close 를 한 번만 부른다. main 의 정상 경로와 신호 스레드의 강제 종료가 함께 쓴다 */
static void client_close(void)
{
    pthread_mutex_lock(&C.close_mtx);
    if (!C.closed) {
        tp_client_close();
        C.closed = true;
    }
    pthread_mutex_unlock(&C.close_mtx);
}

/* 두 번째 신호: 남은 응답을 기다리지 않고 끝낸다 (맨 위의 DECISION) */
static void force_quit(int sig)
{
    fprintf(stderr, "client_%s %u: 두 번째 %s 를 받아 남은 응답을 기다리지 않고 끝낸다\n",
            tp_name(), C.cfg.id, sig == SIGTERM ? "SIGTERM" : "SIGINT");
    client_close();
    _exit(1);
}

static void *signal_main(void *arg)
{
    const sigset_t *set = arg;
    for (;;) {
        int sig;
        if (sigwait(set, &sig) != 0) continue;
        if (sig == SIGUSR1) {
            if (atomic_load(&C.quit_sig)) return NULL;
            continue;
        }
        if (sig == SIGUSR2) {           /* -G 의 측정 시작 신호. -G 가 없으면 무시한다 */
            pthread_mutex_lock(&C.gate_mtx);
            C.go = true;
            pthread_cond_broadcast(&C.gate_cv);
            pthread_mutex_unlock(&C.gate_mtx);
            continue;
        }
        /* SIGINT, SIGTERM: 새 요청을 멈춘다. 빈 슬롯을 기다리는 송신 스레드를 깨운다. 두 번째면 바로 끝낸다 */
        if (atomic_exchange(&C.stop, true)) force_quit(sig);
        C.signo = sig;
        sem_post(&C.free_slots);
        pthread_mutex_lock(&C.gate_mtx);   /* 시작 신호를 기다리던 송신 스레드도 깨운다 */
        pthread_cond_broadcast(&C.gate_cv);
        pthread_mutex_unlock(&C.gate_mtx);
    }
}

/* ---- main ---- */

static void usage(const char *prog)
{
    fprintf(stderr,
            "사용법: %s [-i 번호] [-s 시드] [-n 요청수] [-w 워밍업] [-m DRAW%%] [-r 변] [-S 변] [-f 동시요청]\n"
            "          [-o 결과.csv] [-e 실험] [-R 반복] [-C 커밋] [-c 클라이언트수] [-L 락] [-T 타일] [-G] [-k 방]\n"
            "  -i N   클라이언트 번호 0~%u (기본 0)\n"
            "  -k N   그릴 그림판(방) 번호. 서버 -K 의 방 수보다 작아야 한다 (기본 0)\n"
            "  -s N   시드 (기본 번호 + 1)\n"
            "  -n N   잴 요청 수 (기본 20000)\n"
            "  -w N   워밍업 요청 수. 캔버스를 바꾸지 않는다 (기본 500)\n"
            "  -m N   DRAW 비율 %%, 나머지는 SNAPSHOT (기본 %u)\n"
            "  -r N   DRAW 사각형 한 변 1~%u (기본 %u)\n"
            "  -S N   SNAPSHOT 영역 한 변 1~%u (기본 %u)\n"
            "  -f N   동시에 띄워 둘 요청 수 1~%u. 1 이면 지연, 크면 처리량 (기본 1)\n"
            "  -o F   결과 CSV 에 한 줄을 덧붙인다\n"
            "  -e S   CSV 의 실험 이름 (기본 latency 또는 throughput)\n"
            "  -R N   CSV 의 반복 번호 (기본 1)\n"
            "  -C S   CSV 의 커밋 (기본: 환경 변수 BENCH_COMMIT)\n"
            "  -c N, -L S, -T N   CSV 의 clients, lock, tile 칸. 서버 설정을 적는다 (동작은 바꾸지 않는다)\n"
            "  -G     워밍업이 끝나면 stderr 에 '준비' 를 쓰고 SIGUSR2 를 받을 때까지 측정을 미룬다.\n"
            "         여러 클라이언트의 측정 시작을 맞출 때 쓴다 (scripts/app-bench.sh)\n",
            prog, MAX_CLIENTS - 1, WL_DRAW_PCT_DEFAULT, CANVAS_W, WL_RECT_DEFAULT, CANVAS_W, WL_SNAP_DEFAULT,
            MAX_INFLIGHT);
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

static const char *open_err(int e, char *buf, size_t len)
{
    switch (e) {
    case ECONNREFUSED: return "서버가 없다";
    case EADDRINUSE:   return "같은 번호의 클라이언트가 실행 중이다";
    default:           return err_str(e, buf, len);
    }
}

/*
 * 첫 요청 HELLO 를 보내고 응답을 기다린다 (transport.h 의 약속). 송수신 스레드를 만들기 전이라 main 만 송수신한다.
 * 같은 번호의 예전 세션에 갈 응답은 버린다. 성공 0
 */
static int hello(void)
{
    msg_hdr_t h;
    memset(&h, 0, sizeof h);
    h.type = REQ_HELLO;
    h.client_id = C.cfg.id;
    h.reserved = C.cfg.room;
    h.req_id = C.next_id++;
    uint64_t want = h.req_id;
    if (tp_send_req(&h, NULL) < 0) { fail("HELLO 보내기", errno); return -1; }
    for (;;) {
        ssize_t r = tp_recv_resp(&h, C.rbuf, MAX_PAYLOAD);
        if (r < 0) { fail("HELLO 응답 받기", errno); return -1; }
        if (h.req_id != want) { C.n_stale++; continue; }
        if (h.type != RESP_OK) { fail("서버가 HELLO 를 거부했다 (없는 방 번호일 수 있다)", 0); return -1; }
        return 0;
    }
}

int main(int argc, char **argv)
{
    /*
     * 신호는 가장 먼저 막는다. 이후에 만드는 스레드(전송 계층 안의 감시 스레드 포함)가 모두 막힌 채 시작하고,
     * 신호 스레드만 sigwait 로 받는다.
     */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    cfg_t *cfg = &C.cfg;
    *cfg = (cfg_t){
        .id = 0, .seed = 0, .n = 20000, .warm = 500,
        .draw_pct = WL_DRAW_PCT_DEFAULT, .rect = WL_RECT_DEFAULT, .snap = WL_SNAP_DEFAULT, .inflight = 1,
        .out = NULL, .exp = NULL, .commit = getenv("BENCH_COMMIT"), .lock = NULL,
        .run = 1, .clients = -1, .tile = -1,
    };
    bool have_seed = false;
    uint64_t v;
    int opt;
    while ((opt = getopt(argc, argv, "i:k:s:n:w:m:r:S:f:o:e:R:C:c:L:T:Gh")) != -1) {
        switch (opt) {
        case 'i': if (parse_u64(optarg, &v) < 0 || v >= MAX_CLIENTS) goto bad; cfg->id = (uint32_t)v; break;
        case 'k': if (parse_u64(optarg, &v) < 0 || v >= MAX_ROOMS) goto bad; cfg->room = (uint32_t)v; break;
        case 's': if (parse_u64(optarg, &v) < 0) goto bad; cfg->seed = v; have_seed = true; break;
        case 'n': if (parse_u64(optarg, &v) < 0 || v < 1 || v > MAX_REQUESTS) goto bad; cfg->n = v; break;
        case 'w': if (parse_u64(optarg, &v) < 0 || v > MAX_WARM) goto bad; cfg->warm = v; break;
        case 'm': if (parse_u64(optarg, &v) < 0 || v > 100) goto bad; cfg->draw_pct = (uint32_t)v; break;
        case 'r': if (parse_u64(optarg, &v) < 0 || v < 1 || v > CANVAS_W) goto bad; cfg->rect = (uint32_t)v; break;
        case 'S': if (parse_u64(optarg, &v) < 0 || v < 1 || v > CANVAS_W) goto bad; cfg->snap = (uint32_t)v; break;
        case 'f': if (parse_u64(optarg, &v) < 0 || v < 1 || v > MAX_INFLIGHT) goto bad; cfg->inflight = (uint32_t)v; break;
        case 'o': cfg->out = optarg; break;
        case 'e': cfg->exp = optarg; break;
        case 'R': if (parse_u64(optarg, &v) < 0 || v > 1000000) goto bad; cfg->run = (long)v; break;
        case 'C': cfg->commit = optarg; break;
        case 'c': if (parse_u64(optarg, &v) < 0 || v < 1 || v > MAX_CLIENTS) goto bad; cfg->clients = (long)v; break;
        case 'L': cfg->lock = optarg; break;
        case 'T': if (parse_u64(optarg, &v) < 0 || v < 1 || v > CANVAS_W) goto bad; cfg->tile = (long)v; break;
        case 'G': cfg->gate = true; break;
        case 'h': usage(argv[0]); return 0;
        default: goto bad;
        }
    }
    if (optind != argc) goto bad;
    if (!have_seed) cfg->seed = cfg->id + 1;
    if (!cfg->exp) cfg->exp = cfg->inflight == 1 ? "latency" : "throughput";
    workload_t probe;
    if (workload_init(&probe, cfg->seed, cfg->draw_pct, cfg->rect, cfg->snap) < 0) goto bad;

    /* 기록 배열과 수신 버퍼는 연결 전에 모두 잡는다. 측정 구간에서는 malloc 을 부르지 않는다 */
    C.slots = calloc(cfg->inflight, sizeof *C.slots);
    C.rtt = malloc(cfg->n * sizeof *C.rtt);
    C.rbuf = malloc(MAX_PAYLOAD);
    if (!C.slots || !C.rtt || !C.rbuf) {
        fprintf(stderr, "client: 메모리가 부족하다\n");
        return 1;
    }
    memset(C.rtt, 0, cfg->n * sizeof *C.rtt);  /* 측정 중에 페이지를 처음 건드리지 않게 미리 쓴다 */
    memset(C.rbuf, 0, MAX_PAYLOAD);
    for (unsigned k = 0; k < cfg->inflight; k++) atomic_init(&C.slots[k].t0, 0);
    sem_init(&C.free_slots, 0, cfg->inflight);
    pthread_mutex_init(&C.mtx, NULL);
    pthread_cond_init(&C.cv, NULL);
    pthread_mutex_init(&C.rec_mtx, NULL);
    pthread_cond_init(&C.rec_cv, NULL);
    pthread_mutex_init(&C.close_mtx, NULL);
    pthread_mutex_init(&C.gate_mtx, NULL);
    pthread_cond_init(&C.gate_cv, NULL);
    atomic_init(&C.stop, false);
    atomic_init(&C.failed, false);
    atomic_init(&C.quit_sig, false);
    C.base = (uint64_t)getpid() << 32;     /* 세션마다 다른 시작값 (transport.h 의 약속) */
    C.next_id = C.base;
    C.exit_code = 1;

    char eb[128];
    if (tp_client_open(cfg->id) < 0) {
        int e = errno;
        fprintf(stderr, "client_%s: 연결하지 못함 (번호 %u): %s\n", tp_name(), cfg->id, open_err(e, eb, sizeof eb));
        return 1;
    }

    /* 여기서부터는 어느 경로로 끝나든 tp_client_close 를 부른다 */
    pthread_t sig_th, stats_th, recv_th, send_th;
    bool sig_ok = pthread_create(&sig_th, NULL, signal_main, &set) == 0;
    bool stats_ok = false, recv_ok = false, send_ok = false;

    if (hello() == 0 && !atomic_load(&C.stop)) {
        stats_ok = pthread_create(&stats_th, NULL, stats_main, NULL) == 0;
        recv_ok = pthread_create(&recv_th, NULL, recv_main, NULL) == 0;
        if (recv_ok) {
            /* 송신 스레드를 만들지 못하면 main 이 대신 보낸다. 그래야 수신 스레드가 기다리는 STATS 응답이 온다 */
            send_ok = pthread_create(&send_th, NULL, send_main, NULL) == 0;
            if (!send_ok) send_main(NULL);
        } else {
            fail("스레드 만들기", EAGAIN);
        }
    }
    if (send_ok) pthread_join(send_th, NULL);
    if (recv_ok) pthread_join(recv_th, NULL);
    if (!recv_ok) {
        /* 송수신 스레드가 없었으면 통계 스레드를 대신 풀어 준다 */
        mark_done(&C.send_done);
        mark_done(&C.recv_done);
    }
    if (stats_ok) pthread_join(stats_th, NULL);
    else if (recv_ok) stats_main(NULL);

    client_close();

    if (sig_ok) {
        atomic_store(&C.quit_sig, true);
        pthread_kill(sig_th, SIGUSR1);
        pthread_join(sig_th, NULL);
    }

    if (atomic_load(&C.failed)) {
        int e = C.fail_errno;
        const char *why = e == ECONNRESET || e == EPIPE ? "서버가 끝났다" : e ? err_str(e, eb, sizeof eb) : NULL;
        fprintf(stderr, "client_%s %u: %s%s%s\n", tp_name(), cfg->id, C.fail_what, why ? ": " : "", why ? why : "");
        C.exit_code = 1;
    } else if (atomic_load(&C.stop)) {
        fprintf(stderr, "client_%s %u: %s 를 받아 요청 %zu개만 재고 끝냈다 (CSV 에 쓰지 않음)\n",
                tp_name(), cfg->id, C.signo == SIGTERM ? "SIGTERM" : "SIGINT", C.n_rec);
        C.exit_code = 1;
    } else if (C.n_err) {
        fprintf(stderr, "client_%s %u: 응답 검사 실패 %" PRIu64 "개 (처음: 요청 %" PRIu64 ", %s)\n",
                tp_name(), cfg->id, C.n_err, C.err_req, C.err_what);
        C.exit_code = 1;
    } else if (!sig_ok || !stats_ok || !send_ok) {
        fprintf(stderr, "client_%s %u: 스레드를 만들지 못했다\n", tp_name(), cfg->id);
        C.exit_code = 1;
    }
    return C.exit_code;

bad:
    fprintf(stderr, "client: 잘못된 옵션이다 (도움말: %s -h)\n", argv[0]);
    return 1;
}
