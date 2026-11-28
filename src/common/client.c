/*
 * client.c - 부하 클라이언트 (세 IPC 공통. 링크하는 전송 계층만 다르다)
 *
 * 시드로 정해진 요청열(workload.c)을 보내고 왕복 지연, 구간별 시간, 처리량, 자원 사용량을 잰다.
 * 스레드 (docs/design/README.md#스레드-설계)
 *   main       옵션, 전송 계층 열기·닫기, 스레드 만들고 거두기, 결과 출력·표본 파일·CSV
 *   송신       HELLO → 워밍업 → (실행기의 시작 신호) → 측정 요청 → STATS → BYE
 *   수신       응답을 받아 요청 표에서 짝을 찾고, 검사하고, 표본을 남기고, 칸을 돌려준다
 *   신호       sigwait: SIGUSR2(측정 시작), SIGINT·SIGTERM(멈춤, 두 번째는 강제)
 *
 * 요청 표: -f 칸. 빈 칸 수는 세마포어, 칸의 주인은 원자적 상태로 넘긴다(송신이 채우고 release, 수신이 acquire 로 읽음).
 * 요청 번호 = (pid << 40) | (일련번호 << 10) | 칸 번호. 아래 10비트로 응답의 칸을 바로 찾는다.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "proto.h"
#include "transport.h"
#include "util.h"
#include "workload.h"
#include "latdump.h"

#define MAX_INFLIGHT 1024u
#define SLOT_BITS 10

/* ---- 설정 ---- */

static uint32_t opt_id, opt_room = UINT32_MAX, opt_f = 1, opt_warm = 200;
static uint64_t opt_seed, opt_n = 10000;
static double opt_dur, opt_hz;
static bool opt_aligned;             /* -A: 페이스 모드의 위상을 흩지 않는다(처음 목표 부하 실험과 같게) */
static wl_cfg_t W = { 0, WL_DRAW_PCT, WL_RECT, WL_SNAP };
static bool opt_gate, opt_quiet;
static const char *opt_csv, *opt_exp = "latency";
static long opt_run = 1;

/* ---- 요청 표 ---- */

typedef struct {
    atomic_int state;           /* 0 = 빈 칸, 1 = 응답을 기다림 */
    uint64_t   req_id;
    uint64_t   t0;              /* tp_send_req 직전 */
    uint32_t   type;
    uint32_t   want_len;        /* 응답 payload 길이 (srv_ts_t 포함) */
    bool       measured;
} slot_t;

static slot_t   *slots;
static uint32_t *free_stack, free_top;
static pthread_mutex_t free_mtx = PTHREAD_MUTEX_INITIALIZER;
static sem_t     free_sem;
static uint64_t  seq;           /* 송신 스레드만 쓴다 */
static uint64_t  id_base;

/* ---- 측정 상태 ---- */

static pthread_mutex_t st_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  st_cond = PTHREAD_COND_INITIALIZER;     /* 시작, 멈춤, 응답이 다 옴, 수신 끝남 */
static bool started, stop_req, recv_dead;
static uint32_t inflight;       /* 응답을 기다리는 요청 수 (st_mtx) */

/* 수신 스레드만 쓰고, main 이 두 스레드를 거둔 뒤에 읽는다 */
static uint32_t *lat_draw, *lat_snap;
static uint64_t cap_samples, n_draw, n_snap, n_drop, n_done, bytes_recv, seg_ns[5], t_last;
static stats_resp_t last_stats;
static bool have_stats;
static atomic_ullong errors, ts_errors, strays;

/* 송신 스레드만 쓰고, main 이 거둔 뒤에 읽는다 */
static uint64_t t_start, n_sent;
static usage_t u_start, u_end;
static bool measured_all;

static pthread_t sig_th;

/* ---- 송신 ---- */

static void wait_drain(void)
{
    pthread_mutex_lock(&st_mtx);
    while (inflight > 0 && !recv_dead) pthread_cond_wait(&st_cond, &st_mtx);
    pthread_mutex_unlock(&st_mtx);
}

