/*
 * util.c - 측정 도우미 (시각, 통계, 자원 사용량, 결과 CSV)
 *
 * 측정 방법의 원본은 docs/measurement/README.md 다.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "util.h"

/* ---- 시각 ---- */

uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void clock_cost_samples(uint64_t *v, size_t n)
{
    uint64_t prev = now_ns();
    for (size_t i = 0; i < n; i++) {
        uint64_t t = now_ns();
        v[i] = t - prev;
        prev = t;
    }
}

/* ---- 통계 ---- */

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* 정렬된 v 에서 nearest-rank 백분위수. q 는 0 ~ 1 */
static uint64_t rank_value(const uint64_t *v, size_t n, double q)
{
    double want = q * (double)n;
    size_t rank = (size_t)want;
    if ((double)rank < want) rank++;            /* ceil, libm 없이 */
    if (rank < 1) rank = 1;
    if (rank > n) rank = n;
    return v[rank - 1];
}

void lat_stats(uint64_t *v, size_t n, lat_stats_t *out)
{
    memset(out, 0, sizeof *out);
    out->n = n;
    if (n == 0) return;

    qsort(v, n, sizeof *v, cmp_u64);
    long double sum = 0;
    for (size_t i = 0; i < n; i++) sum += v[i];
    out->mean = (double)(sum / n);
    out->p50 = rank_value(v, n, 0.50);
    out->p99 = rank_value(v, n, 0.99);
    out->max = v[n - 1];
}

/* ---- 자원 사용량 ---- */

static double tv_sec(struct timeval tv) { return (double)tv.tv_sec + (double)tv.tv_usec / 1e6; }

void usage_now(usage_t *u)
{
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) < 0) {
        memset(u, 0, sizeof *u);
        return;
    }
    u->utime_s = tv_sec(ru.ru_utime);
    u->stime_s = tv_sec(ru.ru_stime);
    u->nvcsw = ru.ru_nvcsw;
    u->nivcsw = ru.ru_nivcsw;
}

void usage_diff(const usage_t *end, const usage_t *start, usage_t *out)
{
    out->utime_s = end->utime_s - start->utime_s;
    out->stime_s = end->stime_s - start->stime_s;
    out->nvcsw = end->nvcsw - start->nvcsw;
    out->nivcsw = end->nivcsw - start->nivcsw;
}

/* /proc/self/status 에서 "<key>:  1234 kB" 줄의 값을 읽는다 */
static long status_kb(const char *key)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    size_t klen = strlen(key);
    long v = -1;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, klen) == 0 && line[klen] == ':') {
            v = strtol(line + klen + 1, NULL, 10);
            break;
        }
    }
    fclose(f);
    return v;
}

long vm_rss_kb(void) { return status_kb("VmRSS"); }
long vm_hwm_kb(void) { return status_kb("VmHWM"); }

/* ---- 오류 문구 ---- */

const char *err_str(int e, char *buf, size_t len)
{
#if (_POSIX_C_SOURCE >= 200112L) && !defined(_GNU_SOURCE)
    /* XSI 판: 성공하면 0 을 돌려주고 buf 를 채운다 */
    if (strerror_r(e, buf, len) != 0) snprintf(buf, len, "오류 %d", e);
    return buf;
#else
    /* GNU 판: 설명 문자열을 돌려준다 (buf 가 아닐 수 있다) */
    return strerror_r(e, buf, len);
#endif
}

/* ---- 결과 CSV ---- */

void csv_row_init(csv_row_t *r)
{
    r->stage = r->ipc = r->role = r->experiment = r->commit = r->lock = NULL;
    r->run = -1;
    r->msg_size = r->clients = r->inflight = r->workers = r->tile = -1;
    r->draw_pct = r->rect = r->snap = -1;
    r->n = -1;
    r->elapsed_s = -1;
    r->lat_mean_us = r->lat_p50_us = r->lat_p99_us = r->lat_max_us = -1;
    r->thr_msg_s = r->thr_mb_s = -1;
    r->req_us = r->queue_us = r->lock_us = r->work_us = r->resp_us = -1;
    r->utime_s = r->stime_s = -1;
    r->nvcsw = r->nivcsw = r->vmrss_kb = r->vmhwm_kb = -1;
}

