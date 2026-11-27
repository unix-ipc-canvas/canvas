/*
 * transport_mq.c - System V message queue 로 구현한 전송 계층
 *
 * 채널 (docs/design/README.md#채널)
 *   요청 채널  key IPC_KEY(0) 큐 1개     모든 클라이언트 → 서버 (N:1)
 *   응답 채널  key IPC_KEY(1) 큐 1개     서버 → 클라이언트. mtype = client_id + 1 로 받는 쪽을 나눈다
 *   잠금 파일  /tmp/osproject_<팀>_mq.lock  서버와 클라이언트가 살아 있는지 알리는 표시
 *
 * 조각
 *   System V 메시지 하나는 msgmax(기본 8192B)를 넘지 못한다. 그래서 메시지 하나(msg_hdr_t + payload)를
 *   조각 여러 개로 나눠 보내고 받는 쪽에서 다시 붙인다. 조각마다 앞에 8바이트 머리(종류)가 붙는다.
 *     첫 조각   FRAG_FIRST : msg_hdr_t + payload 앞부분
 *     이어지는  FRAG_CONT  : payload 나머지
 *   조각 크기는 열 때 /proc/sys/kernel/msgmax 와 큐의 msg_qbytes 를 읽어 정한다.
 *   한도를 올리고 서버를 다시 띄우면 코드를 고치지 않고 큰 조각으로 잴 수 있다.
 *
 * 섞임 방지
 *   - 응답은 클라이언트별 mutex 를 잡고 조각을 모두 보낸다. 같은 클라이언트로 가는 두 응답의 조각이 섞이지 않는다.
 *   - 요청은 여러 클라이언트의 조각이 한 큐에 섞여 들어온다. mtype(= client_id + 1)으로 보낸 쪽을 가려
 *     서버가 클라이언트별로 따로 붙인다.
 *
 * 살아 있는지 확인
 *   메시지 큐는 상대가 죽어도 알려 주지 않는다. 잠금 파일의 바이트마다 OFD 잠금을 걸어 표시한다.
 *     바이트 0        서버
 *     바이트 1 + id   클라이언트 id
 *   프로세스가 죽으면 커널이 잠금을 풀어 주므로 kill -9 뒤에도 표시가 남지 않는다.
 *
 * DECISION: 응답 큐는 클라이언트마다 두지 않고 하나를 mtype 으로 나눈다. key 가 60020~60029 의 10개뿐이라
 *   클라이언트 16개에 큐를 하나씩 줄 수 없다 (shm 버전이 4개를 쓴다).
 * DECISION: msgsnd 는 mtype 이 앞에 붙은 버퍼를 요구하므로 보낼 때와 받을 때 조각 버퍼를 거쳐 한 번씩 복사한다.
 */
#define _GNU_SOURCE             /* F_OFD_SETLK, F_OFD_GETLK */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "transport.h"

#define KEY_REQ   IPC_KEY(0)
#define KEY_RESP  IPC_KEY(1)

#define PATH_LEN 64
#define DEFAULT_MSGMAX 8192u

enum { FRAG_FIRST = 1, FRAG_CONT = 2, FRAG_WAKE = 3 };

/* 큐에 들어가는 조각 하나. msgsnd/msgrcv 의 크기 인자는 mtype 을 뺀 길이다 */
typedef struct {
    long     mtype;
    uint32_t kind;
    uint32_t reserved;
    char     data[];
} frag_t;

#define FRAG_HDR  (offsetof(frag_t, data) - sizeof(long))       /* 조각 머리 8바이트 */
#define FRAG_MAX  (FRAG_HDR + sizeof(msg_hdr_t) + MAX_PAYLOAD)   /* 조각 하나로 다 보낼 때의 크기 */
#define FRAG_MIN  (FRAG_HDR + sizeof(msg_hdr_t) + 1)

#define MTYPE_OF(id) ((long)(id) + 1)
#define MTYPE_WAKE   ((long)MAX_CLIENTS + 1)

#define LOCK_SERVER      ((off_t)0)
#define LOCK_CLIENT(id)  ((off_t)(id) + 1)

static void lock_path(char *buf) { snprintf(buf, PATH_LEN, "/tmp/osproject_%d_mq.lock", TEAM_NO); }

/* ---- 잠금 파일 ---- */

/* 바이트 하나에 쓰기 잠금을 건다. 다른 쪽이 잡고 있으면 기다리지 않고 -1 (EAGAIN 또는 EACCES) */
static int lock_byte(int fd, off_t byte)
{
    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = byte, .l_len = 1 };
    return fcntl(fd, F_OFD_SETLK, &fl);
}

