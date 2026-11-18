/*
 * transport_pipe.c - FIFO(named pipe)로 구현한 전송 계층
 *
 * 채널 (docs/design/README.md#채널)
 *   요청 채널  /tmp/osproject_<팀>_req          모든 클라이언트 → 서버 (N:1)
 *   응답 채널  /tmp/osproject_<팀>_resp_<id>    서버 → 클라이언트 id (클라이언트마다 1개)
 *   잠금 파일  /tmp/osproject_<팀>_req.lock     PIPE_BUF 를 넘는 요청을 보낼 때만 쓰는 송신 잠금
 *
 * 메시지 하나 = msg_hdr_t + payload_len 바이트. 받는 쪽은 머리를 먼저 읽고 payload 를 끝까지 읽는다.
 *
 * 섞임 방지
 *   - 요청이 PIPE_BUF(4096B) 이하이면 write 한 번으로 보낸다. 커널이 원자성을 보장하므로 잠금이 없다.
 *   - 요청이 PIPE_BUF 를 넘으면(1단계 벤치마크의 큰 메시지) 잠금 파일에 fcntl 쓰기 잠금을 걸고 보낸다.
 *   - 응답은 클라이언트별 mutex 를 잡고 보낸다. 큰 응답이 여러 번의 write 로 나뉘어도 섞이지 않는다.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <limits.h>

#include "transport.h"

#define PATH_LEN 64
#define WAKE_TYPE 0u          /* tp_server_wake 가 보내는 머리의 type. 실제 메시지는 1 이상이다 */
#define DRAIN_BUF 4096

/* ---- 경로 ---- */

static void req_path(char *buf)  { snprintf(buf, PATH_LEN, "/tmp/osproject_%d_req", TEAM_NO); }
static void lock_path(char *buf) { snprintf(buf, PATH_LEN, "/tmp/osproject_%d_req.lock", TEAM_NO); }
static void resp_path(char *buf, uint32_t id)
{
    snprintf(buf, PATH_LEN, "/tmp/osproject_%d_resp_%u", TEAM_NO, id);
}

/* ---- 공통 입출력 도우미 ---- */

/* n 바이트를 끝까지 읽는다. 성공 0, 상대가 모두 닫혀 EOF 이면 -1 (errno = ECONNRESET) */
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

/* n 바이트를 읽어서 버린다. 받을 버퍼보다 큰 메시지가 왔을 때 스트림 위치를 맞추는 데 쓴다 */
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

static int clear_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
}

/*
 * 이미 있는 FIFO 를 누군가 읽고 있는지 본다.
 * 1 = 읽는 쪽이 있음(사용 중), 0 = 없음(비정상 종료가 남긴 파일), -1 = 확인 실패
 */
static int fifo_in_use(const char *path)
{
    int fd = open(path, O_WRONLY | O_NONBLOCK);
    if (fd >= 0) { close(fd); return 1; }
    if (errno == ENXIO) return 0;
    return -1;
}

/*
 * FIFO 를 만든다. 같은 이름이 있으면 읽는 쪽이 있는지 보고, 없으면 남은 파일로 보고 지운 뒤 다시 만든다.
 * 누가 쓰고 있으면 지우지 않고 EADDRINUSE 로 실패한다 (남의 자원을 지우지 않는다).
 */
static int make_fifo(const char *path)
{
    if (mkfifo(path, 0600) == 0) return 0;
    if (errno != EEXIST) return -1;

    struct stat st;
    if (stat(path, &st) < 0) return -1;
    if (!S_ISFIFO(st.st_mode)) { errno = EEXIST; return -1; }

    int used = fifo_in_use(path);
    if (used < 0) return -1;
    if (used == 1) { errno = EADDRINUSE; return -1; }
    if (unlink(path) < 0 && errno != ENOENT) return -1;
    return mkfifo(path, 0600);
}

