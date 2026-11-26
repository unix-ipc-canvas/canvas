/*
 * transport.h - 전송 계층 인터페이스 (IPC 버전마다 하나씩 구현)
 *
 * 11/18 확정 (#4). 바꿀 때는 owner 가 승인한 PR 로만 바꾼다 (docs/CONTRIBUTING.md 의 PR 절).
 *
 * 서버, 클라이언트, 1단계 벤치마크는 이 함수들만 부른다. 어떤 IPC 인지는 링크할 때 정해진다.
 *   src/transport/transport_pipe.c   FIFO (이름 있는 pipe)
 *   src/transport/transport_mq.c     System V message queue
 *   src/transport/transport_shm.c    System V shared memory + semaphore
 *   src/transport/transport_upipe.c  익명 pipe. 1단계 벤치마크 전용 (fork 로 물려받아야 이어진다)
 *
 * ==== 약속 (구현과 부르는 쪽이 모두 지킨다) ====
 *
 * 메시지
 *  - 메시지 하나 = msg_hdr_t + payload_len 바이트, payload_len <= MAX_PAYLOAD.
 *    받는 쪽은 항상 메시지 하나를 통째로 받는다. 조각내어 보내고 다시 붙이는 일은 구현 안에서 한다.
 *  - tp_recv_* 의 cap 보다 큰 메시지는 버리고 -1/EMSGSIZE 를 돌려준다. 그다음 메시지는 정상으로 받는다.
 *
 * 스레드
 *  - tp_recv_req, tp_recv_resp 는 각각 한 스레드만 부른다 (서버 수신 스레드, 클라이언트 수신 스레드).
 *  - tp_send_resp 는 여러 워커가 동시에 불러도 된다. 같은 클라이언트로 가는 두 응답이 섞이지 않는다.
 *  - tp_send_req 도 여러 스레드가 동시에 불러도 된다.
 *  - tp_server_close, tp_client_close 는 송수신하던 스레드가 모두 빠져나온 뒤에 부른다.
 *    서버는 tp_server_wake 로 수신 스레드를 깨워 끝낸 뒤, 클라이언트는 띄워 둔 요청의 응답을 다 받은 뒤 부른다.
 *
 * 순서
 *  - 클라이언트의 첫 요청은 REQ_HELLO 다. 응답을 하나라도 받기 전에 보낸다.
 *    (pipe 버전 서버는 HELLO 를 받으면 그 번호의 응답 FIFO 를 다시 연다.)
 *  - 한 실행 안에서 PIPE_BUF(4096B) 이하인 요청과 넘는 요청을 섞지 않는다. pipe 버전은 작은 요청을 잠금 없이
 *    write 한 번으로 보내므로, 섞으면 작은 요청이 다른 클라이언트의 큰 요청 사이에 끼어든다.
 *    응용의 요청은 모두 작다. 1단계 벤치마크는 HELLO 도 측정 메시지와 같은 크기로 보낸다.
 *  - 클라이언트는 어떤 경로로 끝나든(정상, 오류, SIGINT·SIGTERM) tp_client_close 를 부른다.
 *    부르지 않으면 pipe 버전은 /tmp 에 응답 FIFO 가 남고, mq 버전은 서버가 죽었을 때 남은 큐를 지우던 정리가 중간에 끊긴다.
 *  - 같은 번호로 다시 접속하면, 예전 세션이 보내 놓고 서버가 처리하던 요청의 응답이 새 세션에 올 수 있다.
 *    클라이언트는 req_id 시작값을 세션마다 다르게 하고(예: (uint64_t)getpid() << 32), 띄워 두지 않은 req_id 의 응답은 버린다.
 *
 * 실패할 때 (-1 과 errno)
 *   tp_server_open   EADDRINUSE    서버가 이미 떠 있다
 *   tp_recv_req      ECANCELED     tp_server_wake 로 깨웠거나 닫는 중이다. 수신 스레드를 끝낸다
 *                    EMSGSIZE      cap 보다 큰 요청을 버렸다. 계속 받는다
 *   tp_send_resp     EPIPE         그 클라이언트가 끝났다. 그 응답만 버리고 계속한다
 *   tp_client_open   ECONNREFUSED  서버가 없다
 *                    EADDRINUSE    같은 번호의 클라이언트가 실행 중이다
 *   tp_send_req      EPIPE         서버가 끝났다
 *   tp_recv_resp     ECONNRESET    서버가 끝났다
 *                    EMSGSIZE      cap 보다 큰 응답을 버렸다
 *   번호가 MAX_CLIENTS 이상이면 EINVAL. 그 밖의 errno 는 시스템 콜이 낸 값 그대로다.
 *   성공하면 0, tp_recv_* 는 받은 payload 길이를 돌려준다.
 *
 * 자원
 *  - 서버가 만든 IPC 자원과 파일은 tp_server_close 에서 모두 지운다 (ipcs 와 /tmp 에 남으면 안 된다).
 *  - 비정상 종료로 남은 것은 다음에 같은 자원을 만드는 쪽(서버, 같은 번호의 클라이언트)이 지우거나 make ipcclean 으로 지운다.
 *    실행 중인 상대의 자원은 지우지 않는다.
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

const char *tp_name(void);              /* "pipe", "mq", "shm", "upipe" */

