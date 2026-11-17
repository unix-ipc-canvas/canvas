/*
 * transport_upipe.c - 익명 pipe 로 구현한 전송 계층 (1단계 벤치마크 전용)
 *
 * 강의 5장의 pipe 두 가지 가운데 pipe() 로 만드는 익명 pipe 다. 이름이 없어서 공통 조상이 fork 하기 전에
 * 만들어 물려줘야 한다. 그래서 서버와 클라이언트를 터미널마다 따로 띄우는 2단계 응용에는 쓸 수 없고,
 * 조정 프로세스가 서버와 클라이언트를 fork 하는 1단계 벤치마크에서만 쓴다 (make 는 bench_upipe 만 만든다).
 *
 * 채널은 FIFO 버전(transport_pipe.c)과 같게 둔다. 두 버전의 차이가 "이름이 있는가" 하나뿐이게 하려는 것이다.
 *   요청 채널  pipe 1개                         모든 클라이언트 → 서버 (N:1)
 *   응답 채널  pipe MAX_CLIENTS 개                서버 → 클라이언트 id (클라이언트마다 1개)
 *   송신 잠금  이름을 지운 임시 파일 1개            PIPE_BUF 를 넘는 요청을 보낼 때만 거는 fcntl 잠금
 *
 * 누가 어느 끝을 쥐는가 (transport.h 의 tp_prefork, tp_postfork)
 *   조상        tp_prefork 로 모두 만든다 → 서버와 클라이언트를 fork → tp_postfork 로 자기가 쥔 끝을 모두 닫는다
 *   서버        요청 pipe 의 읽기 끝, 쓰기 끝(tp_server_wake 용), 모든 응답 pipe 의 쓰기 끝
 *   클라이언트 i 요청 pipe 의 쓰기 끝, 응답 pipe i 의 읽기 끝
 *   쓰지 않는 끝을 모두 닫아야 상대가 끝났을 때 read 가 EOF, write 가 EPIPE 를 받는다.
 *   서버는 tp_server_open, 클라이언트는 tp_client_open 에서 자기 몫이 아닌 끝을 닫는다.
 *
 * 섞임 방지는 FIFO 버전과 같다
 *   - PIPE_BUF 이하 요청은 write 한 번으로 보낸다 (커널이 원자성을 보장, 잠금 없음).
 *   - PIPE_BUF 를 넘는 요청은 송신 잠금을 걸고 보낸다.
 *   - 응답은 클라이언트별 mutex 를 잡고 보낸다.
 *
 * DECISION: 송신 잠금 파일은 mkstemp 로 /tmp/osproject_<팀>_upipe.XXXXXX 를 만들자마자 지운다. 이름이 없어 남을 일이
 *   없다. fcntl 잠금은 프로세스마다 따로 걸리므로 같은 열린 파일을 물려받은 프로세스끼리도 서로 막는다.
 * DECISION: 입출력 도우미는 transport_pipe.c 와 같은 코드를 이 파일에 따로 둔다. pipe 담당의 파일을 고치지 않고
 *   두 버전이 각자 완결되게 하려는 것이다.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include "transport.h"

#define WAKE_TYPE 0u          /* tp_server_wake 가 보내는 머리의 type. 실제 메시지는 1 이상이다 */
#define DRAIN_BUF 4096

/* ---- tp_prefork 가 만든 채널. fork 로 물려받는다 ---- */

static int up_req[2] = { -1, -1 };          /* [0] 읽기 끝, [1] 쓰기 끝 */
static int up_resp[MAX_CLIENTS][2];         /* tp_prefork 가 -1 로 채운 뒤 만든다 */
static int up_lock = -1;
static bool up_made;                        /* tp_prefork 를 불렀고 아직 닫지 않았다 */

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void close_all(void)
{
    close_fd(&up_req[0]);
    close_fd(&up_req[1]);
    for (uint32_t i = 0; i < MAX_CLIENTS; i++) {
        close_fd(&up_resp[i][0]);
        close_fd(&up_resp[i][1]);
    }
    close_fd(&up_lock);
    up_made = false;
}

