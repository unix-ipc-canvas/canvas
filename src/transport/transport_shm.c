/*
 * transport_shm.c - System V shared memory + semaphore 로 구현한 전송 계층
 *
 * 채널 (docs/design/README.md#채널)
 *   요청 채널  shm key IPC_KEY(2) 의 링 버퍼 1개, 세마포어 집합 key IPC_KEY(4)     모든 클라이언트 → 서버 (N:1)
 *   응답 채널  shm key IPC_KEY(3) 의 링 버퍼 MAX_CLIENTS 개 (클라이언트마다 1개),
 *             세마포어 집합 key IPC_KEY(5)                                     서버 → 클라이언트 id
 *   잠금 파일  /tmp/osproject_<팀>_shm.lock  서버와 클라이언트가 살아 있는지 알리는 표시 (mq 버전과 같은 방식)
 *
 * 링 버퍼
 *   공유 메모리를 같은 크기의 칸(chunk, 기본 4096B)으로 나눈다. 메시지 하나(msg_hdr_t + payload)는 칸의 처음부터
 *   이어진 칸 k 개를 차지하고, 끝에 닿으면 처음으로 이어진다. 머리(24B)는 항상 첫 칸 안에 있다.
 *   링마다 세마포어 3개를 쓴다.
 *     MUTEX  보내는 쪽끼리 한 번에 하나만 쓰게 한다 (요청 링은 여러 클라이언트, 응답 링은 여러 워커)
 *     FREE   빈 칸 수. 보내는 쪽이 k 를 빼고, 받는 쪽이 다 읽은 뒤 k 를 더한다
 *     MSGS   다 쓴 메시지 수. 보내는 쪽이 1 을 더하고, 받는 쪽이 1 을 빼고 읽는다
 *   보내기  semop {FREE -k, MUTEX -1} 를 한 번에 → 복사 → head += k → semop {MSGS +1, MUTEX +1}
 *   받기    semop {MSGS -1} → 복사 → tail += k → semop {FREE +k}
 *   빈 칸과 MUTEX 를 한 번의 semop 으로 함께 얻으므로, 기다리는 동안에는 아무것도 쥐고 있지 않다.
 *   받는 쪽은 링마다 하나뿐이다(서버 수신 스레드, 클라이언트 수신 스레드). 그래서 tail 에는 잠금이 없다.
 *
 * 살아 있는지 확인
 *   세마포어에서 기다리는 쪽은 상대가 죽어도 깨어나지 않는다. 기다릴 때는 semtimedop 으로 WAIT_MS 마다 깨어나
 *   잠금 파일에서 상대의 바이트가 잡혀 있는지 본다 (바이트 0 서버, 바이트 1 + id 클라이언트 id).
 *   기다리지 않고 바로 통과하는 보통의 송수신에서는 이 확인을 하지 않는다.
 *
 * DECISION: 알림은 System V 세마포어로 한다 (프로세스 공유 mutex + 조건변수와 비교해 정함).
 *   - 빈 칸 확보와 MUTEX 획득을 semop 한 번에 원자적으로 할 수 있다.
 *   - SEM_UNDO 로 보내는 도중 kill -9 된 프로세스가 쥔 MUTEX 와 빈 칸을 커널이 돌려준다.
 *   - semtimedop 으로 기다림에 시간 제한을 걸 수 있어 죽은 상대를 알아챌 수 있다.
 *   대가로 메시지 하나에 semop 시스템 콜이 보내는 쪽 2번, 받는 쪽 2번 든다. 작은 메시지에서는 이 비용이 드러난다.
 * DECISION: payload 는 링에 한 번, 링에서 받는 쪽 버퍼로 한 번 복사한다. transport.h 가 받는 쪽 버퍼에 통째로
 *   담아 주는 약속이라 복사를 없앨 수는 없다. 그 대신 메시지 전체가 링에 들어가므로 큰 메시지도 조각내지 않는다.
 * DECISION: 링 크기는 가장 큰 메시지(머리 + 1MB)를 요청 링에 4개, 응답 링에 2개 담을 수 있게 잡는다.
 *   환경 변수 OSP_SHM_CHUNK, OSP_SHM_REQ_CHUNKS, OSP_SHM_RESP_CHUNKS 로 서버를 띄울 때 바꿀 수 있다 (메모리 실험용).
 */
