/*
 * transport_pipe.c - FIFO(named pipe)로 구현한 전송 계층
 *
 * 채널 (docs/design/README.md#채널)
 *   요청 채널  /tmp/osproject_<팀>_req          모든 클라이언트 → 서버 (N:1)
 *   응답 채널  /tmp/osproject_<팀>_resp_<id>    서버 → 클라이언트 id (클라이언트마다 1개)
 *   잠금 파일  /tmp/osproject_<팀>_req.lock     PIPE_BUF 를 넘는 요청을 보낼 때만 쓰는 송신 잠금(fcntl),
 *                                                서버·클라이언트가 FIFO 를 만들고 여는 시작 구간을 묶는 시작 잠금(flock)
 *
 * 메시지 하나 = msg_hdr_t + payload_len 바이트. 받는 쪽은 머리를 먼저 읽고 payload 를 끝까지 읽는다.
 *
 * 섞임 방지
 *   - 요청이 PIPE_BUF(4096B) 이하이면 write 한 번으로 보낸다. 커널이 원자성을 보장하므로 잠금이 없다.
 *   - 요청이 PIPE_BUF 를 넘으면(1단계 벤치마크의 큰 메시지) 잠금 파일에 fcntl 쓰기 잠금을 걸고 보낸다.
 *   - 응답은 클라이언트별 mutex 를 잡고 보낸다. 큰 응답이 여러 번의 write 로 나뉘어도 섞이지 않는다.
 *
 * 설계 개선 (서버 환경 변수 OSP_PIPE_RBUF=1, 기본값은 처음 설계)
 *   받기 버퍼: 처음 설계는 메시지마다 머리와 본문을 따로 read 한다(메시지 하나에 read 2번). 켜면 서버 수신 스레드와
 *   클라이언트 수신 스레드가 FIFO 에 쌓인 바이트를 한 번에 읽어(최대 RBUF_CAP) 사용자 버퍼에서 메시지를 꺼낸다.
 *   서버가 잠금 파일에 설정을 적고 클라이언트가 읽어 따른다(transport_mq.c 와 같은 방식).
 * 원인 확인 실험용 (서버 환경 변수 OSP_PIPE_RESP_SZ=<바이트>)
 *   응답 FIFO 를 F_SETPIPE_SZ 로 그 크기로 키운다. 처음 설계의 응답 FIFO 는 기본 64KB(65,536B)라 가장 큰 응답
 *   (SNAPSHOT 65,592B)이 56B 넘치고, 그러면 워커가 FIFO 에서 잠들어 클라이언트가 비워 줄 때까지 기다린다.
 *   사용자별 파이프 버퍼 한도(pipe-user-pages-soft, 기본 64MB)를 넘으면 커널이 거부하므로 설계 개선으로는 쓰지 않는다
 *   (클라이언트 1024개면 처음 설계도 이 한도를 다 써서 마지막 150개쯤의 FIFO 가 8KB 로 만들어진다).
 * 설계 개선: 워커가 응답 FIFO 에서 클라이언트를 기다리지 않게 한다. 두 부분이고 서버 환경 변수로 따로 켠다.
 *   OSP_PIPE_GROW=1  응답 FIFO 키우기: 큰 응답(32KB 초과)을 보낼 때 그 클라이언트의 FIFO 를 128KB 로 키운다
 *                    (클라이언트당 160KB 이하, 요구사항 N-5). 사용자별 한도를 새 클라이언트의 FIFO 몫으로 남기려고
 *                    붙어 있는 클라이언트가 한도의 3/4 ÷ 128KB(기본 384개) 이하이고 0.1초 동안 새로 붙지 않았을 때만
 *                    키우고, 그보다 많이 붙으면 키운 FIFO 를 그 클라이언트에게 다음에 보낼 때 64KB 로 되돌린다.
 *   OSP_PIPE_ASYNC=1 송신 스레드: 워커는 응답 FIFO 에 막히지 않게(O_NONBLOCK) 쓰고, 자리가 없어 못 쓴 나머지는
 *                    클라이언트별 자리에 옮겨 두고 바로 다음 요청으로 간다. 송신 스레드가 epoll 로 FIFO 에 자리가
 *                    나기를 기다렸다가 나머지를 쓴다. 나머지가 있는 동안 온 응답은 순서를 지키려고 그 뒤에 붙인다.
 *                    FIFO 와 나머지를 합쳐 클라이언트당 160KB(N-5)를 넘을 때는 처음 설계처럼 워커가 기다린다.
 */
#define _GNU_SOURCE             /* F_GETPIPE_SZ */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <sys/resource.h>

#include "transport.h"

#define PATH_LEN 64
#define WAKE_TYPE 0u          /* tp_server_wake 가 보내는 머리의 type. 실제 메시지는 1 이상이다 */
#define DRAIN_BUF 4096
#define CONF_OFF  ((off_t)4096)  /* 잠금 파일에서 설정(conf_t)을 적는 자리 */
#define RBUF_CAP  65536          /* 받기 버퍼 크기. 요청(40B 이하)이면 1,600개 남짓이 한 번에 들어온다 */