/* ---- 입출력 도우미 (transport_pipe.c 와 같다) ---- */

/* n 바이트를 끝까지 읽는다. 성공 0, 쓰는 쪽이 모두 닫혀 EOF 이면 -1 (errno = ECONNRESET) */
static int read_full(int fd, void *buf, size_t n)
{
    char *p = buf;
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r > 0) { p += r; n -= (size_t)r; continue; }
        if (r == 0) { errno = ECONNRESET; return -1; }
        if (errno == EINTR) continue;
        return -1;
    }
    return 0;
}

/* n 바이트를 읽어서 버린다. 받을 버퍼보다 큰 메시지가 왔을 때 스트림 위치를 맞춘다 */
static int drain(int fd, size_t n)
{
    char tmp[DRAIN_BUF];
    while (n > 0) {
        size_t k = n < sizeof tmp ? n : sizeof tmp;
        if (read_full(fd, tmp, k) < 0) return -1;
        n -= k;
    }
    return 0;
}

/* iov 전체를 끝까지 쓴다. *done 에 실제로 쓴 바이트 수를 남긴다. 성공 0, 실패 -1 */
static int writev_full(int fd, struct iovec *iov, int cnt, size_t *done)
{
    *done = 0;
    while (cnt > 0) {
        ssize_t w = writev(fd, iov, cnt);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        *done += (size_t)w;
        size_t left = (size_t)w;
        while (cnt > 0 && left >= iov->iov_len) {
            left -= iov->iov_len;
            iov++; cnt--;
        }
        if (cnt > 0) {
            iov->iov_base = (char *)iov->iov_base + left;
            iov->iov_len -= left;
        }
    }
    return 0;
}

/* 끊긴 상대에게 쓰면 SIGPIPE 로 프로세스가 죽는다. 무시하고 write 의 EPIPE 로 처리한다 (FIFO 버전과 같다) */
static void ignore_sigpipe(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPIPE, &sa, NULL);
}

/* PIPE_BUF 를 넘는 요청에 거는 송신 잠금. 프로세스 사이는 fcntl, 같은 프로세스의 스레드 사이는 mutex */
static pthread_mutex_t big_mtx = PTHREAD_MUTEX_INITIALIZER;

static int big_lock(int lock_fd)
{
    pthread_mutex_lock(&big_mtx);
    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };
    while (fcntl(lock_fd, F_SETLKW, &fl) < 0) {
        if (errno == EINTR) continue;
        int e = errno;
        pthread_mutex_unlock(&big_mtx);
        errno = e;
        return -1;
    }
    return 0;
}

static void big_unlock(int lock_fd)
{
    struct flock fl = { .l_type = F_UNLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };
    fcntl(lock_fd, F_SETLK, &fl);
    pthread_mutex_unlock(&big_mtx);
}

/* 메시지 하나를 요청 채널에 쓴다. PIPE_BUF 이하이면 잠금 없이 write 한 번, 넘으면 송신 잠금을 걸고 쓴다 */
static int send_on_req(int req_fd, int lock_fd, const msg_hdr_t *hdr, const void *payload)
{
    if (hdr->payload_len > MAX_PAYLOAD) { errno = EMSGSIZE; return -1; }
    size_t total = sizeof *hdr + hdr->payload_len;
    size_t done;

    if (total <= PIPE_BUF) {
        char buf[PIPE_BUF];
        memcpy(buf, hdr, sizeof *hdr);
        if (hdr->payload_len) memcpy(buf + sizeof *hdr, payload, hdr->payload_len);
        struct iovec iov = { buf, total };
        return writev_full(req_fd, &iov, 1, &done);
    }

    if (big_lock(lock_fd) < 0) return -1;
    struct iovec iov[2] = {
        { (void *)hdr, sizeof *hdr },
        { (void *)payload, hdr->payload_len },
    };
    int rc = writev_full(req_fd, iov, 2, &done);
    int e = errno;
    big_unlock(lock_fd);
    errno = e;
    return rc;
}