#define _GNU_SOURCE             /* semtimedop, F_OFD_SETLK, F_OFD_GETLK */
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "transport.h"

#define KEY_REQ_SHM   IPC_KEY(2)
#define KEY_RESP_SHM  IPC_KEY(3)
#define KEY_REQ_SEM   IPC_KEY(4)
#define KEY_RESP_SEM  IPC_KEY(5)

#define PATH_LEN 64
#define WAIT_MS  200                        /* 기다리는 동안 상대를 확인하는 주기 */

#define LOCK_SERVER      ((off_t)0)
#define LOCK_CLIENT(id)  ((off_t)(id) + 1)

enum { S_MUTEX = 0, S_FREE = 1, S_MSGS = 2, S_PER_RING = 3 };    /* 링 하나의 세마포어 순서 */

#define SHM_READY      0x53484d32u          /* "SHM2". 서버가 모두 만든 뒤 요청 영역 머리에 쓴다 */
#define MSG_MAX        (sizeof(msg_hdr_t) + (size_t)MAX_PAYLOAD)
#define DEF_CHUNK      4096u
#define DEF_REQ_MSGS   4u
#define DEF_RESP_MSGS  2u
#define SEM_MAX_VALUE  32767u               /* SEMVMX. FREE 의 최댓값 = 칸 수의 한도 */

/* 요청 영역의 맨 앞. 64B */
typedef struct {
    uint32_t ready;
    uint32_t chunk;
    uint32_t req_chunks;
    uint32_t resp_chunks;
    uint32_t pad[12];
} seg_hdr_t;

/* 링 머리. 공유 메모리 안에 있다. 64B */
typedef struct {
    uint64_t head;          /* 다음에 쓸 칸 번호. 계속 커지며 위치는 head % nchunks. MUTEX 가 보호 */
    uint64_t tail;          /* 다음에 읽을 칸 번호. 받는 쪽만 바꾼다 */
    uint32_t pad[12];
} ring_hdr_t;

/* 프로세스 안에서 링 하나를 다루는 정보 */
typedef struct {
    ring_hdr_t    *hdr;
    char          *data;
    int            semid;
    unsigned short base;    /* 집합 안에서 이 링의 첫 세마포어 번호 */
    uint32_t       chunk;
    uint32_t       nchunks;
} ring_t;

union semun {
    int              val;
    struct semid_ds *buf;
    unsigned short  *array;
};

static void lock_path(char *buf) { snprintf(buf, PATH_LEN, "/tmp/osproject_%d_shm.lock", TEAM_NO); }

/* ---- 잠금 파일 (mq 버전과 같다) ---- */

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

/* ---- 링 ---- */

static uint32_t chunks_for(uint32_t chunk, size_t bytes) { return (uint32_t)((bytes + chunk - 1) / chunk); }

static void ring_bind(ring_t *r, char *base, uint32_t chunk, uint32_t nchunks, int semid, unsigned short sem_base)
{
    r->hdr = (ring_hdr_t *)base;
    r->data = base + sizeof(ring_hdr_t);
    r->chunk = chunk;
    r->nchunks = nchunks;
    r->semid = semid;
    r->base = sem_base;
}

static size_t ring_bytes(const ring_t *r) { return (size_t)r->nchunks * r->chunk; }

/* 링의 바이트 위치 off 부터 len 바이트를 쓴다. 끝을 넘으면 처음으로 이어 쓴다 */
static void copy_in(const ring_t *r, size_t off, const void *src, size_t len)
{
    size_t total = ring_bytes(r);
    if (off >= total) off -= total;
    size_t first = len < total - off ? len : total - off;
    memcpy(r->data + off, src, first);
    if (len > first) memcpy(r->data, (const char *)src + first, len - first);
}

static void copy_out(const ring_t *r, size_t off, void *dst, size_t len)
{
    size_t total = ring_bytes(r);
    if (off >= total) off -= total;
    size_t first = len < total - off ? len : total - off;
    memcpy(dst, r->data + off, first);
    if (len > first) memcpy((char *)dst + first, r->data, len - first);
}

/* 기다리는 쪽이 상대를 확인하는 방법. lock_fd < 0 이면 시간 제한 없이 기다린다 */
typedef struct {
    int                lock_fd;
    off_t              peer;        /* 상대의 잠금 바이트 */
    const atomic_bool *cancel;      /* 켜지면 기다림을 그만둔다 (닫는 중) */
} waiter_t;

