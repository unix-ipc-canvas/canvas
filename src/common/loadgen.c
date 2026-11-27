/*
 * loadgen.c - 실행기: 부하 클라이언트 N개(최대 1024)를 띄우고 측정 시작을 맞추고 결과를 합친다 (요구사항 F-12)
 *
 *   loadgen -N 개수 [-K 그림판] [-P 서버pid] [-o CSV -e 실험 -R 반복] [-M 목록] [-T 초] [-q] -- 클라이언트 [옵션...]
 *
 * 순서: 클라이언트마다 fork → exec(클라이언트 -i 번호 -k 번호%K -G -q ...). 모두 워밍업을 마치고 준비를 알리면
 * 서버에 SIGUSR1(측정 시작 표시), 클라이언트들의 프로세스 그룹에 SIGUSR2(측정 시작)를 한 번에 보낸다.
 * 모두 끝나면 서버에 다시 SIGUSR1(끝 표시)을 보내고, 클라이언트들이 남긴 표본 파일을 합쳐 한 실행의 결과를 낸다.
 *
 * 준비 알림은 상속한 익명 pipe 로 받는다(측정 대상이 아닌 제어 통로). IPC 고유 호출은 쓰지 않는다.
 * 스레드: main, 대기(waitpid), 신호(SIGINT·SIGTERM 을 클라이언트에게 넘김).
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "proto.h"
#include "util.h"
#include "latdump.h"

static uint32_t opt_n = 1, opt_rooms = 8;
static pid_t opt_server;
static const char *opt_csv, *opt_exp = "app", *opt_manifest;
static long opt_run = 1;
static double opt_timeout = 120;
static bool opt_quiet;

static pid_t *pids;
static int *status;
static pid_t pgid;
static uint32_t n_exited;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static pthread_t sig_th;

static void *wait_main(void *arg)
{
    (void)arg;
    for (;;) {
        int st;
        pid_t p = waitpid(-1, &st, 0);
        if (p < 0) {
            if (errno == EINTR) continue;
            break;                                   /* ECHILD: 모두 끝났다 */
        }
        pthread_mutex_lock(&mtx);
        for (uint32_t i = 0; i < opt_n; i++)
            if (pids[i] == p) { status[i] = st; n_exited++; break; }
        pthread_cond_broadcast(&cond);
        pthread_mutex_unlock(&mtx);
    }
    return NULL;
}

static void *sig_main(void *arg)
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
        if (sig == SIGUSR1) break;                   /* main: 끝 */
        if (pgid > 0) killpg(pgid, SIGTERM);         /* 클라이언트는 다른 프로세스 그룹이라 터미널의 Ctrl+C 를 넘긴다 */
    }
    return NULL;
}

static double now_s(void) { return (double)now_ns() / 1e9; }

/* 모두 끝날 때까지 기다린다. limit 초를 넘으면 false */
static bool wait_all(double limit)
{
    double end = now_s() + limit;
    pthread_mutex_lock(&mtx);
    while (n_exited < opt_n) {
        double left = end - now_s();
        if (left <= 0) break;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += (time_t)left;
        ts.tv_nsec += (long)((left - (double)(time_t)left) * 1e9);
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&cond, &mtx, &ts);
    }
    bool all = n_exited >= opt_n;
    pthread_mutex_unlock(&mtx);
    return all;
}

static void kill_all(int sig)
{
    if (pgid > 0) killpg(pgid, sig);
}

static void remove_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        char p[600];
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            unlink(p);
        }
        closedir(d);
    }
    rmdir(dir);
}

static void usage(const char *prog)
{
    fprintf(stderr, "사용법: %s -N 개수 [-K 그림판] [-P 서버pid] [-o CSV -e 실험 -R 반복] [-M 목록] [-T 초] [-q] -- 클라이언트 [옵션...]\n",
            prog);
}

static double us_of(uint64_t v) { return (double)v * LAT_UNIT_NS / 1000.0; }

