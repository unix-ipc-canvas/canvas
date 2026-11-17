/*
 * util.h - 측정 도우미 (시각, 통계, 자원 사용량, 결과 CSV)
 *
 * 1단계 벤치마크와 2단계 서버·클라이언트가 같은 방법으로 재고 같은 형식으로 남기도록 한 곳에 모은다.
 * 측정 방법과 CSV 열의 원본은 docs/measurement/README.md 다.
 * IPC 고유 호출은 넣지 않는다 (src/common/ 규칙).
 */
#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <stdint.h>

/* ---- 시각 ---- */

/* CLOCK_MONOTONIC 나노초. 같은 컴퓨터의 프로세스끼리 값을 직접 뺄 수 있다 */
uint64_t now_ns(void);

/* now_ns 를 연달아 불러 한 번에 드는 시간(ns) 표본 n 개를 v 에 채운다 */
void clock_cost_samples(uint64_t *v, size_t n);

/* ---- 통계 ---- */

typedef struct {
    size_t   n;
    double   mean;
    uint64_t p50, p99, max;
} lat_stats_t;

/*
 * 표본의 평균, 중앙값, 상위 99%, 최대를 낸다. 단위는 표본과 같다.
 * v 를 정렬하므로 순서가 바뀐다. 측정 구간이 끝난 뒤에만 부른다.
 * 백분위수는 nearest-rank 방식이다: 작은 쪽부터 ceil(q * n) 번째 값.
 */
void lat_stats(uint64_t *v, size_t n, lat_stats_t *out);

/* ---- 자원 사용량 ---- */

typedef struct {
    double utime_s, stime_s;    /* 사용자·커널 CPU 시간 */
    long   nvcsw, nivcsw;       /* 자발적·비자발적 컨텍스트 스위치 */
} usage_t;

void usage_now(usage_t *u);                                     /* getrusage(RUSAGE_SELF) */
void usage_diff(const usage_t *end, const usage_t *start, usage_t *out);

/* /proc/self/status 의 VmRSS, VmHWM (kB). 읽지 못하면 -1 */
long vm_rss_kb(void);
long vm_hwm_kb(void);

/* ---- 결과 CSV ---- */

/* 열 순서의 원본은 docs/measurement/README.md#원자료-형식 이다. 바꾸면 그 문서부터 고친다 */
#define CSV_HEADER \
    "stage,ipc,role,experiment,commit,run," \
    "msg_size,clients,inflight,workers,lock,draw_pct,rect,snap," \
    "n,elapsed_s,lat_mean_us,lat_p50_us,lat_p99_us,lat_max_us,thr_msg_s,thr_mb_s," \
    "req_us,queue_us,lock_us,work_us,resp_us," \
    "utime_s,stime_s,nvcsw,nivcsw,vmrss_kb,vmhwm_kb"

/*
 * 한 줄의 값. 문자열이 NULL 이거나 수가 음수이면 빈 칸으로 쓴다.
 * csv_row_init 으로 모두 빈 칸으로 만든 뒤 해당하는 칸만 채운다.
 */
typedef struct {
    const char *stage, *ipc, *role, *experiment, *commit;
    long   run;
    long   msg_size, clients, inflight, workers;
    const char *lock;
    long   draw_pct, rect, snap;
    long   n;
    double elapsed_s;
    double lat_mean_us, lat_p50_us, lat_p99_us, lat_max_us;
    double thr_msg_s, thr_mb_s;
    double req_us, queue_us, lock_us, work_us, resp_us;
    double utime_s, stime_s;
    long   nvcsw, nivcsw, vmrss_kb, vmhwm_kb;
} csv_row_t;

void csv_row_init(csv_row_t *r);

/*
 * path 에 한 줄을 덧붙인다. 파일이 비어 있으면 머리글부터 쓴다.
 * 여러 프로세스가 같은 파일에 써도 줄이 섞이지 않게 쓰는 동안 파일 잠금(flock)을 건다.
 * 측정 구간 밖에서만 부른다. 성공 0, 실패 -1.
 */
int csv_append(const char *path, const csv_row_t *r);

#endif /* UTIL_H */