/* 빈 칸 하나를 얻어 요청을 보낸다. 수신이 끝났거나 서버가 끝났으면 -1 */
static int send_req(uint32_t type, const void *payload, uint32_t plen, uint32_t want_len, bool measured)
{
    while (sem_wait(&free_sem) < 0) {
        if (errno != EINTR) return -1;
    }
    pthread_mutex_lock(&st_mtx);
    bool dead = recv_dead;
    if (!dead) inflight++;
    pthread_mutex_unlock(&st_mtx);
    if (dead) { sem_post(&free_sem); return -1; }

    pthread_mutex_lock(&free_mtx);
    uint32_t k = free_stack[--free_top];
    pthread_mutex_unlock(&free_mtx);

    slot_t *s = &slots[k];
    msg_hdr_t h = {
        .type = type, .client_id = opt_id, .req_id = id_base | (++seq << SLOT_BITS) | k,
        .payload_len = plen, .room = opt_room,
    };
    s->req_id = h.req_id;
    s->type = type;
    s->want_len = want_len;
    s->measured = measured;
    s->t0 = now_ns();
    atomic_store_explicit(&s->state, 1, memory_order_release);
    if (tp_send_req(&h, payload) < 0) {
        atomic_store(&s->state, 0);
        pthread_mutex_lock(&free_mtx);
        free_stack[free_top++] = k;
        pthread_mutex_unlock(&free_mtx);
        pthread_mutex_lock(&st_mtx);
        inflight--;
        pthread_cond_broadcast(&st_cond);
        pthread_mutex_unlock(&st_mtx);
        sem_post(&free_sem);
        return -1;
    }
    return 0;
}

static int send_wl(const wl_req_t *r, bool measured)
{
    if (r->type == REQ_DRAW) {
        draw_req_t d = { .x = r->x, .y = r->y, .w = r->w, .h = r->h, .v = measured ? r->v : 0 };
        return send_req(REQ_DRAW, &d, sizeof d, sizeof(srv_ts_t), measured);
    }
    snap_req_t s = { .x = r->x, .y = r->y, .w = r->w, .h = r->h };
    return send_req(REQ_SNAPSHOT, &s, sizeof s, (uint32_t)(sizeof(srv_ts_t) + (uint32_t)r->w * r->h), measured);
}

static bool should_stop(void)
{
    pthread_mutex_lock(&st_mtx);
    bool v = stop_req || recv_dead;
    pthread_mutex_unlock(&st_mtx);
    return v;
}