/*
 * semop 을 끝까지 한다. 기다려야 하면 WAIT_MS 마다 깨어나 상대가 살아 있는지 본다.
 * 성공 0. 상대가 죽었으면 -1/EPIPE, 닫는 중이면 -1/ECANCELED, 세마포어가 지워졌으면 -1/EIDRM.
 */
static int sem_ops(int semid, struct sembuf *ops, size_t n, const waiter_t *w)
{
    struct timespec ts = { 0, WAIT_MS * 1000000L };
    for (;;) {
        int rc = (w && w->lock_fd >= 0) ? semtimedop(semid, ops, n, &ts) : semop(semid, ops, n);
        if (rc == 0) return 0;
        if (errno == EINTR) {
            if (w && w->cancel && atomic_load(w->cancel)) { errno = ECANCELED; return -1; }
            continue;
        }
        if (errno == EAGAIN && w && w->lock_fd >= 0) {     /* 시간 제한에 걸렸다 */
            if (w->cancel && atomic_load(w->cancel)) { errno = ECANCELED; return -1; }
            if (byte_locked(w->lock_fd, w->peer) == 0) { errno = EPIPE; return -1; }
            continue;
        }
        if (errno == EINVAL) errno = EIDRM;     /* 지워진 집합에 다시 부르면 EINVAL */
        return -1;
    }
}

/* 메시지 하나를 링에 넣는다. 빈 칸이 모자라면 생길 때까지 기다린다 */
static int ring_send(const ring_t *r, const msg_hdr_t *hdr, const void *payload, const waiter_t *w)
{
    if (hdr->payload_len > MAX_PAYLOAD) { errno = EMSGSIZE; return -1; }
    size_t total = sizeof *hdr + hdr->payload_len;
    uint32_t k = chunks_for(r->chunk, total);
    if (k > r->nchunks) { errno = EMSGSIZE; return -1; }

    /*
     * 빈 칸과 MUTEX 를 한 번에 얻는다. 둘 다 SEM_UNDO 라서 복사하다 죽어도 커널이 되돌린다.
     * 다 쓴 뒤의 semop 에서 FREE 에 +k(SEM_UNDO), -k 를 함께 하면 값은 그대로이고 되돌리기 기록만 지워진다.
     */
    struct sembuf acq[2] = {
        { (unsigned short)(r->base + S_FREE),  (short)-(int)k, SEM_UNDO },
        { (unsigned short)(r->base + S_MUTEX), -1,             SEM_UNDO },
    };
    if (sem_ops(r->semid, acq, 2, w) < 0) return -1;

    /*
     * head 는 MUTEX(세마포어)가 보호한다. 원자적 읽기·쓰기(acquire/release)로 둔 것은 그 순서를 C 메모리 모델로도
     * 드러내기 위해서다. ThreadSanitizer 는 semop 을 동기화로 보지 못해, 보통 변수로 두면 같은 프로세스의 두 워커가
     * head 를 잠금 없이 쓰는 것처럼 경고한다. x86 에서는 보통의 읽기·쓰기와 같은 명령이다.
     */
    uint64_t head = __atomic_load_n(&r->hdr->head, __ATOMIC_ACQUIRE);
    size_t off = (size_t)(head % r->nchunks) * r->chunk;
    memcpy(r->data + off, hdr, sizeof *hdr);
    if (hdr->payload_len) copy_in(r, off + sizeof *hdr, payload, hdr->payload_len);
    __atomic_store_n(&r->hdr->head, head + k, __ATOMIC_RELEASE);

    struct sembuf pub[4] = {
        { (unsigned short)(r->base + S_MSGS),  1,              0 },
        { (unsigned short)(r->base + S_FREE),  (short)k,       SEM_UNDO },
        { (unsigned short)(r->base + S_FREE),  (short)-(int)k, 0 },
        { (unsigned short)(r->base + S_MUTEX), 1,              SEM_UNDO },
    };
    return sem_ops(r->semid, pub, 4, NULL);
}

/*
 * MSGS 를 하나 얻은 뒤 부른다. 맨 앞 메시지를 꺼내 payload 길이를 돌려준다.
 * cap 보다 큰 메시지는 버리고 -1/EMSGSIZE (hdr 에는 머리를 채운다).
 */