/* 서버가 잠금 파일에 적고 클라이언트가 읽는 설정 */
typedef struct {
    uint32_t rbuf;           /* 1 이면 받기 버퍼를 쓴다 */
    uint32_t reserved;
} conf_t;

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
 * 시작 잠금. make_fifo 의 "읽는 쪽이 없으면 남은 파일" 판정은 만든 쪽이 읽기 끝을 열기 전까지 틀릴 수 있다.
 * 같은 이름을 쓰는 두 프로세스가 같은 순간에 뜨면 한쪽이 다른 쪽이 막 만든 FIFO 를 지워
 * 둘 다 뜨거나(서버 둘이 요청 FIFO 하나를 나눠 읽는다) 둘 다 실패한다.
 * 판정 → 지우고 다시 만들기 → 읽기 끝 열기를 잠금 파일의 flock 으로 묶어 한 번에 하나만 하게 한다.
 * 읽기 끝을 연 뒤에는 읽는 쪽이 있으므로 뒤에 오는 쪽은 EADDRINUSE 로 판정한다.
 * DECISION: 송신 잠금(fcntl 레코드 잠금)과 같은 파일이지만 Linux 의 flock 과 fcntl 잠금은 서로 독립이라
 * (로컬 파일시스템) 시작 잠금이 큰 요청의 송신을 막지 않는다. 프로세스가 죽으면 커널이 flock 을 풀어 준다.
 */
static int start_lock(int fd)
{
    while (flock(fd, LOCK_EX) < 0)
        if (errno != EINTR) return -1;
    return 0;
}