static void *send_main(void *arg)
{
    (void)arg;
    if (send_req(REQ_HELLO, NULL, 0, sizeof(srv_ts_t), false) < 0) goto bye;
    wait_drain();

    wl_req_t r;
    for (uint32_t i = 0; i < opt_warm && !should_stop(); i++) {
        wl_gen(&W, WL_WARM_BASE + i, &r);
        if (send_wl(&r, false) < 0) goto bye;
    }
    wait_drain();

    /* 실행기가 모든 클라이언트의 워밍업을 기다렸다가 SIGUSR2 로 함께 시작시킨다 */
    if (opt_gate) {
        const char *fd_s = getenv("OSP_READY_FD");
        if (fd_s) {
            int fd = atoi(fd_s);
            ssize_t wr = write(fd, "r", 1);
            (void)wr;
            close(fd);
        } else if (!opt_quiet) {
            fprintf(stderr, "준비\n");
        }
        pthread_mutex_lock(&st_mtx);
        while (!started && !stop_req && !recv_dead) pthread_cond_wait(&st_cond, &st_mtx);
        pthread_mutex_unlock(&st_mtx);
    }
    if (should_stop()) goto bye;

    usage_now(&u_start);
    t_start = now_ns();
    uint64_t deadline = opt_dur > 0 ? t_start + (uint64_t)(opt_dur * 1e9) : 0;
    uint64_t period = opt_hz > 0 ? (uint64_t)(1e9 / opt_hz) : 0;
    /*
     * 페이스 모드(-H)의 시작 위상. 클라이언트들은 시작 신호를 거의 같은 순간에 받으므로 위상이 같으면 요청이 주기마다
     * 한꺼번에 몰린다(사람 1024명이 같은 순간에 마우스를 움직이는 꼴). 번호에 황금비를 곱한 소수 부분으로 주기 안에 고르게 흩는다.
     * DECISION: 난수가 아니라 번호로 정해 실행마다 같은 도착 모양이 되게 한다. 처음(위상 0, 지금의 -A)에는 시작 신호를 받는
     *   순서에 따라 클라이언트 1024개가 약 5ms 안에 몰렸고, 그 퍼짐이 IPC마다 달라(shm 개선 모드만 약 19ms) 목표 부하의
     *   지연 비교가 IPC 차이가 아니라 도착 모양 차이를 쟀다 (docs/measurement/stage2.md).
     */
    double frac = (double)opt_id * 0.6180339887498949;
    frac -= (double)(uint64_t)frac;
    uint64_t phase = period && !opt_aligned ? (uint64_t)(frac * (double)period) : 0;
    uint64_t i = 0;
    /* MEASURE_BEGIN 측정 구간: 출력과 malloc 을 하지 않는다 (요구사항 N-7) */
    for (;; i++) {
        if (deadline ? now_ns() >= deadline : i >= opt_n) break;
        if (should_stop()) break;
        if (period) {
            uint64_t at = t_start + phase + i * period;
            struct timespec ts = { (time_t)(at / 1000000000ull), (long)(at % 1000000000ull) };
            while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
                ;
        }
        wl_gen(&W, i, &r);
        if (send_wl(&r, true) < 0) break;
    }
    n_sent = i;
    wait_drain();
    /* MEASURE_END */
    usage_now(&u_end);
    measured_all = !should_stop() && (deadline || n_sent == opt_n);

    /* 서버 통계를 받아 두고 끝낸다 */
    if (send_req(REQ_STATS, NULL, 0, sizeof(srv_ts_t) + sizeof(stats_resp_t), false) == 0) wait_drain();
bye:
    if (send_req(REQ_BYE, NULL, 0, sizeof(srv_ts_t), false) == 0) wait_drain();
    else {
        /* 수신 스레드가 BYE 응답을 받지 못하므로 직접 깨운다 */
        pthread_mutex_lock(&st_mtx);
        recv_dead = true;
        pthread_cond_broadcast(&st_cond);
        pthread_mutex_unlock(&st_mtx);
    }
    return NULL;
}

/* ---- 수신 ---- */

