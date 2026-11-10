/*
 * transport.h - 전송 계층 인터페이스 (IPC 버전마다 하나씩 구현)
 *
 * 초안입니다. 11/13까지 팀이 확정합니다.
 *
 * 서버, 클라이언트, 1단계 벤치마크는 이 함수들만 부릅니다.
 * 어떤 IPC인지는 링크할 때 정해집니다.
 *   src/transport/transport_pipe.c  (FIFO)
 *   src/transport/transport_mq.c    (System V message queue)
 *   src/transport/transport_shm.c   (System V shared memory + semaphore)
 *
 * 약속
 *  - 메시지 하나 = msg_hdr_t + payload. 받는 쪽은 항상 메시지 하나를 통째로 받는다.
 *    (조각내어 보내고 다시 붙이는 일은 구현 안에서 처리한다.)
 *  - tp_send_resp 는 여러 워커 스레드가 동시에 불러도 안전해야 한다.
 *    같은 클라이언트로 가는 두 응답이 섞이면 안 된다.
 *  - 모든 함수는 성공 0, 실패 -1 (errno 설정). tp_recv_* 는 받은 payload 길이를 돌려준다.
 *  - 서버가 만든 IPC 자원은 tp_server_close 에서 모두 지운다 (ipcs 에 남으면 안 됨).
 */
#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <stddef.h>
#include <sys/types.h>
#include "proto.h"

/* System V key 규칙: 60xxy (xx = 팀 번호, y = 0~9). 2팀이므로 60020 ~ 60029. */
#ifndef TEAM_NO
#define TEAM_NO 2
#endif
#define IPC_KEY(y) ((key_t)(60000 + TEAM_NO * 10 + (y)))

const char *tp_name(void);              /* "pipe", "mq", "shm" */

/* ---- 서버 쪽 ---- */
int     tp_server_open(void);                                   /* 요청 채널 만들기 */
ssize_t tp_recv_req(msg_hdr_t *hdr, void *payload, size_t cap); /* 요청 1개 받기 (막힘) */
int     tp_send_resp(const msg_hdr_t *hdr, const void *payload);/* hdr->client_id 에게 응답 */
void    tp_server_wake(void);                                   /* 막혀 있는 tp_recv_req 깨우기 (종료용) */
void    tp_server_close(void);                                  /* 자원 모두 삭제 */

/* ---- 클라이언트 쪽 ---- */
int     tp_client_open(uint32_t client_id);                     /* 요청 채널 연결 + 내 응답 채널 만들기 */
int     tp_send_req(const msg_hdr_t *hdr, const void *payload);
ssize_t tp_recv_resp(msg_hdr_t *hdr, void *payload, size_t cap);/* 응답 1개 받기 (막힘) */
void    tp_client_close(void);

#endif /* TRANSPORT_H */
