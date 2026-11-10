/*
 * proto.h - 공유 캔버스 서버 메시지 형식 (세 IPC 버전 공통)
 *
 * 초안입니다. 11/13까지 팀이 확정한 뒤에는 바꿀 때 반드시 세 사람이 합의합니다.
 * 모든 메시지는 msg_hdr_t 로 시작하고, 그 뒤에 payload_len 바이트가 붙습니다.
 */
#ifndef PROTO_H
#define PROTO_H

#include <stdint.h>

#define CANVAS_W   1024u            /* 가로 칸 수 */
#define CANVAS_H   1024u            /* 세로 칸 수, 한 칸 1바이트 → 전체 1MB */
#define TILE_SIZE  64u              /* 락 단위 구역 한 변 */
#define TILE_COLS  (CANVAS_W / TILE_SIZE)
#define TILE_ROWS  (CANVAS_H / TILE_SIZE)
#define TILE_COUNT (TILE_COLS * TILE_ROWS)   /* 256 */

#define MAX_CLIENTS 16u
#define MAX_PAYLOAD (CANVAS_W * CANVAS_H)    /* 가장 큰 응답 = 캔버스 전체 */

enum msg_type {
    REQ_HELLO    = 1,   /* 클라이언트 등록 */
    REQ_DRAW     = 2,   /* 사각형 영역의 각 칸에 v 더하기 (mod 256) */
    REQ_SNAPSHOT = 3,   /* 사각형 영역 복사해서 받기 */
    REQ_STATS    = 4,   /* 서버 통계 받기 */
    REQ_BYE      = 5,   /* 클라이언트 종료 알림 */
    RESP_OK      = 100,
    RESP_ERROR   = 101,
};

/* 모든 요청과 응답의 머리 */
typedef struct {
    uint32_t type;          /* enum msg_type */
    uint32_t client_id;     /* 0 .. MAX_CLIENTS-1 */
    uint64_t req_id;        /* 클라이언트가 붙이는 일련번호, 응답에 그대로 돌아옴 */
    uint32_t payload_len;   /* 머리 뒤에 붙는 바이트 수 */
    uint32_t reserved;
} msg_hdr_t;

/* REQ_DRAW payload */
typedef struct {
    uint16_t x, y, w, h;
    uint8_t  v;
    uint8_t  pad[7];
} draw_req_t;

/* REQ_SNAPSHOT payload */
typedef struct {
    uint16_t x, y, w, h;
} snap_req_t;

/*
 * 모든 응답 payload 의 맨 앞에 붙는 서버 시각 (CLOCK_MONOTONIC, 나노초).
 * 같은 컴퓨터의 프로세스는 같은 시계를 쓰므로 클라이언트가 구간별 지연을 계산할 수 있다.
 *   요청 전송 = t_recv - (클라이언트 송신 시각)
 *   큐 대기   = t_deq  - t_recv
 *   락 대기   = t_lock - t_deq
 *   처리      = t_send - t_lock
 *   응답 전송 = (클라이언트 수신 시각) - t_send
 */
typedef struct {
    uint64_t t_recv;    /* 수신 스레드가 요청을 꺼낸 시각 */
    uint64_t t_deq;     /* 워커가 작업 큐에서 꺼낸 시각 */
    uint64_t t_lock;    /* 락을 모두 잡은 시각 */
    uint64_t t_send;    /* 응답을 보내기 직전 시각 */
} srv_ts_t;

/* REQ_STATS 응답 payload (srv_ts_t 뒤에 붙음) */
typedef struct {
    uint64_t n_draw;
    uint64_t n_snapshot;
    uint64_t bytes_sent;
    uint32_t n_clients;
    uint32_t n_workers;
} stats_resp_t;

#endif /* PROTO_H */