static ssize_t ring_take(const ring_t *r, msg_hdr_t *hdr, void *payload, size_t cap)
{
    uint64_t tail = __atomic_load_n(&r->hdr->tail, __ATOMIC_ACQUIRE);
    size_t off = (size_t)(tail % r->nchunks) * r->chunk;
    memcpy(hdr, r->data + off, sizeof *hdr);
    size_t len = hdr->payload_len;
    if (len > MAX_PAYLOAD) { errno = EBADMSG; return -1; }     /* 링이 깨졌다. 보통은 생기지 않는다 */
    uint32_t k = chunks_for(r->chunk, sizeof *hdr + len);
    bool fits = len <= cap;
    if (fits && len) copy_out(r, off + sizeof *hdr, payload, len);
    __atomic_store_n(&r->hdr->tail, tail + k, __ATOMIC_RELEASE);

    struct sembuf rel = { (unsigned short)(r->base + S_FREE), (short)k, 0 };
    if (sem_ops(r->semid, &rel, 1, NULL) < 0) return -1;
    if (!fits) { errno = EMSGSIZE; return -1; }
    return (ssize_t)len;
}

static ssize_t ring_recv(const ring_t *r, msg_hdr_t *hdr, void *payload, size_t cap, const waiter_t *w)
{
    struct sembuf wait_msg = { (unsigned short)(r->base + S_MSGS), -1, 0 };
    if (sem_ops(r->semid, &wait_msg, 1, w) < 0) return -1;
    return ring_take(r, hdr, payload, cap);
}

/* ---- 크기 ---- */

typedef struct {
    uint32_t chunk, req_chunks, resp_chunks;
} geom_t;

/* 환경 변수 값. 없으면 def, 숫자가 아니면 0 */
static uint32_t env_u32(const char *name, uint32_t def)
{
    const char *s = getenv(name);
    if (!s || !*s) return def;
    char *end;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno || *end || v > UINT32_MAX) return 0;
    return (uint32_t)v;
}

