/*
 * proto.h - 공유 캔버스 서버 메시지 형식 (세 IPC 버전 공통)
 *
 * 11/18 확정 (#4). 바꿀 때는 owner 가 승인한 PR 로만 바꾼다 (docs/CONTRIBUTING.md 의 PR 절).
 * 모든 메시지는 msg_hdr_t 로 시작하고, 그 뒤에 payload_len 바이트가 붙는다.
 * 응답 payload 는 항상 srv_ts_t 로 시작한다. SNAPSHOT 응답은 그 뒤에 영역의 w × h 바이트가 붙는다.
 * 타일(락 단위)은 서버 내부의 실행 옵션이라 여기 두지 않는다 (include/canvas.h).
 */
#ifndef PROTO_H
#define PROTO_H

#include <stdint.h>

#define CANVAS_W   1024u            /* 가로 칸 수 */
#define CANVAS_H   1024u            /* 세로 칸 수, 한 칸 1바이트 → 전체 1MB */

#define MAX_CLIENTS 1024u           /* 동시에 붙을 수 있는 클라이언트 번호 0 .. MAX_CLIENTS-1 (11/24 16 → 1024) */
#define MAX_ROOMS   64u             /* 서버가 둘 수 있는 그림판(방) 수. 서버 -K 로 정한다 (11/24 추가) */

enum msg_type {
    REQ_HELLO    = 1,   /* 클라이언트 등록. 클라이언트의 첫 요청이다 (transport.h 의 약속) */
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
    uint64_t req_id;        /* 클라이언트가 붙이는 일련번호, 응답에 그대로 돌아옴. 시작값은 세션마다 다르게 */
    uint32_t payload_len;   /* 머리 뒤에 붙는 바이트 수. MAX_PAYLOAD 이하 */
    uint32_t reserved;      /* 요청: 그림판(방) 번호 0 .. 방 수-1. 방 하나면 0. 응답: 0 */
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

/*
 * payload 의 최대 크기 = 가장 큰 응답 = 캔버스 전체 SNAPSHOT (서버 시각 32B + 1MB = 1048608B).
 * 1단계 echo 벤치마크의 메시지도 이 크기까지다. 세 전송 계층은 이 크기를 모두 보내고 받는다.
 */
#define MAX_PAYLOAD ((uint32_t)(sizeof(srv_ts_t) + CANVAS_W * CANVAS_H))

/* 컴파일할 때 확인하는 약속. 어기면 빌드가 실패한다 */
_Static_assert(sizeof(msg_hdr_t) == 24, "msg_hdr_t 는 24B 다. 전송 계층과 측정이 이 크기를 가정한다");
_Static_assert(sizeof(srv_ts_t) == 32, "srv_ts_t 는 32B 다");
_Static_assert(sizeof(draw_req_t) == 16 && sizeof(snap_req_t) == 8, "요청 payload 크기");
_Static_assert(sizeof(msg_hdr_t) + sizeof(draw_req_t) <= 512,
               "응용의 요청은 POSIX 가 보장하는 최소 PIPE_BUF(512B) 이하다. 그래서 FIFO 에 write 한 번으로 섞이지 않고 써진다");
_Static_assert(CANVAS_W <= 65535 && CANVAS_H <= 65535, "좌표와 크기는 uint16_t 에 담긴다");
_Static_assert(CANVAS_W * CANVAS_H == 1048576u, "캔버스는 1MB 다 (요구사항 F-1)");

#endif /* PROTO_H */