static void start_unlock(int fd)
{
    int e = errno;
    flock(fd, LOCK_UN);
    errno = e;
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

/*
 * 받기 버퍼 (설계 개선, OSP_PIPE_RBUF=1).
 * FIFO 에 쌓인 바이트를 read 한 번으로 최대 RBUF_CAP 까지 가져와 버퍼에서 메시지를 꺼낸다. 요청이 몰려 있으면
 * read 한 번에 요청 수백 개가 들어와, 메시지마다 read 2번(머리, 본문)이던 시스템 콜이 메시지 수보다 훨씬 적어진다.
 * 버퍼에 다 들어오지 않은 큰 본문(64KB SNAPSHOT 응답)은 버퍼에 있는 앞부분만 복사하고 나머지는 받는 쪽 버퍼로
 * 바로 읽는다. 그래서 큰 메시지의 복사 횟수는 처음 설계와 같다.
 * DECISION: 버퍼는 수신 스레드 하나만 쓴다(tp_recv_req·tp_recv_resp 는 스레드 하나만 부른다는 transport.h 의 약속).
 *   그래서 락이 없다. 메모리는 정적 배열이라 측정 구간에 malloc 이 없다(N-7).
 */
typedef struct {
    char  *mem;
    size_t off, len;         /* 아직 꺼내지 않은 바이트 = mem[off, off + len) */
} rbuf_t;

static char srv_rb_mem[RBUF_CAP], cli_rb_mem[RBUF_CAP];
static rbuf_t srv_rb = { srv_rb_mem, 0, 0 }, cli_rb = { cli_rb_mem, 0, 0 };
static bool srv_rbuf_on, cli_rbuf_on;

static void rb_used(rbuf_t *b, size_t n)
{
    b->off += n;
    b->len -= n;
    if (b->len == 0) b->off = 0;
}

/* 버퍼에 적어도 need 바이트(need <= RBUF_CAP)가 모일 때까지 읽는다. read 는 FIFO 에 있는 만큼만 돌려준다 */
static int rb_fill(int fd, rbuf_t *b, size_t need)
{
    if (b->len >= need) return 0;
    if (b->off + need > RBUF_CAP) {             /* 뒤쪽 자리가 모자라면 남은 바이트를 앞으로 옮긴다 */
        memmove(b->mem, b->mem + b->off, b->len);
        b->off = 0;
    }
    while (b->len < need) {
        ssize_t r = read(fd, b->mem + b->off + b->len, RBUF_CAP - b->off - b->len);
        if (r > 0) { b->len += (size_t)r; continue; }
        if (r == 0) { errno = ECONNRESET; return -1; }
        if (errno == EINTR) continue;
        return -1;
    }
    return 0;
}

/* 본문 n 바이트를 dst 로 꺼낸다(dst 가 NULL 이면 버린다). 버퍼에 없는 나머지가 크면 버퍼를 거치지 않고 바로 읽는다 */
static int rb_take(int fd, rbuf_t *b, char *dst, size_t n)
{
    size_t k = b->len < n ? b->len : n;
    if (dst) memcpy(dst, b->mem + b->off, k);
    rb_used(b, k);
    n -= k;
    if (n == 0) return 0;
    if (!dst) return drain(fd, n);
    if (n >= RBUF_CAP / 4) return read_full(fd, dst + k, n);
    if (rb_fill(fd, b, n) < 0) return -1;       /* 작은 나머지는 뒤따르는 메시지와 함께 읽히게 버퍼로 */
    memcpy(dst + k, b->mem + b->off, n);
    rb_used(b, n);
    return 0;
}

/* recv_msg 의 받기 버퍼판. 결과와 errno 는 recv_msg 와 같다 */
static ssize_t recv_msg_rb(int fd, rbuf_t *b, msg_hdr_t *hdr, void *payload, size_t cap)
{
    if (rb_fill(fd, b, sizeof *hdr) < 0) return -1;
    memcpy(hdr, b->mem + b->off, sizeof *hdr);
    rb_used(b, sizeof *hdr);
    if (hdr->payload_len > cap) {
        if (rb_take(fd, b, NULL, hdr->payload_len) < 0) return -1;
        errno = EMSGSIZE;
        return -1;
    }
    if (hdr->payload_len && rb_take(fd, b, payload, hdr->payload_len) < 0) return -1;
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
static uint32_t lim_clients = 16;                /* tp_set_limits. 부르지 않으면 1단계 값 */
static int srv_resp_sz;                          /* 응답 FIFO 를 키울 크기. 0 이면 그대로 (원인 확인 실험) */

/* 설계 개선 (OSP_PIPE_GROW=1): 응답 FIFO 키우기 */
#define GROW_SZ      131072                      /* 가장 큰 응답(65,592B)이 통째로 들어가는 2의 거듭제곱 */
#define GROW_MIN     32768u                      /* 이보다 큰 응답을 보낼 때만 키운다 (SNAPSHOT) */
#define GROW_HOLD_NS 100000000ll                 /* 접속이 0.1초 동안 늘지 않았을 때만 키운다 (몰려 붙는 동안은 아니다) */
static bool srv_grow;
static int srv_grow_max;                         /* 붙어 있는 클라이언트가 이만큼 이하일 때만 키운다 */
static atomic_int srv_nopen;                     /* 서버가 연 응답 FIFO 수 = 붙어 있는 클라이언트 수 */
static _Atomic long long srv_open_ns;            /* 마지막으로 응답 FIFO 를 연 시각 (CLOCK_MONOTONIC) */
static bool srv_grown[MAX_CLIENTS];              /* srv_resp_mtx[id] 로 지킨다 */

/*
 * 설계 개선 (OSP_PIPE_ASYNC=1): 워커가 다 못 쓴 응답의 나머지.
 * 클라이언트마다 PEND_CAP 바이트 자리를 서버를 처음 열 때 mmap 으로 한 번 잡는다(측정 중 malloc 없음, 쓴 쪽만 RSS).
 * 실제로 쓰는 한도는 160KB − 응답 FIFO 크기다(pend_cap_locked): 기본 64KB FIFO 면 96KB(가장 큰 응답 65,592B 가
 * 들어간다), 키운 128KB FIFO 면 32KB. FIFO 와 합쳐 클라이언트당 160KB 를 넘지 않는다(요구사항 N-5).
 * 모든 칸은 srv_resp_mtx[id] 로 지킨다.
 */
#define PEND_CAP  98304u                         /* 나머지 자리의 최대 (기본 64KB FIFO 와 합쳐 160KB) */
#define CLIENT_BUF_MAX 163840u                    /* 클라이언트당 버퍼 한도 160KB (요구사항 N-5): FIFO + 나머지 */
#define EV_WAKE   UINT32_MAX                     /* epoll 의 data: 송신 스레드 깨우기(eventfd) */
typedef struct {
    uint32_t off, len;           /* 아직 못 쓴 바이트 = pend_buf(id)[off, off + len) */
    uint32_t gen;                /* 응답 FIFO 를 닫을 때마다 1 증가. 자리를 기다리던 워커가 세션이 바뀐 것을 안다 */
    bool armed;                  /* 송신 스레드의 epoll 에 EPOLLOUT(한 번)으로 걸려 있다 */
    bool reg;                    /* 지금 fd 가 epoll 에 등록돼 있다 */
} pend_t;
static bool srv_async;
static char *srv_pend_mem;                       /* MAX_CLIENTS × PEND_CAP. 프로세스가 끝날 때까지 둔다 */
static pend_t srv_pend[MAX_CLIENTS];
static pthread_cond_t srv_pend_cv[MAX_CLIENTS];  /* 나머지가 줄거나 FIFO 가 닫히면 깨운다 */
static pthread_once_t srv_pend_once = PTHREAD_ONCE_INIT;
static atomic_int srv_pend_n;                    /* 나머지가 남은 클라이언트 수 (닫기 전에 비울 때 본다) */
static int srv_ep = -1, srv_evfd = -1;
static pthread_t srv_sender;
static bool srv_sender_on;
static atomic_bool srv_sender_stop;

/*
 * 클라이언트 수만 쓴다. 서버는 응답 FIFO 를 클라이언트마다 하나씩 열어 두므로, 열린 파일 한도(터미널 기본 soft 1024)가
 * 클라이언트 수보다 작으면 1000개쯤에서 EMFILE 이 난다. tp_server_open 이 soft 한도를 hard 한도 안에서 올린다.
 * 메시지 크기는 FIFO 가 바이트 스트림이라 버퍼 크기와 관계없다.
 */
int tp_set_limits(uint32_t max_clients, size_t max_req, size_t max_resp)
{
    if (max_clients < 1 || max_clients > MAX_CLIENTS || max_req < sizeof(msg_hdr_t) || max_resp < sizeof(msg_hdr_t)) {
        errno = EINVAL;
        return -1;
    }
    lim_clients = max_clients;
    (void)max_resp;
    return 0;
}

/* 원인 확인 실험: 응답 FIFO 를 키울 크기 (OSP_PIPE_RESP_SZ). 없으면 0 = 그대로 */
static int pick_resp_size(void)
{
    const char *fixed = getenv("OSP_PIPE_RESP_SZ");
    return fixed && atoi(fixed) > 0 ? atoi(fixed) : 0;
}

static long long mono_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000000ll + t.tv_nsec;
}

static long read_proc_long(const char *path)
{
    long v = -1;
    FILE *f = fopen(path, "r");
    if (f) {
        if (fscanf(f, "%ld", &v) != 1) v = -1;
        fclose(f);
    }
    return v;
}

/* 설계 개선(OSP_PIPE_GROW=1)을 켤 때: 키워도 되는 클라이언트 수 = 사용자별 한도의 3/4 ÷ 128KB. 못 키우면 0 */
static int grow_limit(void)
{
    long page = sysconf(_SC_PAGESIZE);
    long soft = read_proc_long("/proc/sys/fs/pipe-user-pages-soft");    /* 쪽 수. 0 = 한도 없음 */
    long maxsz = read_proc_long("/proc/sys/fs/pipe-max-size");
    if (page <= 0 || soft < 0 || maxsz < GROW_SZ) return 0;
    if (soft == 0) return (int)MAX_CLIENTS;
    long n = soft / 4 * 3 / (GROW_SZ / page);
    return n > (long)MAX_CLIENTS ? (int)MAX_CLIENTS : (int)n;
}

/*
 * 보내기 직전에 응답 FIFO 크기를 맞춘다(OSP_PIPE_GROW=1). srv_resp_mtx[id] 를 잡고 부른다.
 * 큰 응답이고 붙은 클라이언트가 적고 접속이 잠잠하면 128KB 로 키운다. 커널이 한도로 거부하면(EPERM) 그대로 쓴다.
 * 키운 뒤에 클라이언트가 많아지면 64KB 로 되돌린다(FIFO 에 64KB 넘게 차 있으면 EBUSY, 다음에 다시 해 본다).
 */
static void fit_resp_locked(uint32_t id, size_t total)
{
    int fd = srv_resp_fd[id];
    int n = atomic_load(&srv_nopen);
    if (srv_grown[id]) {
        if (n > srv_grow_max && fcntl(fd, F_SETPIPE_SZ, 65536) >= 0) srv_grown[id] = false;
        return;
    }
    if (total <= GROW_MIN || n > srv_grow_max || mono_ns() - atomic_load(&srv_open_ns) < GROW_HOLD_NS) return;
    if (fcntl(fd, F_SETPIPE_SZ, GROW_SZ) >= 0) srv_grown[id] = true;
}

/* ---- 설계 개선: 송신 스레드 (OSP_PIPE_ASYNC=1) ---- */

static char *pend_buf(uint32_t id) { return srv_pend_mem + (size_t)id * PEND_CAP; }

static void pend_init_once(void)
{
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    for (uint32_t i = 0; i < MAX_CLIENTS; i++) pthread_cond_init(&srv_pend_cv[i], &a);
    pthread_condattr_destroy(&a);
}

/* id 의 응답 FIFO 에 자리가 나면 송신 스레드가 깨게 건다(EPOLLOUT, 한 번). srv_resp_mtx[id] 를 잡고 부른다 */
static void pend_arm(uint32_t id)
{
    pend_t *p = &srv_pend[id];
    if (p->armed || srv_resp_fd[id] < 0) return;
    struct epoll_event ev = { .events = EPOLLOUT | EPOLLONESHOT, .data.u32 = id };
    if (epoll_ctl(srv_ep, p->reg ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, srv_resp_fd[id], &ev) == 0) {
        p->reg = true;
        p->armed = true;
    }
}

/* 나머지를 버리고 epoll 에서 뺀다. FIFO 를 닫기 전에 부른다. 자리를 기다리던 워커를 깨운다 */
static void pend_clear_locked(uint32_t id)
{
    pend_t *p = &srv_pend[id];
    if (p->reg && srv_resp_fd[id] >= 0) epoll_ctl(srv_ep, EPOLL_CTL_DEL, srv_resp_fd[id], NULL);
    if (p->len > 0) atomic_fetch_sub(&srv_pend_n, 1);
    p->off = p->len = 0;
    p->reg = p->armed = false;
    p->gen++;
    pthread_cond_broadcast(&srv_pend_cv[id]);
}

static size_t pend_cap_locked(uint32_t id);

/* 메시지(머리 + payload)의 skip 바이트 뒤를 나머지 끝에 붙인다. 자리는 부르는 쪽이 확인했다 */
static void pend_put(uint32_t id, const msg_hdr_t *hdr, const void *payload, size_t skip)
{
    pend_t *p = &srv_pend[id];
    char *b = pend_buf(id);
    size_t hl = sizeof *hdr, total = hl + hdr->payload_len, add = total - skip;
    if (p->off + p->len + add > PEND_CAP) {                 /* 뒤쪽 자리가 모자라면 앞으로 당긴다 */
        memmove(b, b + p->off, p->len);
        p->off = 0;
    }
    char *dst = b + p->off + p->len;
    if (skip < hl) {
        memcpy(dst, (const char *)hdr + skip, hl - skip);
        dst += hl - skip;
        skip = hl;
    }
    memcpy(dst, (const char *)payload + (skip - hl), total - skip);
    if (p->len == 0) atomic_fetch_add(&srv_pend_n, 1);
    p->len += (uint32_t)add;
#ifdef PEND_CHECK
    if (p->len > pend_cap_locked(id)) { fprintf(stderr, "PEND_CHECK: id %u len %u cap %zu\n", id, p->len, pend_cap_locked(id)); abort(); }
#endif
}

static void drop_resp_locked(uint32_t id);

/* 송신 스레드: FIFO 에 자리가 난 클라이언트의 나머지를 쓸 수 있는 만큼 쓴다 */
static void *sender_main(void *arg)
{
    (void)arg;
    struct epoll_event ev[64];
    while (!atomic_load(&srv_sender_stop)) {
        int n = epoll_wait(srv_ep, ev, 64, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; i++) {
            uint32_t id = ev[i].data.u32;
            if (id == EV_WAKE) {
                uint64_t x;
                if (read(srv_evfd, &x, sizeof x) < 0) { /* 이미 비었다 */ }
                continue;
            }
            pthread_mutex_lock(&srv_resp_mtx[id]);
            pend_t *p = &srv_pend[id];
            p->armed = false;
            ssize_t w = 0;
            while (p->len > 0 && srv_resp_fd[id] >= 0) {
                w = write(srv_resp_fd[id], pend_buf(id) + p->off, p->len);
                if (w < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                p->off += (uint32_t)w;
                p->len -= (uint32_t)w;
                if (p->len == 0) {
                    p->off = 0;
                    atomic_fetch_sub(&srv_pend_n, 1);
                }
                pthread_cond_broadcast(&srv_pend_cv[id]);
            }
            if (w < 0 && errno != EAGAIN) drop_resp_locked(id);  /* EPIPE: 클라이언트가 끝났다. 나머지는 버린다 */
            else if (p->len > 0) pend_arm(id);
            pthread_mutex_unlock(&srv_resp_mtx[id]);
        }
    }
    return NULL;
}

/* 서버를 열 때: 나머지 자리, epoll, 깨우기 eventfd, 송신 스레드 */
static int async_start(void)
{
    pthread_once(&srv_pend_once, pend_init_once);
    if (!srv_pend_mem) {
        void *m = mmap(NULL, (size_t)MAX_CLIENTS * PEND_CAP, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (m == MAP_FAILED) return -1;
        srv_pend_mem = m;
    }
    memset(srv_pend, 0, sizeof srv_pend);
    atomic_store(&srv_pend_n, 0);
    atomic_store(&srv_sender_stop, false);
    srv_ep = epoll_create1(EPOLL_CLOEXEC);
    srv_evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    struct epoll_event ev = { .events = EPOLLIN, .data.u32 = EV_WAKE };
    if (srv_ep < 0 || srv_evfd < 0 || epoll_ctl(srv_ep, EPOLL_CTL_ADD, srv_evfd, &ev) < 0) goto fail;
    /* 신호는 응용이 정한 스레드가 받는다. 송신 스레드는 모두 막고 시작한다 (transport_mq.c 의 감시 스레드와 같다) */
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    int rc = pthread_create(&srv_sender, NULL, sender_main, NULL);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (rc != 0) { errno = rc; goto fail; }
    srv_sender_on = true;
    return 0;
fail:;
    int e = errno;
    if (srv_ep >= 0) { close(srv_ep); srv_ep = -1; }
    if (srv_evfd >= 0) { close(srv_evfd); srv_evfd = -1; }
    errno = e;
    return -1;
}

/* 서버를 닫을 때: 남은 나머지를 잠깐(최대 0.5초) 비우게 두고 송신 스레드를 끝낸다 */
static void async_stop(void)
{
    if (!srv_sender_on) return;
    for (int i = 0; i < 500 && atomic_load(&srv_pend_n) > 0; i++) {
        struct timespec ms = { 0, 1000000 };
        nanosleep(&ms, NULL);
    }
    atomic_store(&srv_sender_stop, true);
    uint64_t one = 1;
    if (write(srv_evfd, &one, sizeof one) < 0) { /* 카운터가 이미 차 있다 = 이미 깨어 있다 */ }
    pthread_join(srv_sender, NULL);
    srv_sender_on = false;
}

/* 열린 파일 한도를 클라이언트 수 + 여유 64개 이상으로 올린다. hard 한도가 모자라면 그대로 두고 0 (열 때 EMFILE 로 알게 된다) */
static void raise_nofile(void)
{
    struct rlimit rl;
    rlim_t need = (rlim_t)lim_clients + 64;
    if (getrlimit(RLIMIT_NOFILE, &rl) < 0 || rl.rlim_cur >= need) return;
    rl.rlim_cur = rl.rlim_max == RLIM_INFINITY || rl.rlim_max >= need ? need : rl.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rl);
}

int tp_server_open(void)
{
    char rp[PATH_LEN], lp[PATH_LEN];
    req_path(rp);
    lock_path(lp);

    ignore_sigpipe();
    raise_nofile();

    /* 잠금 파일을 먼저 만들고 시작 잠금을 건다. 클라이언트는 요청 FIFO 에 읽는 쪽이 생긴 뒤에야 이 파일을 연다 */
    srv_lock_fd = open(lp, O_RDWR | O_CREAT, 0600);
    if (srv_lock_fd < 0) return -1;
    if (start_lock(srv_lock_fd) < 0 || make_fifo(rp) < 0) {
        /* 서버가 이미 떠 있으면 EADDRINUSE. 그 서버의 FIFO·잠금 파일이므로 지우지 않는다. 닫으면 flock 도 풀린다 */
        int e = errno;
        close(srv_lock_fd);
        srv_lock_fd = -1;
        errno = e;
        return -1;
    }

    /* 설계 개선 설정. 클라이언트가 읽을 수 있게 요청 FIFO 를 열기 전에 잠금 파일에 적는다 */
    const char *rb = getenv("OSP_PIPE_RBUF");
    conf_t conf = { .rbuf = rb && atoi(rb) == 1 };
    srv_rbuf_on = conf.rbuf;
    srv_rb.off = srv_rb.len = 0;
    srv_resp_sz = pick_resp_size();
    if (pwrite(srv_lock_fd, &conf, sizeof conf, CONF_OFF) != (ssize_t)sizeof conf) goto fail;

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
    start_unlock(srv_lock_fd);                  /* 읽는 쪽이 생겼다. 뒤에 뜨는 서버는 EADDRINUSE 로 판정한다 */

    for (uint32_t i = 0; i < MAX_CLIENTS; i++) {
        srv_resp_fd[i] = -1;
        pthread_mutex_init(&srv_resp_mtx[i], NULL);
    }
    atomic_store(&srv_stopping, false);

    /* 설계 개선: 응답 FIFO 키우기, 송신 스레드 */
    const char *gr = getenv("OSP_PIPE_GROW");
    srv_grow_max = gr && atoi(gr) == 1 && srv_resp_sz == 0 ? grow_limit() : 0;
    srv_grow = srv_grow_max > 0;
    atomic_store(&srv_nopen, 0);
    atomic_store(&srv_open_ns, mono_ns());
    memset(srv_grown, 0, sizeof srv_grown);
    const char *as = getenv("OSP_PIPE_ASYNC");
    srv_async = as && atoi(as) == 1;
    if (srv_async && async_start() < 0) {
        for (uint32_t i = 0; i < MAX_CLIENTS; i++) pthread_mutex_destroy(&srv_resp_mtx[i]);
        srv_async = false;
        goto fail;
    }
    srv_opened = 1;
    return 0;

fail:;
    int e = errno;
    if (srv_req_fd >= 0) { close(srv_req_fd); srv_req_fd = -1; }
    if (srv_wake_fd >= 0) { close(srv_wake_fd); srv_wake_fd = -1; }
    /* 시작 잠금을 쥔 채로 내가 만든 FIFO 와 잠금 파일을 지운 뒤 닫는다(flock 이 풀린다) */
    unlink(rp);
    unlink(lp);
    close(srv_lock_fd); srv_lock_fd = -1;
    errno = e;
    return -1;
}

static int open_resp(uint32_t id);

/* 클라이언트 id 의 응답 FIFO 쓰기 끝을 닫는다. 호출하는 쪽이 srv_resp_mtx[id] 를 잡고 있어야 한다 */
static void drop_resp_locked(uint32_t id)
{
    if (srv_async) pend_clear_locked(id);           /* 닫기 전에 epoll 에서 빼고 나머지를 버린다 */
    if (srv_resp_fd[id] >= 0) {
        close(srv_resp_fd[id]);
        srv_resp_fd[id] = -1;
        atomic_fetch_sub(&srv_nopen, 1);
        srv_grown[id] = false;
    }
}

/*
 * 요청 1개 받기. 수신 스레드 하나만 부른다.
 * tp_server_wake 로 깨워지면 -1, errno = ECANCELED.
 */
ssize_t tp_recv_req(msg_hdr_t *hdr, void *payload, size_t cap)
{
    if (atomic_load(&srv_stopping)) { errno = ECANCELED; return -1; }

    ssize_t n = srv_rbuf_on ? recv_msg_rb(srv_req_fd, &srv_rb, hdr, payload, cap)
                            : recv_msg(srv_req_fd, hdr, payload, cap);
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
    /* 송신 스레드(설계 개선)를 쓰면 막히지 않는 쓰기 그대로 둔다 */
    if (!srv_async && clear_nonblock(fd) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    /* 원인 확인 실험: 응답 FIFO 크기. 거부되면(사용자별 한도) 그대로 쓴다. 실제 크기는 tp_server_info 가 기록 */
    if (srv_resp_sz > 0) (void)fcntl(fd, F_SETPIPE_SZ, srv_resp_sz);
    atomic_fetch_add(&srv_nopen, 1);                /* 설계 개선(OSP_PIPE_GROW)이 보는 접속 수와 시각 */
    atomic_store(&srv_open_ns, mono_ns());
    return fd;
}

/*
 * 이 클라이언트의 나머지 자리 한도. 응답 FIFO 와 나머지를 합쳐 클라이언트당 160KB(요구사항 N-5)를 넘지 않게
 * 160KB − FIFO 크기로 잡는다: 기본 64KB FIFO 면 96KB, 키운 128KB FIFO 면 32KB.
 * (한도에 걸려 8KB 로 만들어진 FIFO 도 64KB 로 쳐서 넉넉히 잡지 않는다.) srv_resp_mtx[id] 를 잡고 부른다.
 */
static size_t pend_cap_locked(uint32_t id)
{
    size_t fifo = srv_resp_sz > 0 ? (size_t)srv_resp_sz : srv_grown[id] ? (size_t)GROW_SZ : 65536u;
    size_t room = fifo < CLIENT_BUF_MAX ? CLIENT_BUF_MAX - fifo : 0;
    return room < PEND_CAP ? room : PEND_CAP;
}

/* 메시지(머리 + payload)의 *done 바이트 뒤를 막히지 않게 쓸 수 있는 만큼 쓴다. 자리가 없으면(EAGAIN) 0, 오류 -1 */
static int write_from(int fd, const msg_hdr_t *hdr, const void *payload, size_t *done)
{
    size_t hl = sizeof *hdr, total = hl + hdr->payload_len;
    while (*done < total) {
        struct iovec iov[2];
        int cnt = 0;
        if (*done < hl) {
            iov[cnt++] = (struct iovec){ (char *)hdr + *done, hl - *done };
            if (hdr->payload_len) iov[cnt++] = (struct iovec){ (void *)payload, hdr->payload_len };
        } else {
            iov[cnt++] = (struct iovec){ (char *)payload + (*done - hl), total - *done };
        }
        ssize_t w = writev(fd, iov, cnt);
        if (w < 0) {
            if (errno == EINTR) continue;
            return errno == EAGAIN ? 0 : -1;
        }
        *done += (size_t)w;
    }
    return 0;
}

/*
 * 설계 개선의 응답 보내기 (OSP_PIPE_ASYNC=1). 워커는 FIFO 에서 클라이언트를 기다리지 않는다.
 *   나머지가 없으면 막히지 않게 써 보고, 자리가 없어 못 쓴 뒤쪽을 나머지로 옮긴 뒤 송신 스레드에 맡긴다.
 *   나머지가 있으면 순서를 지키려고 그 뒤에 붙인다.
 *   나머지는 클라이언트당 한도(pend_cap_locked) 안에서만 둔다. 넘칠 때는 처음 설계처럼 워커가 자리가 날 때까지
 *   기다린다(동시 요청이 많은 클라이언트가 응답을 읽지 않을 때, 그리고 1단계 시험의 1MB 메시지).
 * srv_resp_mtx[id] 를 잡고 부른다. 성공(보냈거나 맡겼다) 0, 실패 -1.
 */
static int send_resp_async_locked(uint32_t id, const msg_hdr_t *hdr, const void *payload)
{
    size_t total = sizeof *hdr + hdr->payload_len;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (srv_resp_fd[id] < 0) {
            srv_resp_fd[id] = open_resp(id);
            if (srv_resp_fd[id] < 0) return -1;
        }
        pend_t *p = &srv_pend[id];
        uint32_t gen = p->gen;
        /* 앞선 나머지 뒤에 붙일 자리가 없으면 송신 스레드가 비울 때까지 기다린다 (종료를 놓치지 않게 0.05초마다 본다) */
        while (p->len > 0 && p->len + total > pend_cap_locked(id) && p->gen == gen && !atomic_load(&srv_stopping)) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            ts.tv_nsec += 50 * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&srv_pend_cv[id], &srv_resp_mtx[id], &ts);
        }
        if (p->gen != gen || atomic_load(&srv_stopping)) { errno = EPIPE; return -1; }
        if (p->len > 0) {                                       /* 순서를 지키려고 뒤에 붙인다 */
            pend_put(id, hdr, payload, 0);
            pend_arm(id);
            return 0;
        }
        if (srv_grow) fit_resp_locked(id, total);               /* 나머지가 없을 때만 키운다 */
        size_t cap = pend_cap_locked(id), done = 0;
        int fd = srv_resp_fd[id];
        int rc = write_from(fd, hdr, payload, &done);
        while (rc == 0 && total - done > cap) {                 /* 나머지가 한도를 넘는다: 자리가 날 때까지 기다리며 쓴다 */
            if (atomic_load(&srv_stopping)) { errno = EPIPE; rc = -1; break; }
            struct pollfd pf = { .fd = fd, .events = POLLOUT };
            (void)poll(&pf, 1, 50);
            rc = write_from(fd, hdr, payload, &done);
        }
        if (rc == 0) {
            if (done < total) {                                 /* 못 쓴 뒤쪽을 송신 스레드에 맡긴다 */
                pend_put(id, hdr, payload, done);
                pend_arm(id);                                   /* 송신 스레드의 epoll 에 EPOLLOUT 을 건다 */
            }
            return 0;
        }
        int e = errno;
        drop_resp_locked(id);                                   /* 중간까지 쓴 스트림은 이어 쓸 수 없다 */
        errno = e;
        if (!(e == EPIPE && done == 0)) return -1;              /* 처음 설계와 같은 재시도 (아래 tp_send_resp) */
    }
    return -1;
}