static void *recv_main(void *arg)
{
    (void)arg;
    size_t cap = APP_MAX_RESP;
    uint8_t *buf = malloc(cap);
    msg_hdr_t h;
    for (;;) {
        /* MEASURE_BEGIN 응답 하나의 처리: 출력과 malloc 을 하지 않는다 */
        ssize_t n = tp_recv_resp(&h, buf, cap);
        uint64_t t5 = now_ns();
        if (n < 0) {
            if (errno == EMSGSIZE) { atomic_fetch_add(&errors, 1); continue; }
            break;                                       /* ECONNRESET: 서버가 끝났다 */
        }
        uint32_t k = (uint32_t)(h.req_id & ((1u << SLOT_BITS) - 1));
        if (k >= opt_f || atomic_load_explicit(&slots[k].state, memory_order_acquire) != 1 ||
            slots[k].req_id != h.req_id) {
            atomic_fetch_add(&strays, 1);                /* 예전 세션의 응답 (transport.h 의 약속) */
            continue;
        }
        slot_t *s = &slots[k];
        bool bad = h.type != RESP_OK || (uint32_t)n != s->want_len || (size_t)n < sizeof(srv_ts_t);
        if (!bad) {
            const srv_ts_t *ts = (const srv_ts_t *)buf;
            if (!(s->t0 <= ts->t_recv && ts->t_recv <= ts->t_deq && ts->t_deq <= ts->t_lock &&
                  ts->t_lock <= ts->t_send && ts->t_send <= t5)) {
                atomic_fetch_add(&ts_errors, 1);
                bad = true;
            } else if (s->measured) {
                uint64_t lat = (t5 - s->t0 + LAT_UNIT_NS / 2) / LAT_UNIT_NS;
                uint32_t v = lat > UINT32_MAX ? UINT32_MAX : (uint32_t)lat;
                if (s->type == REQ_DRAW) {
                    if (n_draw < cap_samples) lat_draw[n_draw++] = v; else n_drop++;
                } else {
                    if (n_snap < cap_samples) lat_snap[n_snap++] = v; else n_drop++;
                }
                seg_ns[0] += ts->t_recv - s->t0;
                seg_ns[1] += ts->t_deq - ts->t_recv;
                seg_ns[2] += ts->t_lock - ts->t_deq;
                seg_ns[3] += ts->t_send - ts->t_lock;
                seg_ns[4] += t5 - ts->t_send;
                bytes_recv += (uint64_t)n;
                n_done++;
                if (t5 > t_last) t_last = t5;
            }
            if (s->type == REQ_STATS) { memcpy(&last_stats, buf + sizeof(srv_ts_t), sizeof last_stats); have_stats = true; }
        }
        if (bad) atomic_fetch_add(&errors, 1);
        bool bye = s->type == REQ_BYE;

        atomic_store_explicit(&s->state, 0, memory_order_release);
        pthread_mutex_lock(&free_mtx);
        free_stack[free_top++] = k;
        pthread_mutex_unlock(&free_mtx);
        pthread_mutex_lock(&st_mtx);
        if (--inflight == 0) pthread_cond_broadcast(&st_cond);
        pthread_mutex_unlock(&st_mtx);
        sem_post(&free_sem);
        /* MEASURE_END */
        if (bye) break;
    }
    free(buf);
    /* 송신 스레드가 기다리지 않게 깨운다 (서버가 끝났을 때) */
    pthread_mutex_lock(&st_mtx);
    recv_dead = true;
    pthread_cond_broadcast(&st_cond);
    pthread_mutex_unlock(&st_mtx);
    for (uint32_t i = 0; i < opt_f; i++) sem_post(&free_sem);
    return NULL;
}

/* ---- 신호 ---- */

static void *sig_main(void *arg)
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
        if (sigwait(&set, &sig) != 0) continue;
        if (sig == SIGUSR1) break;                       /* main: 끝 */
        pthread_mutex_lock(&st_mtx);
        if (sig == SIGUSR2) started = true;
        else nstop++, stop_req = true;
        pthread_cond_broadcast(&st_cond);
        pthread_mutex_unlock(&st_mtx);
        if (nstop >= 2) {
            /* 서버가 멈춰 응답이 오지 않는다. 기다리지 않고 닫는다 */
            tp_client_close();
            _exit(1);
        }
    }
    return NULL;
}

/* ---- main ---- */

static void usage(const char *prog)
{
    fprintf(stderr,
            "사용법: %s [-i 번호] [-k 그림판] [-s 시드] [-n 요청 | -d 초] [-w 워밍업] [-m DRAW%%] [-r 변] [-S 변]\n"
            "          [-f 동시요청] [-H 초당요청 [-A]] [-G] [-o CSV -e 실험 -R 반복] [-q]\n"
            "  -i 0~%u (0)  -k 그림판 (번호 mod 8)  -s (번호+1)  -n (10000)  -w (200)  -m (90)  -r (16)  -S 1~%u (256)\n"
            "  -f 1~%u (1)  -H 0 = 쉬지 않고  -A = -H 의 위상을 번호로 흩지 않고 모두 0으로\n",
            prog, MAX_CLIENTS - 1, SNAP_MAX_SIDE, MAX_INFLIGHT);
}

static int parse_u64(const char *s, uint64_t lo, uint64_t hi, uint64_t *out)
{
    char *end;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno || *end || !*s || v < lo || v > hi) return -1;
    *out = v;
    return 0;
}