/* 다른 쪽이 그 바이트를 잡고 있는지 본다. 1 = 잡고 있음, 0 = 없음, -1 = 확인 실패 */
static int byte_locked(int fd, off_t byte)
{
    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = byte, .l_len = 1 };
    if (fcntl(fd, F_OFD_GETLK, &fl) < 0) return -1;
    return fl.l_type != F_UNLCK;
}

/* ---- 한도와 버퍼 ---- */

/* 조각 하나의 최대 크기(mtype 제외). 받는 쪽 버퍼는 이 크기로 잡는다 */
static size_t frag_limit(void)
{
    unsigned long v = 0;
    FILE *f = fopen("/proc/sys/kernel/msgmax", "r");
    if (f) {
        if (fscanf(f, "%lu", &v) != 1) v = 0;
        fclose(f);
    }
    if (v == 0) v = DEFAULT_MSGMAX;
    return v < FRAG_MAX ? (size_t)v : FRAG_MAX;
}

/* 큐에 보낼 조각 크기. 큐 용량(msg_qbytes)보다 큰 조각은 영원히 들어가지 못한다 */
static int send_chunk(int qid, size_t limit, size_t *chunk)
{
    struct msqid_ds ds;
    if (msgctl(qid, IPC_STAT, &ds) < 0) return -1;
    *chunk = limit < ds.msg_qbytes ? limit : (size_t)ds.msg_qbytes;
    if (*chunk < FRAG_MIN) { errno = ENOSPC; return -1; }
    return 0;
}

static frag_t *frag_alloc(size_t chunk) { return malloc(sizeof(long) + chunk); }

/* ---- 조각 보내기 ---- */

/*
 * 조각 하나를 큐에 넣는다. 큐가 가득 차 있으면 자리가 날 때까지 기다린다.
 * dead 는 감시 스레드가 "받는 쪽이 죽었다"고 알리는 표시다 (없으면 NULL). 켜져 있으면 -1, errno = EPIPE.
 */
static int put_frag(int qid, const frag_t *f, size_t len, const atomic_bool *dead)
{
    for (;;) {
        if (dead && atomic_load(dead)) { errno = EPIPE; return -1; }
        if (msgsnd(qid, f, len, 0) == 0) return 0;
        if (errno == EINTR) continue;
        if (errno == EIDRM || errno == EINVAL) errno = EPIPE;   /* 큐가 지워졌다 = 서버가 끝났다 */
        return -1;
    }
}

/* 메시지 하나를 조각으로 나눠 보낸다. f 는 chunk 바이트를 담을 수 있는 조각 버퍼다 */
static int send_msg(int qid, long mtype, frag_t *f, size_t chunk,
                    const msg_hdr_t *hdr, const void *payload, const atomic_bool *dead)
{
    const char *p = payload;
    size_t left = hdr->payload_len;
    size_t room = chunk - FRAG_HDR;

    f->mtype = mtype;
    f->kind = FRAG_FIRST;
    f->reserved = 0;
    memcpy(f->data, hdr, sizeof *hdr);
    size_t k = left < room - sizeof *hdr ? left : room - sizeof *hdr;
    if (k) memcpy(f->data + sizeof *hdr, p, k);
    size_t len = sizeof *hdr + k;

    for (;;) {
        if (put_frag(qid, f, FRAG_HDR + len, dead) < 0) return -1;
        p += k;
        left -= k;
        if (left == 0) return 0;
        f->kind = FRAG_CONT;
        k = left < room ? left : room;
        memcpy(f->data, p, k);
        len = k;
    }
}

/* 큐에 남은 mtype 조각을 모두 꺼내 버린다 */
static void purge(int qid, long mtype, frag_t *buf, size_t cap)
{
    while (msgrcv(qid, buf, cap, mtype, IPC_NOWAIT | MSG_NOERROR) >= 0)
        ;
}

/* ---- 감시 스레드 ---- */