static int geom_from_env(geom_t *g)
{
    g->chunk = env_u32("OSP_SHM_CHUNK", DEF_CHUNK);
    if (g->chunk < 64 || g->chunk % 64) { errno = EINVAL; return -1; }     /* 머리가 첫 칸에 들어가고 8B 정렬 */
    uint32_t per_msg = chunks_for(g->chunk, MSG_MAX);
    g->req_chunks = env_u32("OSP_SHM_REQ_CHUNKS", DEF_REQ_MSGS * per_msg);
    g->resp_chunks = env_u32("OSP_SHM_RESP_CHUNKS", DEF_RESP_MSGS * per_msg);
    if (g->req_chunks < per_msg || g->resp_chunks < per_msg ||
        g->req_chunks > SEM_MAX_VALUE || g->resp_chunks > SEM_MAX_VALUE) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static size_t req_seg_size(const geom_t *g)  { return sizeof(seg_hdr_t) + sizeof(ring_hdr_t) + (size_t)g->req_chunks * g->chunk; }
static size_t resp_stride(const geom_t *g)   { return sizeof(ring_hdr_t) + (size_t)g->resp_chunks * g->chunk; }
static size_t resp_seg_size(const geom_t *g) { return resp_stride(g) * MAX_CLIENTS; }

const char *tp_name(void) { return "shm"; }

/* ================================================================
 * 서버 쪽
 * ================================================================ */

static int srv_lock_fd = -1;
static int srv_req_shm = -1, srv_resp_shm = -1;
static int srv_req_sem = -1, srv_resp_sem = -1;
static char *srv_req_base, *srv_resp_base;
static ring_t srv_req;                       /* 받는 쪽 */
static ring_t srv_resp[MAX_CLIENTS];         /* 보내는 쪽 */
static atomic_bool srv_opened;
static atomic_bool srv_stopping;             /* tp_server_wake 이후 */
static atomic_bool srv_closing;              /* tp_server_close 이후. 기다리던 송신이 그만둔다 */
static atomic_int  srv_users;                /* 공유 메모리를 쓰는 중인 스레드 수 */

/* 공유 메모리를 쓰기 전에 부른다. 닫는 중이면 false. 닫는 쪽은 srv_users 가 0 이 될 때까지 기다린 뒤 떼어 낸다 */
static bool srv_enter(void)
{
    atomic_fetch_add(&srv_users, 1);
    if (!atomic_load(&srv_closing) && atomic_load(&srv_opened)) return true;
    atomic_fetch_sub(&srv_users, 1);
    return false;
}

static void srv_leave(void) { atomic_fetch_sub(&srv_users, 1); }

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

/* 비정상 종료한 우리 서버가 남긴 자원을 지운다. 서버 자리를 잡은 뒤에만 부른다 */
static void remove_stale(void)
{
    key_t shm_keys[2] = { KEY_REQ_SHM, KEY_RESP_SHM };
    key_t sem_keys[2] = { KEY_REQ_SEM, KEY_RESP_SEM };
    for (int i = 0; i < 2; i++) {
        int id = shmget(shm_keys[i], 0, 0);
        if (id >= 0) shmctl(id, IPC_RMID, NULL);
        id = semget(sem_keys[i], 0, 0);
        if (id >= 0) semctl(id, 0, IPC_RMID);
    }
}

/* 링 nrings 개의 세마포어 집합을 만든다. 링마다 MUTEX 1, FREE nchunks, MSGS 0 */
static int make_sem(key_t key, unsigned nrings, uint32_t nchunks)
{
    int id = semget(key, (int)(nrings * S_PER_RING), IPC_CREAT | IPC_EXCL | 0600);
    if (id < 0) return -1;
    unsigned short *vals = malloc(sizeof *vals * nrings * S_PER_RING);
    if (!vals) { semctl(id, 0, IPC_RMID); errno = ENOMEM; return -1; }
    for (unsigned i = 0; i < nrings; i++) {
        vals[i * S_PER_RING + S_MUTEX] = 1;
        vals[i * S_PER_RING + S_FREE] = (unsigned short)nchunks;
        vals[i * S_PER_RING + S_MSGS] = 0;
    }
    union semun arg = { .array = vals };
    int rc = semctl(id, 0, SETALL, arg);
    int e = errno;
    free(vals);
    if (rc < 0) { semctl(id, 0, IPC_RMID); errno = e; return -1; }
    return id;
}

/* 공유 메모리를 만들고 붙인다. 새 영역은 커널이 0 으로 채워 준다 */
static char *make_shm(key_t key, size_t size, int *idp)
{
    int id = shmget(key, size, IPC_CREAT | IPC_EXCL | 0600);
    if (id < 0) return NULL;
    void *p = shmat(id, NULL, 0);
    if (p == (void *)-1) {
        int e = errno;
        shmctl(id, IPC_RMID, NULL);
        errno = e;
        return NULL;
    }
    *idp = id;
    return p;
}

static void server_release(void)
{
    char lp[PATH_LEN];
    lock_path(lp);
    if (srv_req_sem >= 0)  { semctl(srv_req_sem, 0, IPC_RMID);  srv_req_sem = -1; }
    if (srv_resp_sem >= 0) { semctl(srv_resp_sem, 0, IPC_RMID); srv_resp_sem = -1; }
    if (srv_req_base)  { shmdt(srv_req_base);  srv_req_base = NULL; }
    if (srv_resp_base) { shmdt(srv_resp_base); srv_resp_base = NULL; }
    if (srv_req_shm >= 0)  { shmctl(srv_req_shm, IPC_RMID, NULL);  srv_req_shm = -1; }
    if (srv_resp_shm >= 0) { shmctl(srv_resp_shm, IPC_RMID, NULL); srv_resp_shm = -1; }
    if (srv_lock_fd >= 0) {
        unlink(lp);                      /* 잠금을 쥔 채로 이름부터 지운다 */
        close(srv_lock_fd);
        srv_lock_fd = -1;
    }
}

int tp_server_open(void)
{
    srv_lock_fd = server_lock();         /* 서버가 이미 떠 있으면 EADDRINUSE */
    if (srv_lock_fd < 0) return -1;
    remove_stale();

    geom_t g;
    if (geom_from_env(&g) < 0) goto fail;

    srv_req_sem = make_sem(KEY_REQ_SEM, 1, g.req_chunks);
    if (srv_req_sem < 0) goto fail;
    srv_resp_sem = make_sem(KEY_RESP_SEM, MAX_CLIENTS, g.resp_chunks);
    if (srv_resp_sem < 0) goto fail;
    srv_req_base = make_shm(KEY_REQ_SHM, req_seg_size(&g), &srv_req_shm);
    if (!srv_req_base) goto fail;
    srv_resp_base = make_shm(KEY_RESP_SHM, resp_seg_size(&g), &srv_resp_shm);
    if (!srv_resp_base) goto fail;

    ring_bind(&srv_req, srv_req_base + sizeof(seg_hdr_t), g.chunk, g.req_chunks, srv_req_sem, 0);
    for (uint32_t i = 0; i < MAX_CLIENTS; i++)
        ring_bind(&srv_resp[i], srv_resp_base + (size_t)i * resp_stride(&g), g.chunk, g.resp_chunks,
                  srv_resp_sem, (unsigned short)(i * S_PER_RING));

    seg_hdr_t *sh = (seg_hdr_t *)srv_req_base;
    sh->chunk = g.chunk;
    sh->req_chunks = g.req_chunks;
    sh->resp_chunks = g.resp_chunks;
    atomic_store(&srv_stopping, false);
    atomic_store(&srv_closing, false);
    atomic_store(&srv_users, 0);
    atomic_store(&srv_opened, true);
    __atomic_store_n(&sh->ready, SHM_READY, __ATOMIC_RELEASE);  /* 이제 클라이언트가 붙을 수 있다 */
    return 0;

fail:;
    int e = errno;
    server_release();
    errno = e;
    return -1;
}

/*
 * 요청 1개 받기. 수신 스레드 하나만 부른다.
 * tp_server_wake 로 깨워지거나 닫히면 -1, errno = ECANCELED. cap 보다 큰 요청은 버리고 -1, errno = EMSGSIZE.
 */
ssize_t tp_recv_req(msg_hdr_t *hdr, void *payload, size_t cap)
{
    if (!srv_enter()) { errno = ECANCELED; return -1; }
    ssize_t n = -1;
    if (atomic_load(&srv_stopping)) { errno = ECANCELED; goto out; }

    /* 요청은 기다릴 상대가 정해져 있지 않다. 시간 제한 없이 기다리고 tp_server_wake 가 깨운다 */
    struct sembuf wait_msg = { (unsigned short)(srv_req.base + S_MSGS), -1, 0 };
    if (sem_ops(srv_req.semid, &wait_msg, 1, NULL) < 0) {
        if (errno == EIDRM) errno = ECANCELED;
        goto out;
    }
    if (atomic_load(&srv_stopping)) { errno = ECANCELED; goto out; }   /* 깨우기였다 */
    n = ring_take(&srv_req, hdr, payload, cap);
out:
    srv_leave();
    return n;
}

/*
 * hdr->client_id 에게 응답을 보낸다. 여러 워커가 동시에 불러도 된다 (MUTEX 가 순서를 정한다).
 * 그 클라이언트의 링이 가득 찬 채로 클라이언트가 죽었으면 -1, errno = EPIPE.
 */
int tp_send_resp(const msg_hdr_t *hdr, const void *payload)
{
    uint32_t id = hdr->client_id;
    if (id >= MAX_CLIENTS) { errno = EINVAL; return -1; }
    if (!srv_enter()) { errno = EPIPE; return -1; }

    waiter_t w = { srv_lock_fd, LOCK_CLIENT(id), &srv_closing };
    int rc = ring_send(&srv_resp[id], hdr, payload, &w);
    if (rc < 0 && (errno == EIDRM || errno == ECANCELED)) errno = EPIPE;
    srv_leave();
    return rc;
}

/* 막혀 있는 tp_recv_req 를 깨운다. 요청 링의 MSGS 를 하나 올린다 (실제 메시지는 없다) */
void tp_server_wake(void)
{
    if (!srv_enter()) return;
    atomic_store(&srv_stopping, true);
    struct sembuf p = { (unsigned short)(srv_req.base + S_MSGS), 1, 0 };
    while (semop(srv_req.semid, &p, 1) < 0 && errno == EINTR)
        ;
    srv_leave();
}

void tp_server_close(void)
{
    if (!atomic_exchange(&srv_opened, false)) return;
    atomic_store(&srv_stopping, true);
    atomic_store(&srv_closing, true);

    /*
     * 세마포어를 먼저 지운다. 이 프로세스와 클라이언트들에서 기다리던 semop 이 모두 EIDRM 으로 깨어난다.
     * 클라이언트의 tp_recv_resp 는 ECONNRESET, tp_send_req 는 EPIPE 로 끝난다.
     */
    semctl(srv_req_sem, 0, IPC_RMID);
    semctl(srv_resp_sem, 0, IPC_RMID);
    srv_req_sem = srv_resp_sem = -1;

    /* 공유 메모리를 쓰던 스레드가 모두 빠져나온 뒤에 떼어 낸다 */
    while (atomic_load(&srv_users) > 0) {
        struct timespec ts = { 0, 1000000L };
        nanosleep(&ts, NULL);
    }
    server_release();
}

/* ================================================================
 * 클라이언트 쪽
 * ================================================================ */

static int cli_lock_fd = -1;
static int cli_req_shm = -1, cli_resp_shm = -1;
static int cli_req_sem = -1, cli_resp_sem = -1;
static char *cli_req_base, *cli_resp_base;
static ring_t cli_req;                       /* 보내는 쪽 */
static ring_t cli_resp;                      /* 내 응답 링, 받는 쪽 */
static uint32_t cli_id;
static atomic_bool cli_opened;
static atomic_bool cli_closing;
static atomic_int  cli_users;

static bool cli_enter(void)
{
    atomic_fetch_add(&cli_users, 1);
    if (!atomic_load(&cli_closing) && atomic_load(&cli_opened)) return true;
    atomic_fetch_sub(&cli_users, 1);
    return false;
}

static void cli_leave(void) { atomic_fetch_sub(&cli_users, 1); }

static void cli_detach(void)
{
    if (cli_req_base)  { shmdt(cli_req_base);  cli_req_base = NULL; }
    if (cli_resp_base) { shmdt(cli_resp_base); cli_resp_base = NULL; }
    if (cli_lock_fd >= 0) { close(cli_lock_fd); cli_lock_fd = -1; }    /* 클라이언트 자리 잠금이 풀린다 */
    cli_req_shm = cli_resp_shm = cli_req_sem = cli_resp_sem = -1;
}

/*
 * 서버가 자원을 지우지 못하고 죽었으면(kill -9) 남은 자원을 지운다. 기다리던 다른 클라이언트들도 깨어난다.
 * 열 때 받아 둔 번호로만 지운다. 그 뒤에 새로 뜬 서버의 자원은 번호가 달라 건드리지 않는다.
 */
static void cli_server_gone(void)
{
    semctl(cli_req_sem, 0, IPC_RMID);
    semctl(cli_resp_sem, 0, IPC_RMID);
    shmctl(cli_req_shm, IPC_RMID, NULL);
    shmctl(cli_resp_shm, IPC_RMID, NULL);
}

/*
 * 내 응답 링을 비운다. 같은 번호를 쓰던 예전 클라이언트가 받지 않고 간 응답이 남아 있을 수 있다.
 * MUTEX 를 쥐고 하므로 서버가 이 링에 쓰는 중이면 끝나기를 기다린다.
 * 한계: 예전 클라이언트가 보낸 요청을 서버가 아직 처리하고 있었다면, 그 응답은 비운 뒤에 들어와 새 클라이언트가 받는다.
 *   kill -9 된 클라이언트의 번호를 곧바로 다시 쓸 때만 생기며 FIFO 버전도 같다. 받는 쪽은 자기가 보내지 않은
 *   req_id 의 응답을 버려야 한다 (임시 검사에서 세션마다 req_id 시작값을 달리해 확인).
 */
static int reset_my_ring(void)
{
    waiter_t w = { cli_lock_fd, LOCK_SERVER, NULL };
    struct sembuf lock = { (unsigned short)(cli_resp.base + S_MUTEX), -1, SEM_UNDO };
    if (sem_ops(cli_resp.semid, &lock, 1, &w) < 0) return -1;

    __atomic_store_n(&cli_resp.hdr->head, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&cli_resp.hdr->tail, 0, __ATOMIC_RELEASE);
    union semun a = { .val = (int)cli_resp.nchunks };
    union semun z = { .val = 0 };
    int rc = 0;
    if (semctl(cli_resp.semid, cli_resp.base + S_FREE, SETVAL, a) < 0 ||
        semctl(cli_resp.semid, cli_resp.base + S_MSGS, SETVAL, z) < 0) rc = -1;
    int e = errno;

    struct sembuf unlock = { (unsigned short)(cli_resp.base + S_MUTEX), 1, SEM_UNDO };
    sem_ops(cli_resp.semid, &unlock, 1, NULL);
    errno = e;
    return rc;
}

static int attach(key_t key, int *idp, char **basep)
{
    int id = shmget(key, 0, 0);
    if (id < 0) return -1;
    void *p = shmat(id, NULL, 0);
    if (p == (void *)-1) return -1;
    *idp = id;
    *basep = p;
    return 0;
}

/*
 * 요청 링과 내 응답 링에 연결한다.
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

    if (attach(KEY_REQ_SHM, &cli_req_shm, &cli_req_base) < 0) {
        if (errno == ENOENT || errno == EIDRM || errno == EINVAL) errno = ECONNREFUSED;
        goto fail;
    }
    seg_hdr_t *sh = (seg_hdr_t *)cli_req_base;
    if (__atomic_load_n(&sh->ready, __ATOMIC_ACQUIRE) != SHM_READY) { errno = ECONNREFUSED; goto fail; }  /* 서버가 아직 만드는 중 */
    geom_t g = { sh->chunk, sh->req_chunks, sh->resp_chunks };

    if (attach(KEY_RESP_SHM, &cli_resp_shm, &cli_resp_base) < 0) {
        if (errno == ENOENT || errno == EIDRM || errno == EINVAL) errno = ECONNREFUSED;
        goto fail;
    }
    struct shmid_ds ds;
    if (shmctl(cli_resp_shm, IPC_STAT, &ds) < 0 || ds.shm_segsz < resp_seg_size(&g)) { errno = ECONNREFUSED; goto fail; }

    cli_req_sem = semget(KEY_REQ_SEM, 0, 0);
    cli_resp_sem = semget(KEY_RESP_SEM, 0, 0);
    if (cli_req_sem < 0 || cli_resp_sem < 0) {
        if (errno == ENOENT) errno = ECONNREFUSED;
        goto fail;
    }

    ring_bind(&cli_req, cli_req_base + sizeof(seg_hdr_t), g.chunk, g.req_chunks, cli_req_sem, 0);
    ring_bind(&cli_resp, cli_resp_base + (size_t)client_id * resp_stride(&g), g.chunk, g.resp_chunks,
              cli_resp_sem, (unsigned short)(client_id * S_PER_RING));

    if (reset_my_ring() < 0) {
        if (errno == EPIPE || errno == EIDRM || errno == EINVAL) errno = ECONNREFUSED;
        goto fail;
    }

    cli_id = client_id;
    atomic_store(&cli_closing, false);
    atomic_store(&cli_users, 0);
    atomic_store(&cli_opened, true);
    return 0;

fail:;
    int e = errno;
    cli_detach();
    errno = e;
    return -1;
}