/* 머리와 payload 를 받는다. cap 보다 큰 payload 는 읽어서 버리고 EMSGSIZE 로 실패한다 */
static ssize_t recv_msg(int fd, msg_hdr_t *hdr, void *payload, size_t cap)
{
    if (read_full(fd, hdr, sizeof *hdr) < 0) return -1;
    if (hdr->payload_len > cap) {
        if (drain(fd, hdr->payload_len) < 0) return -1;
        errno = EMSGSIZE;
        return -1;
    }
    if (hdr->payload_len && read_full(fd, payload, hdr->payload_len) < 0) return -1;
    return (ssize_t)hdr->payload_len;
}

const char *tp_name(void) { return "upipe"; }

/* ================================================================
 * fork 전후 (공통 조상)
 * ================================================================ */

/* 요청 pipe, 응답 pipe MAX_CLIENTS 개, 송신 잠금 파일을 만든다. 서버·클라이언트를 fork 하기 전에 한 번 부른다 */
int tp_prefork(void)
{
    if (up_made) { errno = EALREADY; return -1; }
    up_req[0] = up_req[1] = -1;
    for (uint32_t i = 0; i < MAX_CLIENTS; i++) up_resp[i][0] = up_resp[i][1] = -1;

    if (pipe(up_req) < 0) goto fail;
    for (uint32_t i = 0; i < MAX_CLIENTS; i++)
        if (pipe(up_resp[i]) < 0) goto fail;

    char path[64];
    snprintf(path, sizeof path, "/tmp/osproject_%d_upipe.XXXXXX", TEAM_NO);
    up_lock = mkstemp(path);
    if (up_lock < 0) goto fail;
    unlink(path);                        /* 이름을 바로 지운다. 물려받은 fd 로만 쓴다 */

    up_made = true;
    return 0;

fail:;
    int e = errno;
    close_all();
    errno = e;
    return -1;
}

/* 모두 fork 한 뒤 조상이 부른다. 조상이 쥔 끝을 닫아야 상대가 끝났을 때 EOF·EPIPE 가 난다 */
void tp_postfork(void)
{
    close_all();
}

/* ================================================================
 * 서버 쪽
 * ================================================================ */

static pthread_mutex_t srv_resp_mtx[MAX_CLIENTS];
static atomic_bool srv_stopping;
static int srv_opened;

/* tp_prefork 를 부른 조상에게서 fork 된 서버만 연다. 그렇지 않으면 -1, errno = ENOTCONN */
int tp_server_open(void)
{
    if (!up_made) { errno = ENOTCONN; return -1; }
    ignore_sigpipe();

    /* 응답 pipe 의 읽기 끝은 클라이언트 몫이다. 서버가 쥐고 있으면 클라이언트가 끝나도 EPIPE 가 나지 않는다 */
    for (uint32_t i = 0; i < MAX_CLIENTS; i++) {
        close_fd(&up_resp[i][0]);
        pthread_mutex_init(&srv_resp_mtx[i], NULL);
    }
    atomic_store(&srv_stopping, false);
    srv_opened = 1;
    return 0;
}

/* 요청 1개 받기. 수신 스레드 하나만 부른다. tp_server_wake 로 깨워지면 -1, errno = ECANCELED */
ssize_t tp_recv_req(msg_hdr_t *hdr, void *payload, size_t cap)
{
    if (atomic_load(&srv_stopping)) { errno = ECANCELED; return -1; }
    ssize_t n = recv_msg(up_req[0], hdr, payload, cap);
    if (n < 0) return -1;
    if (hdr->type == WAKE_TYPE) { errno = ECANCELED; return -1; }
    return n;
}

int tp_send_resp(const msg_hdr_t *hdr, const void *payload)
{
    uint32_t id = hdr->client_id;
    if (id >= MAX_CLIENTS) { errno = EINVAL; return -1; }

    int rc = -1;
    pthread_mutex_lock(&srv_resp_mtx[id]);
    if (up_resp[id][1] < 0) {
        errno = EPIPE;                    /* 이미 끊겨 닫았다 */
    } else {
        struct iovec iov[2] = {
            { (void *)hdr, sizeof *hdr },
            { (void *)payload, hdr->payload_len },
        };
        size_t done;
        rc = writev_full(up_resp[id][1], iov, hdr->payload_len ? 2 : 1, &done);
        if (rc < 0) {
            /* 중간까지 쓴 스트림은 이어 쓸 수 없고, 익명 pipe 는 다시 열 수도 없다. 닫고 그 뒤로는 EPIPE */
            int e = errno;
            close_fd(&up_resp[id][1]);
            errno = e;
        }
    }
    pthread_mutex_unlock(&srv_resp_mtx[id]);
    return rc;
}