/*
 * 메시지 큐에서 기다리는 msgsnd, msgrcv 는 상대가 죽어도 깨어나지 않는다.
 * 감시 스레드가 WATCH_MS 마다 잠금 파일을 보고, 상대가 죽었으면 기다리는 쪽을 깨운다.
 *   서버     죽은 클라이언트의 응답 조각을 큐에서 꺼내 버린다. 그 클라이언트에게 보내던 워커가 EPIPE 로 빠져나온다.
 *   클라이언트  서버가 죽었으면 남은 큐 2개를 지운다. 기다리던 송신과 수신이 EPIPE, ECONNRESET 으로 끝난다.
 * DECISION: 주기는 200ms 다. 한 번에 시스템 콜 몇 개라서 측정에 주는 영향은 무시할 만하고,
 *   상대가 죽은 것을 그만큼 늦게 안다. 정상 실행에서는 아무것도 지우지 않는다.
 */
#define WATCH_MS 200

typedef struct {
    pthread_t       th;
    pthread_mutex_t mtx;
    pthread_cond_t  cond;
    bool            stop;
    bool            running;
    bool          (*tick)(void);     /* false 를 돌려주면 감시를 끝낸다 */
} watch_t;

static void *watch_main(void *arg)
{
    watch_t *w = arg;
    pthread_mutex_lock(&w->mtx);
    while (!w->stop) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += WATCH_MS * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&w->cond, &w->mtx, &ts);
        if (w->stop) break;
        pthread_mutex_unlock(&w->mtx);
        bool more = w->tick();
        pthread_mutex_lock(&w->mtx);
        if (!more) break;
    }
    pthread_mutex_unlock(&w->mtx);
    return NULL;
}

static int watch_start(watch_t *w, bool (*tick)(void))
{
    pthread_mutex_init(&w->mtx, NULL);
    pthread_cond_init(&w->cond, NULL);
    w->stop = false;
    w->tick = tick;

    /* 신호는 응용이 정한 스레드가 받는다. 감시 스레드는 모두 막고 시작한다 */
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    int rc = pthread_create(&w->th, NULL, watch_main, w);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&w->mtx);
        pthread_cond_destroy(&w->cond);
        errno = rc;
        return -1;
    }
    w->running = true;
    return 0;
}

static void watch_stop(watch_t *w)
{
    if (!w->running) return;
    pthread_mutex_lock(&w->mtx);
    w->stop = true;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->mtx);
    pthread_join(w->th, NULL);
    pthread_mutex_destroy(&w->mtx);
    pthread_cond_destroy(&w->cond);
    w->running = false;
}

const char *tp_name(void) { return "mq"; }

/* fork 로 함께 띄울 때의 준비. 메시지 큐는 key 로 만나므로 할 일이 없다 */
int  tp_prefork(void)  { return 0; }
void tp_postfork(void) { }

/* ================================================================
 * 서버 쪽
 * ================================================================ */

static int srv_req_q = -1;
static int srv_resp_q = -1;
static int srv_lock_fd = -1;
static size_t srv_limit;                         /* 받는 조각 버퍼 크기 */
static size_t srv_chunk;                         /* 응답 큐에 보내는 조각 크기 */
static frag_t *srv_rbuf;                         /* 수신 스레드의 조각 버퍼 */
static char *srv_sbuf;                           /* 클라이언트별 송신 조각 버퍼 (srv_resp_mtx 가 보호) */
static size_t srv_sbuf_stride;
static pthread_mutex_t srv_resp_mtx[MAX_CLIENTS];
static atomic_bool srv_stopping;
static atomic_bool srv_opened;
/*
 * 깨우기(tp_server_wake)와 닫기(tp_server_close)를 한 번에 하나만 하게 한다.
 * 수신 스레드는 깨우기 메시지 없이 srv_stopping 만 보고 먼저 끝날 수 있다. 그러면 깨우는 스레드가 아직 msgsnd 앞에
 * 있는 동안 main 이 닫기까지 가서, 깨우기가 읽는 srv_req_q 를 닫기가 동기화 없이 -1 로 바꿨다(TSan 이 보고한 경쟁).
 * 닫기는 이 mutex 안에서 srv_opened 를 내리므로, 그 뒤의 깨우기는 큐를 건드리지 않는다.
 * 깨우기는 IPC_NOWAIT 로 보내 쥔 채 막히지 않는다.
 */
static pthread_mutex_t srv_wake_mtx = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool srv_sent[MAX_CLIENTS];        /* 응답을 보낸 적이 있다. 감시 스레드가 볼 대상 */
static atomic_bool srv_dead[MAX_CLIENTS];        /* 감시 스레드가 죽었다고 본 클라이언트 */
static watch_t srv_watch;