/* 서버가 끝났으면 -1, errno = EPIPE */
int tp_send_req(const msg_hdr_t *hdr, const void *payload)
{
    if (!cli_enter()) { errno = EPIPE; return -1; }
    waiter_t w = { cli_lock_fd, LOCK_SERVER, &cli_closing };
    int rc = ring_send(&cli_req, hdr, payload, &w);
    if (rc < 0) {
        if (errno == EPIPE) cli_server_gone();
        if (errno == EIDRM || errno == ECANCELED) errno = EPIPE;
    }
    cli_leave();
    return rc;
}

/*
 * 응답 1개 받기. 수신 스레드 하나만 부른다.
 * 서버가 끝나면 -1, errno = ECONNRESET. cap 보다 큰 응답은 버리고 -1, errno = EMSGSIZE.
 */
ssize_t tp_recv_resp(msg_hdr_t *hdr, void *payload, size_t cap)
{
    if (!cli_enter()) { errno = ECONNRESET; return -1; }
    waiter_t w = { cli_lock_fd, LOCK_SERVER, &cli_closing };
    ssize_t n = ring_recv(&cli_resp, hdr, payload, cap, &w);
    if (n < 0) {
        if (errno == EPIPE) { cli_server_gone(); errno = ECONNRESET; }
        else if (errno == EIDRM) errno = ECONNRESET;
    }
    cli_leave();
    return n;
}

void tp_client_close(void)
{
    if (!atomic_load(&cli_opened)) return;
    atomic_store(&cli_closing, true);

    /* 기다리던 송수신은 WAIT_MS 안에 cli_closing 을 보고 빠져나온다 */
    while (atomic_load(&cli_users) > 0) {
        struct timespec ts = { 0, 1000000L };
        nanosleep(&ts, NULL);
    }
    atomic_store(&cli_opened, false);
    cli_detach();
}