int tp_send_resp(const msg_hdr_t *hdr, const void *payload)
{
    uint32_t id = hdr->client_id;
    if (id >= MAX_CLIENTS) { errno = EINVAL; return -1; }

    int rc = -1;
    pthread_mutex_lock(&srv_resp_mtx[id]);
    if (srv_async) {
        rc = send_resp_async_locked(id, hdr, payload);
        int e = errno;
        pthread_mutex_unlock(&srv_resp_mtx[id]);
        errno = e;
        return rc;
    }

    /*
     * 예전 클라이언트가 나간 뒤 같은 번호의 새 클라이언트가 FIFO 를 다시 만든 경우,
     * 열어 둔 fd 는 지워진 예전 FIFO 를 가리켜 EPIPE 가 난다. 한 바이트도 못 썼을 때만 새로 열어 한 번 더 보낸다.
     */
    for (int attempt = 0; attempt < 2; attempt++) {
        if (srv_resp_fd[id] < 0) {
            srv_resp_fd[id] = open_resp(id);
            if (srv_resp_fd[id] < 0) break;
        }
        if (srv_grow) fit_resp_locked(id, sizeof *hdr + hdr->payload_len);
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

    /* 설계 개선: 송신 스레드가 남은 나머지를 쓰게 잠깐 두고 끝낸다 (응답을 다 받은 클라이언트에게는 남은 것이 없다) */
    if (srv_async) async_stop();

    /*
     * 정상 종료에서는 송신하던 워커가 모두 빠져나온 뒤라 락이 늘 비어 있다(transport.h 의 약속).
     * 서버가 두 번째 신호로 바로 끝날 때는 멈춘 클라이언트에게 write 하며 락을 쥔 워커가 남아 있을 수 있다.
     * 그 락을 기다리면 닫기도 함께 멈추므로, 잡히지 않는 번호는 건너뛴다. 곧 _exit 하므로 그 fd 는 커널이 닫는다.
     */
    for (uint32_t i = 0; i < MAX_CLIENTS; i++) {
        if (pthread_mutex_trylock(&srv_resp_mtx[i]) != 0) continue;
        drop_resp_locked(i);             /* 클라이언트의 수신은 EOF 를 받고 끝난다 */
        pthread_mutex_unlock(&srv_resp_mtx[i]);
        pthread_mutex_destroy(&srv_resp_mtx[i]);
    }
    close(srv_req_fd);  srv_req_fd = -1;
    close(srv_wake_fd); srv_wake_fd = -1;
    close(srv_lock_fd); srv_lock_fd = -1;
    if (srv_async) {
        close(srv_ep);   srv_ep = -1;
        close(srv_evfd); srv_evfd = -1;
        srv_async = false;
    }
    srv_opened = 0;
}

/* 요청 FIFO 와 연 응답 FIFO 들의 버퍼 크기(F_GETPIPE_SZ). 응답 FIFO 는 그 번호의 송신 락을 잡고 읽는다 */
void tp_server_info(tp_info_t *out)
{
    memset(out, 0, sizeof *out);
    if (!srv_opened) return;
    int sz = fcntl(srv_req_fd, F_GETPIPE_SZ);
    if (sz > 0) out->cap_bytes += (uint64_t)sz;
    for (uint32_t i = 0; i < MAX_CLIENTS; i++) {
        if (pthread_mutex_trylock(&srv_resp_mtx[i]) != 0) continue;     /* 보내는 중이면 건너뛴다 */
        if (srv_resp_fd[i] >= 0 && (sz = fcntl(srv_resp_fd[i], F_GETPIPE_SZ)) > 0) {
            out->cap_bytes += (uint64_t)sz;
            out->nchan++;
            if (out->min_chan == 0 || (uint32_t)sz < out->min_chan) out->min_chan = (uint32_t)sz;
        }
        pthread_mutex_unlock(&srv_resp_mtx[i]);
    }
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
 *   5. 첫 응답 전에는 keeper 가 쓰는 쪽으로 남아 있어 서버가 죽어도 EOF 가 오지 않는다. 그동안은 poll 로
 *      요청 FIFO 의 쓰기 끝도 함께 본다. 읽는 쪽(서버)이 모두 사라지면 커널이 쓰기 끝에 POLLERR 을 올린다.
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

    /* 잠금 파일은 서버가 요청 FIFO 보다 먼저 만든다. 시작 잠금을 쥐고 내 응답 FIFO 를 만들어 읽기 끝까지 연다 */
    cli_lock_fd = open(lp, O_RDWR);
    if (cli_lock_fd < 0) {
        if (errno == ENOENT) errno = ECONNREFUSED;
        return -1;
    }
    /* 서버가 적어 둔 설계 개선 설정. 처음 설계의 서버(설정 없음)면 읽은 바이트가 모자라 끈다 */
    conf_t conf;
    cli_rbuf_on = pread(cli_lock_fd, &conf, sizeof conf, CONF_OFF) == (ssize_t)sizeof conf && conf.rbuf == 1;
    cli_rb.off = cli_rb.len = 0;
    if (start_lock(cli_lock_fd) < 0 || make_fifo(mp) < 0) {
        /* 같은 번호가 실행 중이면 EADDRINUSE. 그 클라이언트의 FIFO 이므로 지우지 않는다. 닫으면 flock 도 풀린다 */
        int e = errno;
        close(cli_lock_fd);
        cli_lock_fd = -1;
        errno = e;
        return -1;
    }

    int keeper = -1;
    cli_resp_fd = open(mp, O_RDONLY | O_NONBLOCK);
    if (cli_resp_fd < 0) goto fail;
    start_unlock(cli_lock_fd);                   /* 읽는 쪽이 생겼다. 같은 번호로 뒤에 오는 쪽은 EADDRINUSE 로 판정한다 */
    keeper = open(mp, O_WRONLY | O_NONBLOCK);
    if (keeper < 0) goto fail;
    if (clear_nonblock(cli_resp_fd) < 0) goto fail;

    cli_req_fd = open(rp, O_WRONLY | O_NONBLOCK);
    if (cli_req_fd < 0) {
        if (errno == ENXIO || errno == ENOENT) errno = ECONNREFUSED;
        goto fail;
    }
    if (clear_nonblock(cli_req_fd) < 0) goto fail;

    atomic_store(&cli_keeper_fd, keeper);
    cli_id = client_id;
    cli_opened = 1;
    return 0;

fail:;
    int e = errno;
    /*
     * 이름을 먼저 지운다 (tp_client_close 와 같은 순서). 읽기 끝을 닫은 뒤에 지우면 그 사이에 같은 번호로 뜬 쪽이
     * 이 FIFO 를 남은 파일로 보고 새로 만든 것을 지우게 된다. 시작 잠금을 아직 쥐고 있으면 닫을 때 풀린다.
     */
    unlink(mp);
    if (keeper >= 0) close(keeper);
    if (cli_resp_fd >= 0) { close(cli_resp_fd); cli_resp_fd = -1; }
    if (cli_req_fd >= 0)  { close(cli_req_fd);  cli_req_fd = -1; }
    close(cli_lock_fd); cli_lock_fd = -1;
    errno = e;
    return -1;
}

int tp_send_req(const msg_hdr_t *hdr, const void *payload)
{
    /* 서버가 죽었으면 읽는 쪽이 없어 EPIPE 로 실패한다 (SIGPIPE 는 무시함) */
    return send_on_req(cli_req_fd, cli_lock_fd, hdr, payload);
}

/*
 * 첫 응답이 올 때까지 기다린다 (위 tp_client_open 의 5). 응답이 읽을 수 있게 되면 keeper 를 닫고 0.
 * 그 전에 서버가 죽으면 -1, errno = ECONNRESET.
 * 응답이 와 있다는 것은 서버가 쓰기 끝을 열었다는 뜻이라, 이때 keeper 를 닫아도 EOF 가 먼저 오지 않는다.
 * 서버가 쓰고 곧바로 닫았더라도 쓴 데이터는 남아 있어 먼저 읽힌다.
 * DECISION: poll 은 WAIT_MS 마다 다시 부른다. 커널이 읽는 쪽이 닫힐 때 쓰기 쪽을 깨우지만 그것에 기대지 않는다.
 *   첫 응답 전에만 쓰므로 측정 구간의 수신에는 poll 이 끼지 않는다.
 */
#define WAIT_MS 200

static int wait_first_resp(void)
{
    struct pollfd p[2] = {
        { .fd = cli_resp_fd, .events = POLLIN },
        { .fd = cli_req_fd,  .events = 0 },     /* POLLERR 은 요청하지 않아도 돌아온다 */
    };
    for (;;) {
        int r = poll(p, 2, WAIT_MS);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (p[0].revents) break;                /* 응답이 왔다 (닫힌 fd 이면 POLLNVAL, 아래 read 가 실패한다) */
        if (p[1].revents & (POLLERR | POLLHUP | POLLNVAL)) { errno = ECONNRESET; return -1; }
    }
    int k = atomic_exchange(&cli_keeper_fd, -1);
    if (k >= 0) close(k);
    return 0;
}

/* 응답 1개 받기. 수신 스레드 하나만 부른다. 서버가 끝나면 -1, errno = ECONNRESET */
ssize_t tp_recv_resp(msg_hdr_t *hdr, void *payload, size_t cap)
{
    if (atomic_load(&cli_keeper_fd) >= 0 && cli_rb.len == 0 && wait_first_resp() < 0) return -1;
    ssize_t n = cli_rbuf_on ? recv_msg_rb(cli_resp_fd, &cli_rb, hdr, payload, cap)
                            : recv_msg(cli_resp_fd, hdr, payload, cap);
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