/*
 * 여러 조각으로 온 요청을 클라이언트별로 붙이는 자리.
 * DECISION: 클라이언트마다 요청 최대 크기를 한 번에 잡아 둔다. 측정 구간 안에서 malloc 을 부르지 않기 위해서다.
 *   쓰지 않은 페이지는 실제 메모리를 차지하지 않는다. 1단계 벤치(tp_set_limits 를 부르지 않음)는 클라이언트 16개 ×
 *   MAX_PAYLOAD = 16MB 다. 2단계 응용은 요청이 모두 조각 하나라 이 자리를 잡지 않는다(11/27 재수행, 클라이언트 1024개).
 */
static uint32_t lim_clients = 16;                /* tp_set_limits. 부르지 않으면 1단계 값 */
static size_t   lim_req = sizeof(msg_hdr_t) + MAX_PAYLOAD;
static size_t   srv_asm_stride;                  /* 클라이언트 하나의 붙이는 자리. 0 이면 여러 조각 요청을 받지 않는다 */
static char *srv_asm_buf;
static struct {
    msg_hdr_t hdr;
    size_t    got;
    bool      active;
} srv_asm[MAX_CLIENTS];

static frag_t *srv_sbuf_of(uint32_t id) { return (frag_t *)(srv_sbuf + (size_t)id * srv_sbuf_stride); }

int tp_set_limits(uint32_t max_clients, size_t max_req, size_t max_resp)
{
    if (max_clients < 1 || max_clients > MAX_CLIENTS || max_req < sizeof(msg_hdr_t) || max_resp < sizeof(msg_hdr_t) ||
        max_req > sizeof(msg_hdr_t) + MAX_PAYLOAD) {
        errno = EINVAL;
        return -1;
    }
    lim_clients = max_clients;
    lim_req = max_req;
    return 0;
}

/*
 * 잠금 파일을 열고 서버 자리를 잡는다. 다른 서버가 살아 있으면 -1, errno = EADDRINUSE.
 * 끝나 가는 서버가 파일을 지우는 순간과 겹치면 지워진 파일에 잠금을 걸 수 있다. 이름이 같은 파일인지 확인하고 다시 한다.
 */
static int server_lock(void)
{
    char lp[PATH_LEN];
    lock_path(lp);
    for (int attempt = 0; attempt < 8; attempt++) {
        int fd = open(lp, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0) return -1;
        if (lock_byte(fd, LOCK_SERVER) < 0) {
            int e = (errno == EAGAIN || errno == EACCES) ? EADDRINUSE : errno;
            close(fd);
            errno = e;
            return -1;
        }
        struct stat a, b;
        if (fstat(fd, &a) == 0 && stat(lp, &b) == 0 && a.st_ino == b.st_ino && a.st_dev == b.st_dev)
            return fd;
        close(fd);
    }
    errno = EAGAIN;
    return -1;
}

/*
 * 큐를 새로 만든다. 서버 자리를 잡은 뒤에만 부른다.
 * 같은 key 의 큐가 있으면 비정상 종료한 우리 서버가 남긴 것이므로 지우고 다시 만든다.
 */
static int make_queue(key_t key)
{
    int q = msgget(key, 0);
    if (q >= 0) {
        if (msgctl(q, IPC_RMID, NULL) < 0 && errno != EIDRM && errno != EINVAL) return -1;
    } else if (errno != ENOENT) {
        return -1;
    }
    return msgget(key, IPC_CREAT | IPC_EXCL | 0600);
}

static void server_release(void)
{
    char lp[PATH_LEN];
    lock_path(lp);
    if (srv_req_q >= 0)  { msgctl(srv_req_q, IPC_RMID, NULL);  srv_req_q = -1; }
    if (srv_resp_q >= 0) { msgctl(srv_resp_q, IPC_RMID, NULL); srv_resp_q = -1; }
    if (srv_lock_fd >= 0) {
        unlink(lp);                      /* 잠금을 쥔 채로 이름부터 지운다 */
        close(srv_lock_fd);
        srv_lock_fd = -1;
    }
    free(srv_rbuf);    srv_rbuf = NULL;
    free(srv_sbuf);    srv_sbuf = NULL;
    free(srv_asm_buf); srv_asm_buf = NULL;
}

