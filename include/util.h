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

/* ---- 오류 문구 ---- */

/*
 * strerror 의 스레드 안전판. buf 에 errno e 의 설명을 써서 돌려준다.
 * strerror 는 POSIX 가 스레드 안전을 보장하지 않으므로(정적 버퍼를 쓸 수 있다) 스레드에서는 이것을 쓴다.
 */
const char *err_str(int e, char *buf, size_t len);

/* ---- 결과 CSV ---- */

/* 열 순서의 원본은 docs/measurement/README.md#원자료-형식 이다. 바꾸면 그 문서부터 고친다 */
#define CSV_HEADER \
    "stage,ipc,role,experiment,commit,run," \
    "msg_size,clients,inflight,workers,lock,tile,draw_pct,rect,snap," \
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
    long   tile;                       /* 서버 -t 타일 한 변 */
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
 * 머리글이 CSV_HEADER 와 다른 파일에는 쓰지 않는다 (errno = EBADMSG).
 * 측정 구간 밖에서만 부른다. 성공 0, 실패 -1.
 */
int csv_append(const char *path, const csv_row_t *r);

/* ---- 2단계 CSV ---- */

/* 열 순서의 원본은 docs/measurement/README.md#원자료-형식 의 2단계 파일 설명이다. 줄 하나 = 실행 하나의 클라이언트 전체 또는 서버 */
#define APP_CSV_HEADER \
    "stage,ipc,role,experiment,commit,run," \
    "clients,rooms,inflight,rate_hz,workers,lock,tile,draw_pct,rect,snap," \
    "n,errors,elapsed_s,thr_req_s,thr_mb_s," \
    "lat_mean_us,lat_p50_us,lat_p99_us,lat_max_us," \
    "draw_n,draw_mean_us,draw_p50_us,draw_p99_us,draw_max_us," \
    "snap_n,snap_mean_us,snap_p50_us,snap_p99_us,snap_max_us," \
    "req_us,queue_us,lock_us,work_us,resp_us," \
    "utime_s,stime_s,nvcsw,nivcsw,vmrss_kb,vmhwm_kb," \
    "ipc_cap_kb,ipc_rss_kb,ipc_min_chan,checksum_ok,env"

/* 값이 음수이거나 문자열이 NULL 이면 빈 칸. app_row_init 으로 모두 빈 칸으로 만든 뒤 채운다 */
typedef struct {
    const char *ipc, *role, *experiment, *commit, *lock, *env;
    long   run;
    long   clients, rooms, inflight;
    double rate_hz;
    long   workers, tile, draw_pct, rect, snap;
    long   n, errors;
    double elapsed_s, thr_req_s, thr_mb_s;
    double lat_mean_us, lat_p50_us, lat_p99_us, lat_max_us;
    long   draw_n;
    double draw_mean_us, draw_p50_us, draw_p99_us, draw_max_us;
    long   snap_n;
    double snap_mean_us, snap_p50_us, snap_p99_us, snap_max_us;
    double req_us, queue_us, lock_us, work_us, resp_us;
    double utime_s, stime_s;
    long   nvcsw, nivcsw, vmrss_kb, vmhwm_kb;
    long   ipc_cap_kb, ipc_rss_kb, ipc_min_chan, checksum_ok;
} app_row_t;

void app_row_init(app_row_t *r);
int  app_csv_append(const char *path, const app_row_t *r);    /* csv_append 와 같은 규칙 (flock, 머리글 확인) */

#endif /* UTIL_H */