int main(int argc, char **argv)
{
    int c;
    while ((c = getopt(argc, argv, "+N:K:P:o:e:R:M:T:qh")) != -1) {
        switch (c) {
        case 'N': opt_n = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'K': opt_rooms = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'P': opt_server = (pid_t)strtol(optarg, NULL, 10); break;
        case 'o': opt_csv = optarg; break;
        case 'e': opt_exp = optarg; break;
        case 'R': opt_run = strtol(optarg, NULL, 10); break;
        case 'M': opt_manifest = optarg; break;
        case 'T': opt_timeout = atof(optarg); break;
        case 'q': opt_quiet = true; break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 2;
        }
    }
    if (opt_n < 1 || opt_n > MAX_CLIENTS || opt_rooms < 1 || opt_rooms > MAX_ROOMS || optind >= argc) {
        usage(argv[0]);
        return 2;
    }
    char **cargv = argv + optind;
    int cargc = argc - optind;

    char dir[] = "/tmp/osproject_2_lat.XXXXXX";
    if (!mkdtemp(dir)) { perror("loadgen: mkdtemp"); return 1; }
    int rp[2];
    if (pipe2(rp, O_CLOEXEC) < 0) { perror("loadgen: pipe"); remove_dir(dir); return 1; }

    pids = calloc(opt_n, sizeof *pids);
    status = calloc(opt_n, sizeof *status);
    char **av = calloc((size_t)cargc + 10, sizeof *av);
    if (!pids || !status || !av) { fprintf(stderr, "loadgen: 메모리 부족\n"); return 1; }

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    pthread_t wth;
    if (pthread_create(&sig_th, NULL, sig_main, NULL) != 0) { fprintf(stderr, "loadgen: 스레드\n"); return 1; }

    char rfd[16];
    snprintf(rfd, sizeof rfd, "%d", rp[1]);
    double t0 = now_s();
    uint32_t started = 0;
    for (uint32_t i = 0; i < opt_n; i++) {
        char id[16], room[16];
        snprintf(id, sizeof id, "%u", i);
        snprintf(room, sizeof room, "%u", i % opt_rooms);
        int k = 0;
        for (int j = 0; j < cargc; j++) av[k++] = cargv[j];
        av[k++] = "-i"; av[k++] = id; av[k++] = "-k"; av[k++] = room; av[k++] = "-G"; av[k++] = "-q";
        av[k] = NULL;
        pid_t p = fork();
        if (p < 0) { perror("loadgen: fork"); break; }
        if (p == 0) {
            /* 자식: 클라이언트들만의 프로세스 그룹. 첫 자식이 그룹을 만든다 */
            setpgid(0, pgid);
            sigset_t none;
            sigemptyset(&none);
            pthread_sigmask(SIG_SETMASK, &none, NULL);
            int fl = fcntl(rp[1], F_GETFD);
            fcntl(rp[1], F_SETFD, fl & ~FD_CLOEXEC);      /* 준비 알림용 쓰기 끝만 물려준다 */
            setenv("OSP_READY_FD", rfd, 1);
            setenv("OSP_LAT_DIR", dir, 1);
            execv(av[0], av);
            _exit(127);
        }
        setpgid(p, pgid ? pgid : p);                     /* 부모에서도 걸어 경쟁을 없앤다 */
        if (!pgid) pgid = p;
        pids[i] = p;
        started++;
    }
    close(rp[1]);
    if (pthread_create(&wth, NULL, wait_main, NULL) != 0) { fprintf(stderr, "loadgen: 스레드\n"); kill_all(SIGKILL); return 1; }

    /* 준비 알림 N 바이트를 기다린다 (모두 워밍업을 마쳤다) */
    uint32_t ready = 0;
    bool fail = started < opt_n;
    while (!fail && ready < opt_n) {
        double left = opt_timeout - (now_s() - t0);
        if (left <= 0) { fail = true; break; }
        struct pollfd pf = { rp[0], POLLIN, 0 };
        int r = poll(&pf, 1, (int)(left * 1000 < 1000 ? left * 1000 : 1000));
        if (r < 0 && errno != EINTR) { fail = true; break; }
        if (r > 0) {
            char b[256];
            ssize_t got = read(rp[0], b, sizeof b);
            if (got > 0) ready += (uint32_t)got;
            else if (got == 0) { fail = true; break; }   /* 준비 전에 모두 끝났다 */
        }
        pthread_mutex_lock(&mtx);
        if (n_exited > 0) fail = true;                   /* 준비 전에 끝난 클라이언트가 있다 */
        pthread_mutex_unlock(&mtx);
    }
    double t_ready = now_s();
    if (fail) {
        fprintf(stderr, "loadgen: 클라이언트 %u개 중 %u개만 준비됨\n", opt_n, ready);
        kill_all(SIGTERM);
        if (!wait_all(5)) { kill_all(SIGKILL); wait_all(5); }
    } else {
        if (opt_server > 0) kill(opt_server, SIGUSR1);   /* 서버: 측정 시작 표시 */
        kill_all(SIGUSR2);                               /* 클라이언트 전체: 측정 시작 */
        if (!wait_all(opt_timeout)) {
            fprintf(stderr, "loadgen: 제한 시간 %.0f초 안에 끝나지 않음\n", opt_timeout);
            fail = true;
            kill_all(SIGTERM);
            if (!wait_all(5)) { kill_all(SIGKILL); wait_all(5); }
        }
        if (opt_server > 0) kill(opt_server, SIGUSR1);   /* 서버: 측정 끝 표시 */
    }
    close(rp[0]);
    pthread_join(wth, NULL);

    /* 표본 파일 합치기 */
    uint64_t n_sent = 0, n_done = 0, errs = 0, bytes = 0, seg[5] = { 0 }, n_drop = 0, tmin = UINT64_MAX, tmax = 0;
    double ut = 0, st = 0;
    int64_t nv = 0, niv = 0, rss = 0, hwm = 0;
    uint32_t n_ok = 0, n_exit0 = 0;
    size_t cap = 1 << 20, nd = 0, ns = 0;
    uint64_t *ad = malloc(cap * sizeof *ad), *as = malloc(cap * sizeof *as);
    size_t capd = cap, caps = cap;
    FILE *mf = opt_manifest ? fopen(opt_manifest, "w") : NULL;
    for (uint32_t i = 0; i < opt_n; i++) {
        if (WIFEXITED(status[i]) && WEXITSTATUS(status[i]) == 0) n_exit0++;
        char p[600];
        snprintf(p, sizeof p, "%s/%u.lat", dir, i);
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        lat_hdr_t h;
        if (fread(&h, sizeof h, 1, f) == 1 && h.magic == LAT_MAGIC) {
            if (h.ok) n_ok++;
            n_sent += h.n_sent; n_done += h.n_done; errs += h.errors; bytes += h.bytes; n_drop += h.n_drop;
            for (int k = 0; k < 5; k++) seg[k] += h.seg_ns[k];
            if (h.n_done) {
                if (h.t_start < tmin) tmin = h.t_start;
                if (h.t_last > tmax) tmax = h.t_last;
            }
            ut += h.utime_s; st += h.stime_s; nv += h.nvcsw; niv += h.nivcsw;
            if (h.vmrss_kb > rss) rss = h.vmrss_kb;
            if (h.vmhwm_kb > hwm) hwm = h.vmhwm_kb;
            if (mf) fprintf(mf, "%u %llu %u %llu %u %u %u\n", h.id, (unsigned long long)h.seed, h.room,
                            (unsigned long long)h.n_sent, h.draw_pct, h.rect, h.snap);
            for (int t = 0; t < 2; t++) {
                uint64_t cnt = t == 0 ? h.n_draw : h.n_snap;
                uint64_t **arr = t == 0 ? &ad : &as;
                size_t *nn = t == 0 ? &nd : &ns, *cp = t == 0 ? &capd : &caps;
                if (*nn + cnt > *cp) {
                    while (*nn + cnt > *cp) *cp *= 2;
                    *arr = realloc(*arr, *cp * sizeof **arr);
                }
                for (uint64_t k = 0; k < cnt; k++) {
                    uint32_t v;
                    if (fread(&v, sizeof v, 1, f) != 1) break;
                    (*arr)[(*nn)++] = v;
                }
            }
        }
        fclose(f);
    }
    if (mf) fclose(mf);
    remove_dir(dir);

    lat_stats_t sd, ss, sa;
    uint64_t *aa = malloc((nd + ns + 1) * sizeof *aa);
    memcpy(aa, ad, nd * sizeof *aa);
    memcpy(aa + nd, as, ns * sizeof *aa);
    lat_stats(ad, nd, &sd);
    lat_stats(as, ns, &ss);
    lat_stats(aa, nd + ns, &sa);
    double el = tmax > tmin ? (double)(tmax - tmin) / 1e9 : 0;
    double dn = n_done ? (double)n_done : 1;
    bool ok = !fail && n_exit0 == opt_n && n_ok == opt_n && errs == 0;

    if (!opt_quiet) {
        printf("loadgen: 클라이언트 %u개 (정상 종료 %u, 측정 완료 %u), 준비 %.2f s, 측정 %.3f s, 오류 %llu%s\n", opt_n, n_exit0,
               n_ok, t_ready - t0, el, (unsigned long long)errs, ok ? "" : " — 실패");
        printf("  요청 %llu (DRAW %zu, SNAPSHOT %zu), 처리량 %.0f 요청/s, %.1f MB/s\n", (unsigned long long)n_done, nd, ns,
               el > 0 ? n_done / el : 0, el > 0 ? bytes / el / 1e6 : 0);
        printf("  지연 µs  전체 중앙 %.2f 상위99%% %.2f 최대 %.2f / DRAW 중앙 %.2f 상위99%% %.2f / SNAPSHOT 중앙 %.2f 상위99%% %.2f\n",
               us_of(sa.p50), us_of(sa.p99), us_of(sa.max), us_of(sd.p50), us_of(sd.p99), us_of(ss.p50), us_of(ss.p99));
        printf("  구간 µs  요청 %.2f 큐 %.2f 락 %.2f 처리 %.2f 응답 %.2f   클라이언트 CPU %.2f+%.2f s, 문맥 교환 %lld/%lld\n",
               seg[0] / dn / 1000, seg[1] / dn / 1000, seg[2] / dn / 1000, seg[3] / dn / 1000, seg[4] / dn / 1000, ut, st,
               (long long)nv, (long long)niv);
        if (n_drop) printf("  담지 못한 표본 %llu\n", (unsigned long long)n_drop);
    }
    if (opt_csv) {
        app_row_t r;
        app_row_init(&r);
        r.ipc = getenv("OSP_IPC"); r.role = "clients"; r.experiment = opt_exp; r.commit = getenv("BENCH_COMMIT");
        r.run = opt_run; r.clients = opt_n; r.rooms = opt_rooms; r.env = getenv("OSP_ENV_NOTE");
        const char *s;
        if ((s = getenv("OSP_INFLIGHT"))) r.inflight = atol(s);
        if ((s = getenv("OSP_RATE"))) r.rate_hz = atof(s);
        if ((s = getenv("OSP_DRAW_PCT"))) r.draw_pct = atol(s);
        if ((s = getenv("OSP_RECT"))) r.rect = atol(s);
        if ((s = getenv("OSP_SNAP"))) r.snap = atol(s);
        if ((s = getenv("OSP_WORKERS"))) r.workers = atol(s);
        if ((s = getenv("OSP_LOCK"))) r.lock = s;
        if ((s = getenv("OSP_TILE"))) r.tile = atol(s);
        r.n = (long)n_done; r.errors = (long)errs + (ok ? 0 : 1); r.elapsed_s = el;
        if (el > 0) { r.thr_req_s = n_done / el; r.thr_mb_s = bytes / el / 1e6; }
        r.lat_mean_us = sa.mean * LAT_UNIT_NS / 1000.0; r.lat_p50_us = us_of(sa.p50); r.lat_p99_us = us_of(sa.p99);
        r.lat_max_us = us_of(sa.max);
        r.draw_n = (long)nd; r.draw_mean_us = sd.mean * LAT_UNIT_NS / 1000.0; r.draw_p50_us = us_of(sd.p50);
        r.draw_p99_us = us_of(sd.p99); r.draw_max_us = us_of(sd.max);
        r.snap_n = (long)ns; r.snap_mean_us = ss.mean * LAT_UNIT_NS / 1000.0; r.snap_p50_us = us_of(ss.p50);
        r.snap_p99_us = us_of(ss.p99); r.snap_max_us = us_of(ss.max);
        r.req_us = seg[0] / dn / 1000; r.queue_us = seg[1] / dn / 1000; r.lock_us = seg[2] / dn / 1000;
        r.work_us = seg[3] / dn / 1000; r.resp_us = seg[4] / dn / 1000;
        r.utime_s = ut; r.stime_s = st; r.nvcsw = (long)nv; r.nivcsw = (long)niv; r.vmrss_kb = (long)rss; r.vmhwm_kb = (long)hwm;
        if (app_csv_append(opt_csv, &r) < 0) perror("loadgen: CSV");
    }
    free(ad); free(as); free(aa);
    pthread_kill(sig_th, SIGUSR1);
    pthread_join(sig_th, NULL);
    (void)n_sent;
    return ok ? 0 : 1;
}
