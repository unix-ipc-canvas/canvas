/*
 * workload.h - 시드로 요청열을 만드는 생성기 (부하 클라이언트와 검증기가 함께 쓴다)
 *
 * 요청 i 는 (시드, i) 만으로 정해진다(상태 없음). 그래서 검증기는 클라이언트가 보낸 요청 수만 알면 같은 요청열을
 * 다시 만들 수 있다. 분포의 원본은 docs/records/prediction.md#워크로드-명세 다.
 * 2단계 측정을 시작한 뒤에는 수열을 바꾸지 않는다 (바꾸면 앞서 잰 결과와 요청열이 달라진다).
 */
#ifndef WORKLOAD_H
#define WORKLOAD_H

#include <stdint.h>

#define WL_DRAW_PCT  90u     /* 기준 DRAW 비율 % */
#define WL_RECT      16u     /* 기준 DRAW 한 변 */
#define WL_SNAP      256u    /* 기준 SNAPSHOT 한 변 */

typedef struct {
    uint64_t seed;
    uint32_t draw_pct;       /* 0 ~ 100 */
    uint32_t rect;           /* DRAW 한 변 1 ~ CANVAS_W */
    uint32_t snap;           /* SNAPSHOT 한 변 1 ~ SNAP_MAX_SIDE */
} wl_cfg_t;

typedef struct {
    uint32_t type;           /* REQ_DRAW 또는 REQ_SNAPSHOT */
    uint16_t x, y, w, h;
    uint8_t  v;              /* DRAW 값 1 ~ 255 */
} wl_req_t;

/* 측정 요청 i. 워밍업은 i 에 WL_WARM_BASE 를 더해 다른 수열을 쓰고 v 를 0 으로 보낸다 */
#define WL_WARM_BASE (1ull << 40)
void wl_gen(const wl_cfg_t *cfg, uint64_t i, wl_req_t *out);

#endif /* WORKLOAD_H */