/*
 * 서버 감시: 응답을 보낸 적이 있는 클라이언트가 죽었으면 그 클라이언트의 조각을 큐에서 꺼내 버린다.
 * 죽은 클라이언트의 조각이 큐 용량을 차지하면 살아 있는 클라이언트로 가는 응답까지 모두 막힌다.
 *
 * 같은 번호의 새 클라이언트가 받을 응답을 지우면 안 된다. 그 클라이언트로 보내는 워커가 없을 때는
 * 송신 mutex 를 잡고 지우므로 안전하다. 워커가 보내다 막혀 있을 때는 mutex 를 잡을 수 없어 그냥 지운다.
 * 이때 지워지는 것은 죽은 클라이언트에게 가던 조각이고, 새 클라이언트의 응답은 그 워커가 mutex 를 놓은 뒤에야 나간다.
 */
static bool srv_watch_tick(void)
{
    struct { long mtype; char data[16]; } junk;

    for (uint32_t id = 0; id < MAX_CLIENTS; id++) {
        if (!atomic_load(&srv_sent[id])) continue;
        if (byte_locked(srv_lock_fd, LOCK_CLIENT(id)) != 0) continue;

        atomic_store(&srv_dead[id], true);
        bool idle = pthread_mutex_trylock(&srv_resp_mtx[id]) == 0;
        if (byte_locked(srv_lock_fd, LOCK_CLIENT(id)) == 0)
            purge(srv_resp_q, MTYPE_OF(id), (frag_t *)&junk, sizeof junk.data);
        if (idle) {
            atomic_store(&srv_sent[id], false);
            pthread_mutex_unlock(&srv_resp_mtx[id]);
        }
    }
    return true;
}

int tp_server_open(void)
{
    srv_lock_fd = server_lock();         /* 서버가 이미 떠 있으면 EADDRINUSE */
    if (srv_lock_fd < 0) return -1;

    srv_req_q = make_queue(KEY_REQ);
    if (srv_req_q < 0) goto fail;
    srv_resp_q = make_queue(KEY_RESP);
    if (srv_resp_q < 0) goto fail;

    srv_limit = frag_limit();
    if (send_chunk(srv_resp_q, srv_limit, &srv_chunk) < 0) goto fail;

    srv_sbuf_stride = (sizeof(long) + srv_chunk + 15) & ~(size_t)15;
    srv_rbuf = frag_alloc(srv_limit);
    srv_sbuf = malloc(srv_sbuf_stride * MAX_CLIENTS);
    /* 요청이 조각 하나(FRAG_HDR + 머리 + payload <= srv_limit)에 들어가면 붙이는 자리가 필요 없다 */
    srv_asm_stride = FRAG_HDR + lim_req <= srv_limit ? 0 : lim_req - sizeof(msg_hdr_t);
    srv_asm_buf = srv_asm_stride ? malloc((size_t)lim_clients * srv_asm_stride) : NULL;
    if (!srv_rbuf || !srv_sbuf || (srv_asm_stride && !srv_asm_buf)) { errno = ENOMEM; goto fail; }

    for (uint32_t i = 0; i < MAX_CLIENTS; i++) {
        pthread_mutex_init(&srv_resp_mtx[i], NULL);
        srv_asm[i].active = false;
        atomic_store(&srv_sent[i], false);
        atomic_store(&srv_dead[i], false);
    }
    atomic_store(&srv_stopping, false);
    if (watch_start(&srv_watch, srv_watch_tick) < 0) {
        int e = errno;
        for (uint32_t i = 0; i < MAX_CLIENTS; i++) pthread_mutex_destroy(&srv_resp_mtx[i]);
        errno = e;
        goto fail;
    }
    atomic_store(&srv_opened, true);
    return 0;

fail:;
    int e = errno;
    server_release();
    errno = e;
    return -1;
}

/*
 * 요청 1개 받기. 수신 스레드 하나만 부른다.
 * tp_server_wake 로 깨워지거나 큐가 지워지면 -1, errno = ECANCELED.
 * cap 보다 큰 요청은 버리고 -1, errno = EMSGSIZE.
 */