static void write_dump(void)
{
    const char *dir = getenv("OSP_LAT_DIR");
    if (!dir) return;
    char path[512];
    snprintf(path, sizeof path, "%s/%u.lat", dir, opt_id);
    FILE *f = fopen(path, "wb");
    if (!f) { perror("client: 표본 파일"); return; }
    usage_t u;
    usage_diff(&u_end, &u_start, &u);
    lat_hdr_t hd = {
        .magic = LAT_MAGIC, .id = opt_id, .room = opt_room, .inflight = opt_f, .seed = W.seed,
        .draw_pct = W.draw_pct, .rect = W.rect, .snap = W.snap,
        .ok = measured_all && atomic_load(&errors) == 0,
        .n_sent = n_sent, .n_done = n_done, .errors = atomic_load(&errors), .t_start = t_start, .t_last = t_last,
        .bytes = bytes_recv, .n_draw = n_draw, .n_snap = n_snap, .n_drop = n_drop,
        .utime_s = u.utime_s, .stime_s = u.stime_s, .nvcsw = u.nvcsw, .nivcsw = u.nivcsw,
        .vmrss_kb = vm_rss_kb(), .vmhwm_kb = vm_hwm_kb(),
    };
    memcpy(hd.seg_ns, seg_ns, sizeof seg_ns);
    if (fwrite(&hd, sizeof hd, 1, f) != 1 || fwrite(lat_draw, sizeof *lat_draw, n_draw, f) != n_draw ||
        fwrite(lat_snap, sizeof *lat_snap, n_snap, f) != n_snap)
        perror("client: 표본 쓰기");
    fclose(f);
}

static double us_of(uint64_t v) { return (double)v * LAT_UNIT_NS / 1000.0; }