/* ---- 서버 쪽 ---- */
int     tp_server_open(void);                                   /* 요청 채널 만들기 */
ssize_t tp_recv_req(msg_hdr_t *hdr, void *payload, size_t cap); /* 요청 1개 받기 (막힘) */
int     tp_send_resp(const msg_hdr_t *hdr, const void *payload);/* hdr->client_id 에게 응답 */
void    tp_server_wake(void);                                   /* 막혀 있는 tp_recv_req 깨우기 (종료용). 그 뒤의 tp_recv_req 는 모두 ECANCELED */
void    tp_server_close(void);                                  /* 자원 모두 삭제 */
/*
 * 클라이언트 id 가 지금 살아 있는지. 1 살아 있음, 0 끝났음(kill -9 로 죽은 것 포함), -1 알 수 없음.
 * 막히지 않는다. 서버가 STATS 의 접속 수를 셀 때 쓴다 (BYE 없이 죽은 클라이언트를 빼려고)
 */
int     tp_client_alive(uint32_t id);

/* ---- 클라이언트 쪽 ---- */
int     tp_client_open(uint32_t client_id);                     /* 요청 채널 연결 + 내 응답 채널 만들기 */
int     tp_send_req(const msg_hdr_t *hdr, const void *payload);
ssize_t tp_recv_resp(msg_hdr_t *hdr, void *payload, size_t cap);/* 응답 1개 받기 (막힘) */
void    tp_client_close(void);

/*
 * ---- 서버와 클라이언트를 한 프로세스가 fork 로 띄울 때 (1단계 벤치마크) ----
 * 이름 없이 부모에게서 물려받아야 하는 채널(익명 pipe)을 위한 자리다. 이름이나 key 로 만나는 버전은
 * 아무것도 하지 않고 0 을 돌려준다. fork 로 띄우는 프로그램은 버전과 관계없이 둘 다 부른다.
 *   tp_prefork   공통 조상이 서버와 클라이언트를 fork 하기 전에 한 번 부른다. 채널을 미리 만든다
 *   tp_postfork  공통 조상이 모두 fork 한 뒤 부른다. 조상이 쥔 채널 끝을 닫아, 상대가 끝나면 EOF·EPIPE 가 나게 한다
 * 자식은 fork 뒤에 평소처럼 tp_server_open 또는 tp_client_open 을 부른다.
 */
int     tp_prefork(void);
void    tp_postfork(void);

#endif /* TRANSPORT_H */