ssize_t tp_recv_req(msg_hdr_t *hdr, void *payload, size_t cap)
{
    for (;;) {
        if (atomic_load(&srv_stopping)) { errno = ECANCELED; return -1; }

        ssize_t n = msgrcv(srv_req_q, srv_rbuf, srv_limit, 0, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EIDRM || errno == EINVAL) errno = ECANCELED;
            return -1;
        }
        if (srv_rbuf->mtype == MTYPE_WAKE) { errno = ECANCELED; return -1; }
        if (srv_rbuf->mtype < 1 || srv_rbuf->mtype > (long)MAX_CLIENTS) continue;
        if ((size_t)n < FRAG_HDR) continue;

        uint32_t id = (uint32_t)(srv_rbuf->mtype - 1);
        size_t dlen = (size_t)n - FRAG_HDR;
        const char *data = srv_rbuf->data;

        if (srv_rbuf->kind == FRAG_FIRST) {
            /* 붙이던 요청이 있었다면 보낸 쪽이 중간에 죽은 것이다. 버리고 새로 시작한다 */
            srv_asm[id].active = false;
            if (dlen < sizeof *hdr) continue;

            msg_hdr_t h;
            memcpy(&h, data, sizeof h);
            data += sizeof h;
            dlen -= sizeof h;
            if (dlen > h.payload_len) continue;

            if (h.payload_len > cap || h.payload_len > MAX_PAYLOAD ||
                (dlen < h.payload_len && (id >= lim_clients || h.payload_len > srv_asm_stride))) {
                /* 이어지는 조각은 붙이는 중이 아니므로 아래에서 버려진다 */
                *hdr = h;
                errno = EMSGSIZE;
                return -1;
            }
            if (dlen == h.payload_len) {             /* 조각 하나로 끝나는 요청 (응용의 모든 요청) */
                *hdr = h;
                if (dlen) memcpy(payload, data, dlen);
                return (ssize_t)dlen;
            }
            srv_asm[id].hdr = h;
            srv_asm[id].got = dlen;
            srv_asm[id].active = true;
            memcpy(srv_asm_buf + (size_t)id * srv_asm_stride, data, dlen);
        } else if (srv_rbuf->kind == FRAG_CONT) {
            if (!srv_asm[id].active) continue;       /* 첫 조각을 못 받은 조각은 버린다 */

            size_t total = srv_asm[id].hdr.payload_len;
            if (dlen > total - srv_asm[id].got) { srv_asm[id].active = false; continue; }

            char *buf = srv_asm_buf + (size_t)id * srv_asm_stride;
            memcpy(buf + srv_asm[id].got, data, dlen);
            srv_asm[id].got += dlen;
            if (srv_asm[id].got < total) continue;

            srv_asm[id].active = false;
            *hdr = srv_asm[id].hdr;
            if (total > cap) { errno = EMSGSIZE; return -1; }
            memcpy(payload, buf, total);
            return (ssize_t)total;
        }
    }
}

/*
 * hdr->client_id 에게 응답을 보낸다. 여러 워커가 동시에 불러도 된다.
 * 그 클라이언트가 죽었으면 -1, errno = EPIPE. 이미 넣은 조각은 큐에서 꺼내 버린다.
 */
int tp_send_resp(const msg_hdr_t *hdr, const void *payload)
{
    uint32_t id = hdr->client_id;
    if (id >= MAX_CLIENTS) { errno = EINVAL; return -1; }
    if (!atomic_load(&srv_opened)) { errno = EPIPE; return -1; }

    pthread_mutex_lock(&srv_resp_mtx[id]);
    frag_t *f = srv_sbuf_of(id);
    int rc = -1;

    atomic_store(&srv_sent[id], true);
    if (atomic_load(&srv_dead[id])) {
        /* 죽었다고 본 번호다. 같은 번호의 새 클라이언트가 들어왔는지 직접 확인한다 */
        if (byte_locked(srv_lock_fd, LOCK_CLIENT(id)) == 1) atomic_store(&srv_dead[id], false);
        else { errno = EPIPE; goto out; }
    }
    rc = send_msg(srv_resp_q, MTYPE_OF(id), f, srv_chunk, hdr, payload, &srv_dead[id]);
    if (rc < 0 && errno == EPIPE && atomic_load(&srv_opened)) {
        /* 보내다 만 조각을 치운다. mutex 를 잡고 있어서 새 클라이언트의 응답은 아직 큐에 없다 */
        purge(srv_resp_q, MTYPE_OF(id), f, srv_chunk);
        errno = EPIPE;
    }
out:
    pthread_mutex_unlock(&srv_resp_mtx[id]);
    return rc;
}

/*
 * 막혀 있는 tp_recv_req 를 깨운다. 요청 큐에 깨우기 조각 하나를 넣는다.
 * 큐가 가득 차 넣지 못해도 된다. 그때는 수신 스레드가 막혀 있지 않고, 다음 호출에서 srv_stopping 을 본다.
 */
