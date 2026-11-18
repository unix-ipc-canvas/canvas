/*
 * workload.h - 요청열 생성기 (클라이언트와 검증기가 함께 쓴다)
 *
 * 시드와 옵션이 같으면 어느 컴퓨터, 어느 IPC 버전에서든 같은 요청열을 만든다(요구사항 F-7).
 * 클라이언트는 이 요청을 서버에 보내고, 검증기는 같은 시드들의 요청을 스레드 하나로 다시 그려 정답 체크섬을 낸다(F-10).
 * 워크로드의 뜻과 기준값은 docs/records/prediction.md 의 워크로드 명세가 원본이다.
 * IPC 고유 호출은 넣지 않는다 (src/common/ 규칙).
 */
#ifndef WORKLOAD_H
#define WORKLOAD_H

#include <stddef.h>
#include <stdint.h>

#include "proto.h"

/* 기준 실험 조건 (워크로드 명세). 클라이언트 옵션 -m, -r, -S 의 기본값이다 */
#define WL_DRAW_PCT_DEFAULT 90u     /* DRAW 비율(%). 나머지는 SNAPSHOT */
#define WL_RECT_DEFAULT     16u     /* DRAW 사각형 한 변 */
#define WL_SNAP_DEFAULT     256u    /* SNAPSHOT 영역 한 변 (256 × 256 = 64KB) */

typedef struct {
    uint64_t state;                 /* 난수 상태. 직접 건드리지 않는다 */
    uint32_t draw_pct, rect, snap;
} workload_t;

/* 요청 하나. msg_hdr_t 의 type, payload_len 과 payload 를 그대로 채울 수 있다 */
typedef struct {
    uint32_t type;                  /* REQ_DRAW 또는 REQ_SNAPSHOT */
    uint32_t len;                   /* payload 바이트 (sizeof(draw_req_t) 또는 sizeof(snap_req_t)) */
    union {
        draw_req_t draw;
        snap_req_t snap;
    } p;
} wl_req_t;

/*
 * 생성기를 준비한다. 성공 0.
 * draw_pct 가 100 을 넘거나, rect·snap 이 1 ~ CANVAS_W 밖이면 -1, errno = EINVAL.
 */
int workload_init(workload_t *w, uint64_t seed, uint32_t draw_pct, uint32_t rect, uint32_t snap);

/*
 * 다음 요청을 만든다. 메모리를 잡지 않으므로 측정 구간 안에서 불러도 된다.
 * 영역은 늘 캔버스 안이고 w, h 는 0 이 아니다(오류 요청은 만들지 않는다). DRAW 의 v 는 1 ~ 255 다.
 */
void workload_next(workload_t *w, wl_req_t *r);

/* 이 요청이 성공했을 때 받을 응답 payload 바이트. 서버 시각(srv_ts_t) 뒤에 SNAPSHOT 이면 w × h 바이트가 붙는다 */
size_t workload_resp_len(const wl_req_t *r);

#endif /* WORKLOAD_H */