/*
 * 끊긴 상대에게 쓰면 SIGPIPE 로 프로세스가 죽는다. 무시하고 write 의 EPIPE 로 처리한다.
 * DECISION: 프로세스 전체의 SIGPIPE 를 무시한다. 서버·클라이언트·벤치마크 모두 이 동작을 원한다.
 */
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

/*
 * 메시지 하나를 요청 채널에 쓴다.
 * DECISION: PIPE_BUF 이하 요청은 잠금 없이 write 한 번으로 보낸다. 작은 메시지 측정에 잠금 비용이 섞이지 않게 하려는 것이다.
 *   그 대신 큰 요청과 작은 요청을 여러 클라이언트가 동시에 섞어 보내면 작은 요청이 큰 요청 사이에 끼어들 수 있다.
 *   응용의 요청은 모두 PIPE_BUF 이하이고, 1단계 벤치마크는 한 실행에서 크기가 같으므로 이 경우가 생기지 않는다.
 */
static int send_on_req(int req_fd, int lock_fd, const msg_hdr_t *hdr, const void *payload)
{
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

const char *tp_name(void) { return "pipe"; }

/* fork 로 함께 띄울 때의 준비. FIFO 는 경로 이름으로 만나므로 할 일이 없다 */
int  tp_prefork(void)  { return 0; }
void tp_postfork(void) { }

/* ================================================================
 * 서버 쪽
 * ================================================================ */

static int srv_req_fd = -1;                      /* 요청 FIFO, O_RDWR */
static int srv_wake_fd = -1;                     /* 요청 FIFO 쓰기 끝, O_NONBLOCK. tp_server_wake 만 쓴다 */
static int srv_lock_fd = -1;
static int srv_resp_fd[MAX_CLIENTS];             /* 응답 FIFO 쓰기 끝. 처음 보낼 때 연다 */
static pthread_mutex_t srv_resp_mtx[MAX_CLIENTS];
static atomic_bool srv_stopping;
static int srv_opened;

int tp_server_open(void)
{
    char rp[PATH_LEN], lp[PATH_LEN];
    req_path(rp);
    lock_path(lp);

    ignore_sigpipe();
    if (make_fifo(rp) < 0) return -1;           /* 서버가 이미 떠 있으면 EADDRINUSE */

    /*
     * O_RDWR 로 연다. 서버 자신이 쓰는 쪽도 되므로
     *   - 클라이언트가 아직 없어도 open 이 막히지 않고
     *   - 클라이언트가 모두 나가도 read 가 EOF(0)를 돌려주지 않으며
     *   - tp_server_wake 가 이 fd 로 자기 자신에게 깨우기 메시지를 쓸 수 있다.
     * DECISION: FIFO 를 O_RDWR 로 여는 것은 POSIX 에서 정의되지 않았지만 Linux 에서는 위와 같이 동작한다.
     */
    srv_req_fd = open(rp, O_RDWR);
    if (srv_req_fd < 0) goto fail;

    /* 깨우기 전용 쓰기 끝. 막히지 않게 따로 연다 (O_NONBLOCK 은 열린 파일마다라 srv_req_fd 의 읽기에는 영향이 없다) */
    srv_wake_fd = open(rp, O_WRONLY | O_NONBLOCK);
    if (srv_wake_fd < 0) goto fail;

    srv_lock_fd = open(lp, O_RDWR | O_CREAT, 0600);
    if (srv_lock_fd < 0) goto fail;

    for (uint32_t i = 0; i < MAX_CLIENTS; i++) {
        srv_resp_fd[i] = -1;
        pthread_mutex_init(&srv_resp_mtx[i], NULL);
    }
    atomic_store(&srv_stopping, false);
    srv_opened = 1;
    return 0;

fail:;
    int e = errno;
    if (srv_req_fd >= 0) { close(srv_req_fd); srv_req_fd = -1; }
    if (srv_wake_fd >= 0) { close(srv_wake_fd); srv_wake_fd = -1; }
    unlink(rp);
    unlink(lp);
    errno = e;
    return -1;
}

static int open_resp(uint32_t id);

/* 클라이언트 id 의 응답 FIFO 쓰기 끝을 닫는다. 호출하는 쪽이 srv_resp_mtx[id] 를 잡고 있어야 한다 */
static void drop_resp_locked(uint32_t id)
{
    if (srv_resp_fd[id] >= 0) {
        close(srv_resp_fd[id]);
        srv_resp_fd[id] = -1;
    }
}

/*
 * 요청 1개 받기. 수신 스레드 하나만 부른다.
 * tp_server_wake 로 깨워지면 -1, errno = ECANCELED.
 */
ssize_t tp_recv_req(msg_hdr_t *hdr, void *payload, size_t cap)
{
    if (atomic_load(&srv_stopping)) { errno = ECANCELED; return -1; }

    ssize_t n = recv_msg(srv_req_fd, hdr, payload, cap);
    if (n < 0) return -1;

    if (hdr->type == WAKE_TYPE) { errno = ECANCELED; return -1; }

    /*
     * 같은 번호로 새 클라이언트가 들어오면 응답 FIFO 의 쓰기 끝을 새 FIFO 로 바꾼다.
     * 새 쓰기 끝을 먼저 연 뒤 예전 것을 닫는다. 순서가 반대면 쓰는 쪽이 0개가 되는 순간이 생긴다:
     *   예전 세션의 응답이 tp_send_resp 의 재시도로 HELLO 보다 먼저 새 FIFO 에 들어가면, 클라이언트는 그 응답을
     *   받으면서 keeper 를 닫는다. 그때 서버 쪽 쓰기 끝까지 닫으면 클라이언트 수신이 EOF(ECONNRESET)를 받는다 (#36).
     * 새 FIFO 를 열지 못하면(클라이언트가 이미 끝남) 예전 것만 닫고, 다음 응답 때 다시 열어 본다.
     */
    if (hdr->type == REQ_HELLO && hdr->client_id < MAX_CLIENTS) {
        uint32_t id = hdr->client_id;
        pthread_mutex_lock(&srv_resp_mtx[id]);
        int fresh = open_resp(id);
        drop_resp_locked(id);
        srv_resp_fd[id] = fresh;
        pthread_mutex_unlock(&srv_resp_mtx[id]);
    }
    return n;
}

/* 응답 FIFO 를 쓰기로 연다. 클라이언트가 읽고 있지 않으면 막히지 않고 EPIPE 로 실패한다 */
static int open_resp(uint32_t id)
{
    char path[PATH_LEN];
    resp_path(path, id);
    int fd = open(path, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENXIO || errno == ENOENT) errno = EPIPE;
        return -1;
    }
    if (clear_nonblock(fd) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

int tp_send_resp(const msg_hdr_t *hdr, const void *payload)
{
    uint32_t id = hdr->client_id;
    if (id >= MAX_CLIENTS) { errno = EINVAL; return -1; }

    int rc = -1;
    pthread_mutex_lock(&srv_resp_mtx[id]);

    /*
     * 예전 클라이언트가 나간 뒤 같은 번호의 새 클라이언트가 FIFO 를 다시 만든 경우,
     * 열어 둔 fd 는 지워진 예전 FIFO 를 가리켜 EPIPE 가 난다. 한 바이트도 못 썼을 때만 새로 열어 한 번 더 보낸다.
     */
    for (int attempt = 0; attempt < 2; attempt++) {
        if (srv_resp_fd[id] < 0) {
            srv_resp_fd[id] = open_resp(id);
            if (srv_resp_fd[id] < 0) break;
        }
        struct iovec iov[2] = {
            { (void *)hdr, sizeof *hdr },
            { (void *)payload, hdr->payload_len },
        };
        size_t done;
        if (writev_full(srv_resp_fd[id], iov, hdr->payload_len ? 2 : 1, &done) == 0) {
            rc = 0;
            break;
        }
        int e = errno;
        drop_resp_locked(id);            /* 중간까지 쓴 스트림은 이어 쓸 수 없다 */
        errno = e;
        if (!(e == EPIPE && done == 0)) break;
    }

    pthread_mutex_unlock(&srv_resp_mtx[id]);
    return rc;
}

/*
 * 막혀 있는 tp_recv_req 를 깨운다. 요청 FIFO 에 type 0 머리 하나를 막히지 않게 쓴다.
 * 수신 스레드는 srv_stopping 을 보고 다음 호출에서 끝나므로, 깨우기는 수신 스레드가 빈 FIFO 를 기다리며
 * 막혀 있을 때만 필요하다. 그때는 FIFO 에 자리가 있어 바로 써진다.
 * DECISION: 깨우기는 막히지 않게(O_NONBLOCK) 쓰고 송신 잠금도 잡지 않는다. 막히는 write 나 잠금은 서버를 멈춘다:
 *   수신 스레드가 먼저 끝나 FIFO 가 가득 찬 채로 남거나, 큰 요청을 쓰던 클라이언트가 잠금을 쥔 채 그 FIFO 에서
 *   막혀 있기 때문이다. 큰 요청 도중에 서버를 SIGINT 로 끝내는 시험에서 40번 중 2번 멈췄다 (잠금을 잡으면 60번 중 55번).
 *   깨우기가 큰 요청의 중간에 끼어들면 그 요청은 깨진 채 한 번 처리되고, 다음 호출에서 수신 스레드가 끝난다.
 */
void tp_server_wake(void)
{
    if (!srv_opened) return;
    atomic_store(&srv_stopping, true);
    msg_hdr_t h;
    memset(&h, 0, sizeof h);
    h.type = WAKE_TYPE;
    while (write(srv_wake_fd, &h, sizeof h) < 0 && errno == EINTR)
        ;                                /* EAGAIN = FIFO 가 차 있다 = 수신 스레드가 막혀 있지 않다 */
}

void tp_server_close(void)
{
    if (!srv_opened) return;
    char rp[PATH_LEN], lp[PATH_LEN];
    req_path(rp);
    lock_path(lp);

    /* 먼저 이름을 지워 새 클라이언트가 붙지 못하게 한다 */
    unlink(rp);
    unlink(lp);

    for (uint32_t i = 0; i < MAX_CLIENTS; i++) {
        pthread_mutex_lock(&srv_resp_mtx[i]);
        drop_resp_locked(i);             /* 클라이언트의 수신은 EOF 를 받고 끝난다 */
        pthread_mutex_unlock(&srv_resp_mtx[i]);
        pthread_mutex_destroy(&srv_resp_mtx[i]);
    }
    close(srv_req_fd);  srv_req_fd = -1;
    close(srv_wake_fd); srv_wake_fd = -1;
    close(srv_lock_fd); srv_lock_fd = -1;
    srv_opened = 0;
}

/* ================================================================
 * 클라이언트 쪽
 * ================================================================ */

static int cli_req_fd = -1;          /* 요청 FIFO 쓰기 끝 */
static int cli_lock_fd = -1;
static int cli_resp_fd = -1;         /* 내 응답 FIFO 읽기 끝 */
static atomic_int cli_keeper_fd = -1;
static uint32_t cli_id;
static int cli_opened;

/*
 * 내 응답 FIFO 를 만들고 요청 채널에 연결한다.
 * 서버가 없으면 -1, errno = ECONNREFUSED. 같은 번호의 클라이언트가 실행 중이면 -1, errno = EADDRINUSE.
 *
 * 응답 FIFO 를 여는 순서
 *   1. 읽기 끝을 O_NONBLOCK 으로 연다 (쓰는 쪽이 없어도 막히지 않음).
 *   2. 같은 FIFO 의 쓰기 끝(keeper)을 하나 열어 둔다. 서버가 아직 쓰기 끝을 열지 않았을 때
 *      read 가 EOF(0)를 돌려주지 않게 하려는 것이다. 이제 서버도 막히지 않고 쓰기 끝을 열 수 있다.
 *   3. 읽기 끝을 막힘 모드로 되돌린다.
 *   4. 첫 응답을 받으면 keeper 를 닫는다. 그 뒤 서버가 죽으면 쓰는 쪽이 모두 사라져
 *      tp_recv_resp 가 EOF 로 끝난다 (ECONNRESET). 서버가 죽었는데 클라이언트가 영원히 기다리지 않는다.
 */
int tp_client_open(uint32_t client_id)
{
    char rp[PATH_LEN], lp[PATH_LEN], mp[PATH_LEN];
    if (client_id >= MAX_CLIENTS) { errno = EINVAL; return -1; }
    req_path(rp);
    lock_path(lp);
    resp_path(mp, client_id);

    ignore_sigpipe();

    /* 서버가 있는지 먼저 본다. 없으면 내 FIFO 를 만들지 않고 끝낸다 */
    int used = fifo_in_use(rp);
    if (used != 1) {
        if (used == 0 || errno == ENOENT) errno = ECONNREFUSED;
        return -1;
    }

    if (make_fifo(mp) < 0) return -1;            /* 같은 번호가 실행 중이면 EADDRINUSE */

    int keeper = -1;
    cli_resp_fd = open(mp, O_RDONLY | O_NONBLOCK);
    if (cli_resp_fd < 0) goto fail;
    keeper = open(mp, O_WRONLY | O_NONBLOCK);
    if (keeper < 0) goto fail;
    if (clear_nonblock(cli_resp_fd) < 0) goto fail;

    cli_req_fd = open(rp, O_WRONLY | O_NONBLOCK);
    if (cli_req_fd < 0) {
        if (errno == ENXIO || errno == ENOENT) errno = ECONNREFUSED;
        goto fail;
    }
    if (clear_nonblock(cli_req_fd) < 0) goto fail;

    cli_lock_fd = open(lp, O_RDWR);
    if (cli_lock_fd < 0) {
        if (errno == ENOENT) errno = ECONNREFUSED;
        goto fail;
    }

    atomic_store(&cli_keeper_fd, keeper);
    cli_id = client_id;
    cli_opened = 1;
    return 0;

fail:;
    int e = errno;
    if (keeper >= 0) close(keeper);
    if (cli_resp_fd >= 0) { close(cli_resp_fd); cli_resp_fd = -1; }
    if (cli_req_fd >= 0)  { close(cli_req_fd);  cli_req_fd = -1; }
    unlink(mp);
    errno = e;
    return -1;
}

int tp_send_req(const msg_hdr_t *hdr, const void *payload)
{
    /* 서버가 죽었으면 읽는 쪽이 없어 EPIPE 로 실패한다 (SIGPIPE 는 무시함) */
    return send_on_req(cli_req_fd, cli_lock_fd, hdr, payload);
}

/* 응답 1개 받기. 수신 스레드 하나만 부른다. 서버가 끝나면 -1, errno = ECONNRESET */
ssize_t tp_recv_resp(msg_hdr_t *hdr, void *payload, size_t cap)
{
    ssize_t n = recv_msg(cli_resp_fd, hdr, payload, cap);
    if (n >= 0 || errno == EMSGSIZE) {
        int k = atomic_exchange(&cli_keeper_fd, -1);
        if (k >= 0) close(k);
    }
    return n;
}

void tp_client_close(void)
{
    if (!cli_opened) return;
    char mp[PATH_LEN];
    resp_path(mp, cli_id);
    unlink(mp);

    int k = atomic_exchange(&cli_keeper_fd, -1);
    if (k >= 0) close(k);
    close(cli_resp_fd); cli_resp_fd = -1;
    close(cli_req_fd);  cli_req_fd = -1;
    close(cli_lock_fd); cli_lock_fd = -1;
    cli_opened = 0;
}
