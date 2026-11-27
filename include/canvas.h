/*
 * canvas.h - 그림판 K개와 타일 락 (서버의 공유 자료구조)
 *
 * 그림판 하나는 CANVAS_W × CANVAS_H 칸, 칸당 1바이트다. 락의 단위는 tile × tile 칸의 타일이다.
 * 한 요청이 여러 타일을 잡을 때는 그림판 안에서 타일 번호(행 우선) 오름차순으로만 잡는다 → 교착이 없다.
 * 동기화 설계의 원본은 docs/design/README.md#공유-자원과-동기화 다.
 */
#ifndef CANVAS_MODULE_H
#define CANVAS_MODULE_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#define TILE_MIN 16u
#define TILE_MAX 1024u
#define TILE_DEFAULT 64u

typedef enum {
    LOCK_RW,        /* 타일마다 rwlock (쓰기 우선). 기본 */
    LOCK_MUTEX,     /* 타일마다 mutex */
    LOCK_ROOM,      /* 그림판 전체에 mutex 하나 */
    LOCK_NONE,      /* 락 없음. 경쟁 상태 시연 전용 (요구사항 N-3 의 대조군) */
} lock_mode_t;

typedef struct {
    uint32_t nrooms;
    uint32_t tile;              /* 타일 한 변 */
    uint32_t tps;               /* 한 변의 타일 수 = CANVAS_W / tile */
    uint32_t ntiles;            /* 그림판 하나의 타일 수 */
    lock_mode_t mode;
    uint8_t *pix;               /* nrooms × CANVAS_W × CANVAS_H */
    pthread_rwlock_t *rw;       /* LOCK_RW: nrooms × ntiles */
    pthread_mutex_t *mtx;       /* LOCK_MUTEX: nrooms × ntiles, LOCK_ROOM: nrooms */
} canvas_t;

/* 성공 0. 잘못된 값이면 -1/EINVAL, 메모리가 없으면 -1/ENOMEM */
int  canvas_init(canvas_t *c, uint32_t nrooms, uint32_t tile, lock_mode_t mode);
void canvas_free(canvas_t *c);

int  lock_mode_parse(const char *s, lock_mode_t *out);    /* "rw", "mutex", "room", "none" */
const char *lock_mode_name(lock_mode_t m);

/* 영역이 그림판 안이고 크기가 0 이 아니면 0, 아니면 proto.h 의 ERR_* */
int  canvas_check(const canvas_t *c, uint32_t room, uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* 영역을 덮는 타일 락을 오름차순으로 모두 잡는다 / 푼다. write = DRAW, 아니면 SNAPSHOT */
void canvas_lock(canvas_t *c, uint32_t room, uint32_t x, uint32_t y, uint32_t w, uint32_t h, bool write);
void canvas_unlock(canvas_t *c, uint32_t room, uint32_t x, uint32_t y, uint32_t w, uint32_t h, bool write);

/* 락을 잡은 뒤에 부른다. 영역은 canvas_check 를 통과한 값이어야 한다 */
void canvas_draw(canvas_t *c, uint32_t room, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t v);
void canvas_copy(const canvas_t *c, uint32_t room, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t *dst);

/* 그림판 하나의 FNV-1a 64비트 체크섬. 모든 그림판을 차례로 이은 값은 canvas_checksum_all */
uint64_t canvas_checksum(const canvas_t *c, uint32_t room);
uint64_t canvas_checksum_all(const canvas_t *c);

/* 그림판 하나를 PGM(P5, 1024 × 1024, 255)으로 저장. 성공 0 */
int  canvas_save_pgm(const canvas_t *c, uint32_t room, const char *path);

#endif /* CANVAS_MODULE_H */