/* 버퍼 b 에 칸 하나와 뒤따르는 쉼표(마지막 칸이 아니면)를 붙인다 */
typedef struct { char buf[2048]; size_t len; } line_t;

static void put(line_t *l, const char *s)
{
    int w = snprintf(l->buf + l->len, sizeof l->buf - l->len, "%s,", s ? s : "");
    if (w > 0 && (size_t)w < sizeof l->buf - l->len) l->len += (size_t)w;
}

static void put_long(line_t *l, long v)
{
    char s[32] = "";
    if (v >= 0) snprintf(s, sizeof s, "%ld", v);
    put(l, s);
}

static void put_dbl(line_t *l, double v, int prec)
{
    char s[64] = "";
    if (v >= 0) snprintf(s, sizeof s, "%.*f", prec, v);
    put(l, s);
}

/*
 * path 에 줄 하나를 덧붙인다. 파일이 비어 있으면 머리글 hdr 부터 쓴다. 머리글이 다른 파일에는 쓰지 않는다 (EBADMSG).
 * 여러 프로세스가 같은 파일에 써도 줄이 섞이지 않게 쓰는 동안 flock 을 건다.
 */
static int append_line(const char *path, const char *hdr, size_t hlen, const char *line, size_t len)
{
    int fd = open(path, O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    while (flock(fd, LOCK_EX) < 0) {
        if (errno != EINTR) { close(fd); return -1; }
    }
    int rc = 0;
    struct stat st;
    if (fstat(fd, &st) < 0) rc = -1;
    if (rc == 0 && st.st_size == 0) {
        if (write(fd, hdr, hlen) != (ssize_t)hlen) rc = -1;
    } else if (rc == 0) {
        /* 머리글이 다른 파일(열을 바꾸기 전의 결과 등)에 덧붙이면 열이 어긋난다. 쓰지 않는다 */
        char got[1024];
        if (hlen > sizeof got || pread(fd, got, hlen, 0) != (ssize_t)hlen || memcmp(got, hdr, hlen) != 0) {
            errno = EBADMSG;
            rc = -1;
        }
    }
    if (rc == 0 && write(fd, line, len) != (ssize_t)len) rc = -1;
    int e = errno;
    flock(fd, LOCK_UN);
    close(fd);
    errno = e;
    return rc;
}

int csv_append(const char *path, const csv_row_t *r)
{
    line_t l = { .len = 0 };
    put(&l, r->stage);      put(&l, r->ipc);        put(&l, r->role);
    put(&l, r->experiment); put(&l, r->commit);     put_long(&l, r->run);
    put_long(&l, r->msg_size); put_long(&l, r->clients); put_long(&l, r->inflight);
    put_long(&l, r->workers);  put(&l, r->lock);    put_long(&l, r->tile);
    put_long(&l, r->draw_pct); put_long(&l, r->rect); put_long(&l, r->snap);
    put_long(&l, r->n);        put_dbl(&l, r->elapsed_s, 6);
    put_dbl(&l, r->lat_mean_us, 3); put_dbl(&l, r->lat_p50_us, 3);
    put_dbl(&l, r->lat_p99_us, 3);  put_dbl(&l, r->lat_max_us, 3);
    put_dbl(&l, r->thr_msg_s, 1);   put_dbl(&l, r->thr_mb_s, 3);
    put_dbl(&l, r->req_us, 3);  put_dbl(&l, r->queue_us, 3); put_dbl(&l, r->lock_us, 3);
    put_dbl(&l, r->work_us, 3); put_dbl(&l, r->resp_us, 3);
    put_dbl(&l, r->utime_s, 6); put_dbl(&l, r->stime_s, 6);
    put_long(&l, r->nvcsw);     put_long(&l, r->nivcsw);
    put_long(&l, r->vmrss_kb);  put_long(&l, r->vmhwm_kb);
    if (l.len > 0) l.buf[l.len - 1] = '\n';      /* 마지막 쉼표를 줄바꿈으로 */
    static const char hdr[] = CSV_HEADER "\n";
    return append_line(path, hdr, sizeof hdr - 1, l.buf, l.len);
}

/* ---- 2단계 CSV ---- */

void app_row_init(app_row_t *r)
{
    memset(r, 0, sizeof *r);
    r->run = -1;
    long *ls[] = { &r->clients, &r->rooms, &r->inflight, &r->workers, &r->tile, &r->draw_pct, &r->rect, &r->snap,
                   &r->n, &r->errors, &r->draw_n, &r->snap_n, &r->nvcsw, &r->nivcsw, &r->vmrss_kb, &r->vmhwm_kb,
                   &r->ipc_cap_kb, &r->ipc_rss_kb, &r->ipc_min_chan, &r->checksum_ok };
    for (size_t i = 0; i < sizeof ls / sizeof *ls; i++) *ls[i] = -1;
    double *ds[] = { &r->rate_hz, &r->elapsed_s, &r->thr_req_s, &r->thr_mb_s,
                     &r->lat_mean_us, &r->lat_p50_us, &r->lat_p99_us, &r->lat_max_us,
                     &r->draw_mean_us, &r->draw_p50_us, &r->draw_p99_us, &r->draw_max_us,
                     &r->snap_mean_us, &r->snap_p50_us, &r->snap_p99_us, &r->snap_max_us,
                     &r->req_us, &r->queue_us, &r->lock_us, &r->work_us, &r->resp_us, &r->utime_s, &r->stime_s };
    for (size_t i = 0; i < sizeof ds / sizeof *ds; i++) *ds[i] = -1;
}

int app_csv_append(const char *path, const app_row_t *r)
{
    line_t l = { .len = 0 };
    put(&l, "app");         put(&l, r->ipc);        put(&l, r->role);
    put(&l, r->experiment); put(&l, r->commit);     put_long(&l, r->run);
    put_long(&l, r->clients); put_long(&l, r->rooms); put_long(&l, r->inflight); put_dbl(&l, r->rate_hz, 1);
    put_long(&l, r->workers); put(&l, r->lock);     put_long(&l, r->tile);
    put_long(&l, r->draw_pct); put_long(&l, r->rect); put_long(&l, r->snap);
    put_long(&l, r->n);       put_long(&l, r->errors); put_dbl(&l, r->elapsed_s, 6);
    put_dbl(&l, r->thr_req_s, 1); put_dbl(&l, r->thr_mb_s, 3);
    put_dbl(&l, r->lat_mean_us, 3); put_dbl(&l, r->lat_p50_us, 3); put_dbl(&l, r->lat_p99_us, 3); put_dbl(&l, r->lat_max_us, 3);
    put_long(&l, r->draw_n);
    put_dbl(&l, r->draw_mean_us, 3); put_dbl(&l, r->draw_p50_us, 3); put_dbl(&l, r->draw_p99_us, 3); put_dbl(&l, r->draw_max_us, 3);
    put_long(&l, r->snap_n);
    put_dbl(&l, r->snap_mean_us, 3); put_dbl(&l, r->snap_p50_us, 3); put_dbl(&l, r->snap_p99_us, 3); put_dbl(&l, r->snap_max_us, 3);
    put_dbl(&l, r->req_us, 3);  put_dbl(&l, r->queue_us, 3); put_dbl(&l, r->lock_us, 3);
    put_dbl(&l, r->work_us, 3); put_dbl(&l, r->resp_us, 3);
    put_dbl(&l, r->utime_s, 6); put_dbl(&l, r->stime_s, 6);
    put_long(&l, r->nvcsw);     put_long(&l, r->nivcsw);
    put_long(&l, r->vmrss_kb);  put_long(&l, r->vmhwm_kb);
    put_long(&l, r->ipc_cap_kb); put_long(&l, r->ipc_rss_kb); put_long(&l, r->ipc_min_chan);
    put_long(&l, r->checksum_ok);
    put(&l, r->env);
    if (l.len > 0) l.buf[l.len - 1] = '\n';
    static const char hdr[] = APP_CSV_HEADER "\n";
    return append_line(path, hdr, sizeof hdr - 1, l.buf, l.len);
}