void tp_server_wake(void)
{
    pthread_mutex_lock(&srv_wake_mtx);         /* 닫기와 겹치지 않게 (srv_wake_mtx 설명) */
    if (atomic_load(&srv_opened)) {
        atomic_store(&srv_stopping, true);

        struct { long mtype; uint32_t kind; uint32_t reserved; } w = { MTYPE_WAKE, FRAG_WAKE, 0 };
        while (msgsnd(srv_req_q, &w, FRAG_HDR, IPC_NOWAIT) < 0 && errno == EINTR)
            ;
    }
    pthread_mutex_unlock(&srv_wake_mtx);
}

void tp_server_close(void)
{
    pthread_mutex_lock(&srv_wake_mtx);         /* 진행 중인 깨우기가 끝난 뒤에 닫는다 */
    bool was_open = atomic_exchange(&srv_opened, false);
    pthread_mutex_unlock(&srv_wake_mtx);
    if (!was_open) return;
    watch_stop(&srv_watch);

    /*
     * 큐를 먼저 지운다. 막혀 있던 msgsnd, msgrcv 가 모두 EIDRM 으로 깨어난다.
     * 클라이언트의 tp_recv_resp 는 ECONNRESET, tp_send_req 는 EPIPE 로 끝난다.
     */
    msgctl(srv_req_q, IPC_RMID, NULL);
    msgctl(srv_resp_q, IPC_RMID, NULL);

    for (uint32_t i = 0; i < MAX_CLIENTS; i++) {
        pthread_mutex_lock(&srv_resp_mtx[i]);        /* 보내던 워커가 빠져나오기를 기다린다 */
        pthread_mutex_unlock(&srv_resp_mtx[i]);
        pthread_mutex_destroy(&srv_resp_mtx[i]);
    }
    srv_req_q = -1;
    srv_resp_q = -1;
    server_release();
}

/* 요청·응답 큐의 용량(msg_qbytes). 응답 채널은 큐 하나를 모든 클라이언트가 나눠 쓴다 */
void tp_server_info(tp_info_t *out)
{
    memset(out, 0, sizeof *out);
    if (!atomic_load(&srv_opened)) return;
    struct msqid_ds ds;
    if (msgctl(srv_req_q, IPC_STAT, &ds) == 0) out->cap_bytes += ds.msg_qbytes;
    if (msgctl(srv_resp_q, IPC_STAT, &ds) == 0) {
        out->cap_bytes += ds.msg_qbytes;
        out->nchan = 1;
        out->min_chan = (uint32_t)ds.msg_qbytes;
    }
}

/* ================================================================
 * 클라이언트 쪽
 * ================================================================ */

static int cli_req_q = -1;
static int cli_resp_q = -1;
static int cli_lock_fd = -1;
static uint32_t cli_id;
static size_t cli_limit;
static size_t cli_chunk;                         /* 요청 큐에 보내는 조각 크기 */
static frag_t *cli_sbuf;                         /* cli_send_mtx 가 보호 */
static frag_t *cli_rbuf;
static pthread_mutex_t cli_send_mtx = PTHREAD_MUTEX_INITIALIZER;
static int cli_opened;
static watch_t cli_watch;

/*
 * 클라이언트 감시: 서버가 큐를 지우지 못하고 죽었으면(kill -9) 남은 큐를 지운다.
 * 이 클라이언트와 다른 클라이언트들의 기다리던 송신·수신이 모두 깨어난다.
 * 열 때 받아 둔 큐 번호로만 지운다. 그 뒤에 새로 뜬 서버의 큐는 번호가 달라 건드리지 않는다.
 */
static bool cli_watch_tick(void)
{
    if (byte_locked(cli_lock_fd, LOCK_SERVER) != 0) return true;
    msgctl(cli_req_q, IPC_RMID, NULL);
    msgctl(cli_resp_q, IPC_RMID, NULL);
    return false;
}

/*
 * 요청 큐와 응답 큐에 연결한다.
 * 서버가 없으면 -1, errno = ECONNREFUSED. 같은 번호의 클라이언트가 실행 중이면 -1, errno = EADDRINUSE.
 */
