/*
 * latdump.h - 자동 그리기 클라이언트가 남기고 실행기(loadgen)가 합치는 지연 표본 파일
 *
 * 실행 하나의 지연 분포는 모든 클라이언트의 표본을 합쳐서 낸다(클라이언트마다의 백분위수를 평균하지 않는다).
 * 클라이언트는 환경 변수 OSP_LAT_DIR 이 있으면 끝날 때 <dir>/<번호>.lat 에 머리 + 표본을 쓴다.
 * 표본은 왕복 지연을 10ns 단위로 담은 uint32_t 다 (최대 약 42초).
 */
#ifndef LATDUMP_H
#define LATDUMP_H

#include <stdint.h>

#define LAT_MAGIC 0x4c50534fu       /* "OSPL" */
#define LAT_UNIT_NS 10u

typedef struct {
    uint32_t magic;
    uint32_t id, room, inflight;
    uint64_t seed;
    uint32_t draw_pct, rect, snap, ok;      /* ok: 측정을 끝까지 했고 응답 검사 오류가 없다 */
    uint64_t n_sent;                        /* 보낸 측정 요청 수 (검증기가 재생할 수) */
    uint64_t n_done;                        /* 받은 측정 응답 수 */
    uint64_t errors;
    uint64_t t_start, t_last;               /* 측정 시작, 마지막 측정 응답 (CLOCK_MONOTONIC ns) */
    uint64_t bytes;                         /* 받은 측정 응답 payload 합 */
    uint64_t seg_ns[5];                     /* 구간별 시간 합: 요청 전송, 큐 대기, 락 대기, 처리, 응답 전송 */
    uint64_t n_draw, n_snap, n_drop;        /* 담은 표본 수, 담지 못한 표본 수 */
    double   utime_s, stime_s;
    int64_t  nvcsw, nivcsw, vmrss_kb, vmhwm_kb;
} lat_hdr_t;

#endif /* LATDUMP_H */