int main(int argc, char **argv)
{
    int c;
    uint64_t v;
    bool seed_set = false;
    while ((c = getopt(argc, argv, "i:k:s:n:d:w:m:r:S:f:H:AGo:e:R:qh")) != -1) {
        switch (c) {
        case 'i': if (parse_u64(optarg, 0, MAX_CLIENTS - 1, &v) < 0) goto bad; opt_id = (uint32_t)v; break;
        case 'k': if (parse_u64(optarg, 0, MAX_ROOMS - 1, &v) < 0) goto bad; opt_room = (uint32_t)v; break;
        case 's': if (parse_u64(optarg, 0, UINT64_MAX, &opt_seed) < 0) goto bad; seed_set = true; break;
        case 'n': if (parse_u64(optarg, 0, 1ull << 30, &opt_n) < 0) goto bad; break;
        case 'd': opt_dur = atof(optarg); if (opt_dur <= 0 || opt_dur > 3600) goto bad; break;
        case 'w': if (parse_u64(optarg, 0, 1u << 24, &v) < 0) goto bad; opt_warm = (uint32_t)v; break;
        case 'm': if (parse_u64(optarg, 0, 100, &v) < 0) goto bad; W.draw_pct = (uint32_t)v; break;
        case 'r': if (parse_u64(optarg, 1, CANVAS_W, &v) < 0) goto bad; W.rect = (uint32_t)v; break;
        case 'S': if (parse_u64(optarg, 1, SNAP_MAX_SIDE, &v) < 0) goto bad; W.snap = (uint32_t)v; break;
        case 'f': if (parse_u64(optarg, 1, MAX_INFLIGHT, &v) < 0) goto bad; opt_f = (uint32_t)v; break;
        case 'H': opt_hz = atof(optarg); if (opt_hz < 0) goto bad; break;
        case 'A': opt_aligned = true; break;
        case 'G': opt_gate = true; break;
        case 'o': opt_csv = optarg; break;
        case 'e': opt_exp = optarg; break;
        case 'R': opt_run = strtol(optarg, NULL, 10); break;
        case 'q': opt_quiet = true; break;
        case 'h': usage(argv[0]); return 0;
        default: goto bad;
        }
    }
    if (optind != argc) goto bad;
    if (opt_room == UINT32_MAX) opt_room = opt_id % 8;
    W.seed = seed_set ? opt_seed : opt_id + 1ull;
    id_base = (uint64_t)getpid() << 40;

    /* 표본 배열: -n 이면 그 수, -d 이면 초당 50만 개까지. 쓴 만큼만 실제 메모리가 잡힌다 */
    cap_samples = opt_dur > 0 ? (uint64_t)(opt_dur * 500000) + 1024 : opt_n + 1;
    slots = calloc(opt_f, sizeof *slots);
    free_stack = calloc(opt_f, sizeof *free_stack);
    lat_draw = malloc(cap_samples * sizeof *lat_draw);
    lat_snap = malloc(cap_samples * sizeof *lat_snap);
    if (!slots || !free_stack || !lat_draw || !lat_snap) { fprintf(stderr, "client: 메모리 부족\n"); return 1; }
    for (uint32_t i = 0; i < opt_f; i++) free_stack[i] = opt_f - 1 - i;
    free_top = opt_f;
    sem_init(&free_sem, 0, opt_f);

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    if (tp_client_open(opt_id) < 0) {
        char eb[128];
        if (errno == ECONNREFUSED) fprintf(stderr, "client_%s %u: 서버가 없다\n", tp_name(), opt_id);
        else if (errno == EADDRINUSE) fprintf(stderr, "client_%s %u: 같은 번호의 클라이언트가 실행 중이다\n", tp_name(), opt_id);
        else fprintf(stderr, "client_%s %u: 전송 계층 열기: %s\n", tp_name(), opt_id, err_str(errno, eb, sizeof eb));
        return 1;
    }

    pthread_t sth, rth;
    if (pthread_create(&sig_th, NULL, sig_main, NULL) || pthread_create(&rth, NULL, recv_main, NULL) ||
        pthread_create(&sth, NULL, send_main, NULL)) {
        fprintf(stderr, "client: 스레드 만들기 실패\n");
        tp_client_close();
        return 1;
    }
    pthread_join(sth, NULL);
    pthread_join(rth, NULL);
    pthread_kill(sig_th, SIGUSR1);
    pthread_join(sig_th, NULL);
    tp_client_close();

    uint64_t err = atomic_load(&errors);
    bool ok = measured_all && err == 0;
    write_dump();

    /* 결과 (측정 구간 밖) */
    uint64_t *a = malloc((n_draw + n_snap + 1) * sizeof *a);
    lat_stats_t all = { 0 }, sd = { 0 }, ss = { 0 };
    if (a) {
        for (uint64_t i = 0; i < n_draw; i++) a[i] = lat_draw[i];
        lat_stats(a, n_draw, &sd);
        for (uint64_t i = 0; i < n_snap; i++) a[i] = lat_snap[i];
        lat_stats(a, n_snap, &ss);
        for (uint64_t i = 0; i < n_draw; i++) a[i] = lat_draw[i];
        for (uint64_t i = 0; i < n_snap; i++) a[n_draw + i] = lat_snap[i];
        lat_stats(a, n_draw + n_snap, &all);
        free(a);
    }
    double el = t_last > t_start ? (double)(t_last - t_start) / 1e9 : 0;
    usage_t u;
    usage_diff(&u_end, &u_start, &u);
    double nd = n_done ? (double)n_done : 1;
    if (!opt_quiet) {
        printf("client_%s %u: 그림판 %u, 동시 요청 %u, 측정 요청 %llu (DRAW %llu, SNAPSHOT %llu), %.3f s, 오류 %llu%s\n",
               tp_name(), opt_id, opt_room, opt_f, (unsigned long long)n_done, (unsigned long long)n_draw,
               (unsigned long long)n_snap, el, (unsigned long long)err, ok ? "" : " (끝까지 재지 못함)");
        printf("  지연 µs  전체 평균 %.2f 중앙 %.2f 상위99%% %.2f 최대 %.2f\n", all.mean * LAT_UNIT_NS / 1000.0,
               us_of(all.p50), us_of(all.p99), us_of(all.max));
        printf("           DRAW 중앙 %.2f 상위99%% %.2f / SNAPSHOT 중앙 %.2f 상위99%% %.2f\n", us_of(sd.p50), us_of(sd.p99),
               us_of(ss.p50), us_of(ss.p99));
        printf("  구간 µs  요청 %.2f 큐 %.2f 락 %.2f 처리 %.2f 응답 %.2f\n", seg_ns[0] / nd / 1000, seg_ns[1] / nd / 1000,
               seg_ns[2] / nd / 1000, seg_ns[3] / nd / 1000, seg_ns[4] / nd / 1000);
        printf("  처리량   %.0f 요청/s, %.1f MB/s   CPU %.3f+%.3f s, 문맥 교환 %ld/%ld, VmHWM %ld kB\n",
               el > 0 ? n_done / el : 0, el > 0 ? bytes_recv / el / 1e6 : 0, u.utime_s, u.stime_s, u.nvcsw, u.nivcsw,
               vm_hwm_kb());
        if (have_stats)
            printf("  서버     DRAW %llu, SNAPSHOT %llu, 오류 %llu, 접속 %u, 그림판 %u, 워커 %u\n",
                   (unsigned long long)last_stats.n_draw, (unsigned long long)last_stats.n_snapshot,
                   (unsigned long long)last_stats.n_error, last_stats.n_clients, last_stats.n_rooms, last_stats.n_workers);
        if (atomic_load(&ts_errors)) printf("  서버 시각 순서 오류 %llu\n", (unsigned long long)atomic_load(&ts_errors));
    }
    if (opt_csv && ok) {
        app_row_t r;
        app_row_init(&r);
        r.ipc = tp_name(); r.role = "client"; r.experiment = opt_exp; r.commit = getenv("BENCH_COMMIT"); r.run = opt_run;
        r.clients = 1; r.inflight = opt_f; r.rate_hz = opt_hz; r.draw_pct = W.draw_pct; r.rect = W.rect; r.snap = W.snap;
        r.n = (long)n_done; r.errors = (long)err; r.elapsed_s = el;
        if (el > 0) { r.thr_req_s = n_done / el; r.thr_mb_s = bytes_recv / el / 1e6; }
        r.lat_mean_us = all.mean * LAT_UNIT_NS / 1000.0; r.lat_p50_us = us_of(all.p50);
        r.lat_p99_us = us_of(all.p99); r.lat_max_us = us_of(all.max);
        r.draw_n = (long)n_draw; r.draw_mean_us = sd.mean * LAT_UNIT_NS / 1000.0; r.draw_p50_us = us_of(sd.p50);
        r.draw_p99_us = us_of(sd.p99); r.draw_max_us = us_of(sd.max);
        r.snap_n = (long)n_snap; r.snap_mean_us = ss.mean * LAT_UNIT_NS / 1000.0; r.snap_p50_us = us_of(ss.p50);
        r.snap_p99_us = us_of(ss.p99); r.snap_max_us = us_of(ss.max);
        r.req_us = seg_ns[0] / nd / 1000; r.queue_us = seg_ns[1] / nd / 1000; r.lock_us = seg_ns[2] / nd / 1000;
        r.work_us = seg_ns[3] / nd / 1000; r.resp_us = seg_ns[4] / nd / 1000;
        r.utime_s = u.utime_s; r.stime_s = u.stime_s; r.nvcsw = u.nvcsw; r.nivcsw = u.nivcsw;
        r.vmrss_kb = vm_rss_kb(); r.vmhwm_kb = vm_hwm_kb();
        if (app_csv_append(opt_csv, &r) < 0) perror("client: CSV");
    }
    free(lat_draw);
    free(lat_snap);
    free(slots);
    free(free_stack);
    return ok ? 0 : 1;

bad:
    usage(argv[0]);
    return 2;
}