int tp_client_open(uint32_t client_id)
{
    char lp[PATH_LEN];
    if (client_id >= MAX_CLIENTS) { errno = EINVAL; return -1; }
    lock_path(lp);

    cli_lock_fd = open(lp, O_RDWR | O_CLOEXEC);
    if (cli_lock_fd < 0) {
        if (errno == ENOENT) errno = ECONNREFUSED;
        return -1;
    }
    /* 잠금 파일은 있는데 서버 자리가 비었다 = 서버가 비정상 종료했다 */
    if (byte_locked(cli_lock_fd, LOCK_SERVER) != 1) { errno = ECONNREFUSED; goto fail; }

    if (lock_byte(cli_lock_fd, LOCK_CLIENT(client_id)) < 0) {
        if (errno == EAGAIN || errno == EACCES) errno = EADDRINUSE;
        goto fail;
    }

    cli_req_q = msgget(KEY_REQ, 0);
    cli_resp_q = msgget(KEY_RESP, 0);
    if (cli_req_q < 0 || cli_resp_q < 0) {
        if (errno == ENOENT) errno = ECONNREFUSED;
        goto fail;
    }

    cli_limit = frag_limit();
    if (send_chunk(cli_req_q, cli_limit, &cli_chunk) < 0) {
        if (errno == EIDRM || errno == EINVAL) errno = ECONNREFUSED;
        goto fail;
    }
    cli_sbuf = frag_alloc(cli_chunk);
    cli_rbuf = frag_alloc(cli_limit);
    if (!cli_sbuf || !cli_rbuf) { errno = ENOMEM; goto fail; }

    /* 같은 번호를 쓰던 예전 클라이언트가 받지 않고 간 응답을 버린다 */
    purge(cli_resp_q, MTYPE_OF(client_id), cli_rbuf, cli_limit);

    if (watch_start(&cli_watch, cli_watch_tick) < 0) goto fail;
    cli_id = client_id;
    cli_opened = 1;
    return 0;

fail:;
    int e = errno;
    free(cli_sbuf); cli_sbuf = NULL;
    free(cli_rbuf); cli_rbuf = NULL;
    close(cli_lock_fd); cli_lock_fd = -1;
    cli_req_q = cli_resp_q = -1;
    errno = e;
    return -1;
}

/* 서버가 끝났으면 -1, errno = EPIPE */
int tp_send_req(const msg_hdr_t *hdr, const void *payload)
{
    pthread_mutex_lock(&cli_send_mtx);
    int rc = send_msg(cli_req_q, MTYPE_OF(cli_id), cli_sbuf, cli_chunk, hdr, payload, NULL);
    pthread_mutex_unlock(&cli_send_mtx);
    return rc;
}

/*
 * 응답 1개 받기. 수신 스레드 하나만 부른다.
 * 서버가 끝나면 -1, errno = ECONNRESET. cap 보다 큰 응답은 버리고 -1, errno = EMSGSIZE.
 */
ssize_t tp_recv_resp(msg_hdr_t *hdr, void *payload, size_t cap)
{
    size_t got = 0;
    bool active = false;

    for (;;) {
        ssize_t n = msgrcv(cli_resp_q, cli_rbuf, cli_limit, MTYPE_OF(cli_id), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EIDRM || errno == EINVAL) errno = ECONNRESET;
            return -1;
        }
        if ((size_t)n < FRAG_HDR) continue;

        size_t dlen = (size_t)n - FRAG_HDR;
        const char *data = cli_rbuf->data;

        if (cli_rbuf->kind == FRAG_FIRST) {
            active = false;
            if (dlen < sizeof *hdr) continue;
            memcpy(hdr, data, sizeof *hdr);
            data += sizeof *hdr;
            dlen -= sizeof *hdr;
            if (dlen > hdr->payload_len) continue;
            /* 이어지는 조각은 다음 호출에서 첫 조각이 아니라서 버려진다 */
            if (hdr->payload_len > cap) { errno = EMSGSIZE; return -1; }
            if (dlen) memcpy(payload, data, dlen);
            got = dlen;
            active = true;
        } else if (cli_rbuf->kind == FRAG_CONT) {
            if (!active) continue;
            if (dlen > hdr->payload_len - got) { active = false; continue; }
            memcpy((char *)payload + got, data, dlen);
            got += dlen;
        } else {
            continue;
        }
        if (got == hdr->payload_len) return (ssize_t)got;
    }
}

void tp_client_close(void)
{
    if (!cli_opened) return;
    watch_stop(&cli_watch);

    /* 받지 않은 응답이 큐 용량을 차지하지 않게 버린다. 큐 자체는 서버가 지운다 */
    purge(cli_resp_q, MTYPE_OF(cli_id), cli_rbuf, cli_limit);

    close(cli_lock_fd); cli_lock_fd = -1;            /* 클라이언트 자리 잠금이 풀린다 */
    free(cli_sbuf); cli_sbuf = NULL;
    free(cli_rbuf); cli_rbuf = NULL;
    cli_req_q = cli_resp_q = -1;
    cli_opened = 0;
}