/* 막혀 있는 tp_recv_req 를 깨운다. 서버가 쥔 요청 pipe 의 쓰기 끝으로 type 0 머리 하나를 쓴다 */
void tp_server_wake(void)
{
    if (!srv_opened) return;
    atomic_store(&srv_stopping, true);
    msg_hdr_t h;
    memset(&h, 0, sizeof h);
    h.type = WAKE_TYPE;
    send_on_req(up_req[1], up_lock, &h, NULL);
}

/* 서버의 끝을 모두 닫는다. 응답 pipe 의 쓰기 끝이 닫히므로 클라이언트의 수신은 EOF(ECONNRESET)로 끝난다 */
void tp_server_close(void)
{
    if (!srv_opened) return;
    for (uint32_t i = 0; i < MAX_CLIENTS; i++) {
        pthread_mutex_lock(&srv_resp_mtx[i]);
        close_fd(&up_resp[i][1]);
        pthread_mutex_unlock(&srv_resp_mtx[i]);
        pthread_mutex_destroy(&srv_resp_mtx[i]);
    }
    close_all();
    srv_opened = 0;
}

/* ================================================================
 * 클라이언트 쪽
 * ================================================================ */

static int cli_req_fd = -1;          /* 요청 pipe 쓰기 끝 */
static int cli_resp_fd = -1;         /* 내 응답 pipe 읽기 끝 */
static int cli_lock_fd = -1;
static int cli_opened;

/*
 * 내 몫의 끝만 남기고 나머지를 닫는다. tp_prefork 를 부른 조상에게서 fork 되지 않았으면 -1, errno = ENOTCONN.
 * 익명 pipe 는 번호마다 한 번만 쓸 수 있다. 같은 프로세스에서 다시 열 수 없다.
 */
int tp_client_open(uint32_t client_id)
{
    if (client_id >= MAX_CLIENTS) { errno = EINVAL; return -1; }
    if (!up_made || up_resp[client_id][0] < 0) { errno = ENOTCONN; return -1; }
    ignore_sigpipe();

    close_fd(&up_req[0]);                /* 요청 pipe 의 읽기 끝: 서버 몫 */
    for (uint32_t i = 0; i < MAX_CLIENTS; i++) {
        close_fd(&up_resp[i][1]);        /* 응답 pipe 의 쓰기 끝: 서버 몫 */
        if (i != client_id) close_fd(&up_resp[i][0]);
    }
    cli_req_fd = up_req[1];
    cli_resp_fd = up_resp[client_id][0];
    cli_lock_fd = up_lock;
    up_req[1] = up_resp[client_id][0] = up_lock = -1;   /* 이제 cli_* 가 쥔다 */
    up_made = false;
    cli_opened = 1;
    return 0;
}

/* 서버가 끝났으면 읽는 쪽이 없어 EPIPE 로 실패한다 (SIGPIPE 는 무시함) */
int tp_send_req(const msg_hdr_t *hdr, const void *payload)
{
    return send_on_req(cli_req_fd, cli_lock_fd, hdr, payload);
}

/* 응답 1개 받기. 수신 스레드 하나만 부른다. 서버가 끝나면 -1, errno = ECONNRESET */
ssize_t tp_recv_resp(msg_hdr_t *hdr, void *payload, size_t cap)
{
    return recv_msg(cli_resp_fd, hdr, payload, cap);
}

void tp_client_close(void)
{
    if (!cli_opened) return;
    close_fd(&cli_req_fd);
    close_fd(&cli_resp_fd);
    close_fd(&cli_lock_fd);
    cli_opened = 0;
}
