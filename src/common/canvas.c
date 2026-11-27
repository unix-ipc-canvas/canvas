/*
 * canvas.c - 그림판 K개와 타일 락
 *
 * DRAW 는 영역의 칸에 v 를 더한다(mod 256). 더하기는 순서와 무관해서, 같은 DRAW 묶음이면 처리 순서와 관계없이
 * 결과가 같다. 그래서 서버 체크섬을 검증기(스레드 하나로 다시 그린 정답)와 비교해 경쟁 상태를 찾을 수 있다.
 */
#define _GNU_SOURCE             /* PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proto.h"
#include "canvas.h"

#define ROOM_BYTES ((size_t)CANVAS_W * CANVAS_H)

int lock_mode_parse(const char *s, lock_mode_t *out)
{
    static const char *names[] = { "rw", "mutex", "room", "none" };
    for (int i = 0; i < 4; i++)
        if (strcmp(s, names[i]) == 0) { *out = (lock_mode_t)i; return 0; }
    return -1;
}

const char *lock_mode_name(lock_mode_t m)
{
    switch (m) {
    case LOCK_RW:    return "rw";
    case LOCK_MUTEX: return "mutex";
    case LOCK_ROOM:  return "room";
    default:         return "none";
    }
}

int canvas_init(canvas_t *c, uint32_t nrooms, uint32_t tile, lock_mode_t mode)
{
    memset(c, 0, sizeof *c);
    if (nrooms < 1 || nrooms > MAX_ROOMS || tile < TILE_MIN || tile > TILE_MAX || (tile & (tile - 1))) {
        errno = EINVAL;
        return -1;
    }
    c->nrooms = nrooms;
    c->tile = tile;
    c->tps = CANVAS_W / tile;
    c->ntiles = c->tps * c->tps;
    c->mode = mode;
    /* 그림판은 페이지 경계에 맞춘다. 처음에 모두 0 이다 (요구사항 F-1) */
    if (posix_memalign((void **)&c->pix, 4096, nrooms * ROOM_BYTES) != 0) { errno = ENOMEM; return -1; }
    memset(c->pix, 0, nrooms * ROOM_BYTES);

    size_t nl = 0;
    if (mode == LOCK_RW) {
        nl = (size_t)nrooms * c->ntiles;
        c->rw = calloc(nl, sizeof *c->rw);
        if (!c->rw) goto nomem;
        pthread_rwlockattr_t a;
        pthread_rwlockattr_init(&a);
        /* 쓰기 우선: DRAW(사용감을 정하는 요청)가 SNAPSHOT 읽기에 밀려 굶지 않게 한다 */
        pthread_rwlockattr_setkind_np(&a, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
        for (size_t i = 0; i < nl; i++) pthread_rwlock_init(&c->rw[i], &a);
        pthread_rwlockattr_destroy(&a);
    } else if (mode == LOCK_MUTEX || mode == LOCK_ROOM) {
        nl = mode == LOCK_MUTEX ? (size_t)nrooms * c->ntiles : nrooms;
        c->mtx = calloc(nl, sizeof *c->mtx);
        if (!c->mtx) goto nomem;
        for (size_t i = 0; i < nl; i++) pthread_mutex_init(&c->mtx[i], NULL);
    }
    return 0;

nomem:
    free(c->pix);
    c->pix = NULL;
    errno = ENOMEM;
    return -1;
}

void canvas_free(canvas_t *c)
{
    if (c->rw) {
        for (size_t i = 0; i < (size_t)c->nrooms * c->ntiles; i++) pthread_rwlock_destroy(&c->rw[i]);
        free(c->rw);
    }
    if (c->mtx) {
        size_t nl = c->mode == LOCK_MUTEX ? (size_t)c->nrooms * c->ntiles : c->nrooms;
        for (size_t i = 0; i < nl; i++) pthread_mutex_destroy(&c->mtx[i]);
        free(c->mtx);
    }
    free(c->pix);
    memset(c, 0, sizeof *c);
}

int canvas_check(const canvas_t *c, uint32_t room, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (room >= c->nrooms) return ERR_ROOM;
    if (w == 0 || h == 0 || x >= CANVAS_W || y >= CANVAS_H || w > CANVAS_W - x || h > CANVAS_H - y) return ERR_RANGE;
    return 0;
}

/* 영역을 덮는 타일의 행·열 범위 */
static void tile_span(const canvas_t *c, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                      uint32_t *tx0, uint32_t *ty0, uint32_t *tx1, uint32_t *ty1)
{
    uint32_t sh = (uint32_t)__builtin_ctz(c->tile);
    *tx0 = x >> sh;
    *ty0 = y >> sh;
    *tx1 = (x + w - 1) >> sh;
    *ty1 = (y + h - 1) >> sh;
}

void canvas_lock(canvas_t *c, uint32_t room, uint32_t x, uint32_t y, uint32_t w, uint32_t h, bool write)
{
    switch (c->mode) {
    case LOCK_NONE:
        return;
    case LOCK_ROOM:
        pthread_mutex_lock(&c->mtx[room]);
        return;
    default:
        break;
    }
    uint32_t tx0, ty0, tx1, ty1;
    tile_span(c, x, y, w, h, &tx0, &ty0, &tx1, &ty1);
    size_t base = (size_t)room * c->ntiles;
    /* 행 우선 오름차순. 모든 스레드가 같은 순서로 잡으므로 기다림의 고리가 생기지 않는다 */
    for (uint32_t ty = ty0; ty <= ty1; ty++)
        for (uint32_t tx = tx0; tx <= tx1; tx++) {
            size_t i = base + (size_t)ty * c->tps + tx;
            if (c->mode == LOCK_RW) {
                if (write) pthread_rwlock_wrlock(&c->rw[i]);
                else       pthread_rwlock_rdlock(&c->rw[i]);
            } else {
                pthread_mutex_lock(&c->mtx[i]);
            }
        }
}

void canvas_unlock(canvas_t *c, uint32_t room, uint32_t x, uint32_t y, uint32_t w, uint32_t h, bool write)
{
    (void)write;
    switch (c->mode) {
    case LOCK_NONE:
        return;
    case LOCK_ROOM:
        pthread_mutex_unlock(&c->mtx[room]);
        return;
    default:
        break;
    }
    uint32_t tx0, ty0, tx1, ty1;
    tile_span(c, x, y, w, h, &tx0, &ty0, &tx1, &ty1);
    size_t base = (size_t)room * c->ntiles;
    for (uint32_t ty = ty0; ty <= ty1; ty++)
        for (uint32_t tx = tx0; tx <= tx1; tx++) {
            size_t i = base + (size_t)ty * c->tps + tx;
            if (c->mode == LOCK_RW) pthread_rwlock_unlock(&c->rw[i]);
            else                    pthread_mutex_unlock(&c->mtx[i]);
        }
}

void canvas_draw(canvas_t *c, uint32_t room, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t v)
{
    uint8_t *p = c->pix + (size_t)room * ROOM_BYTES + (size_t)y * CANVAS_W + x;
    for (uint32_t r = 0; r < h; r++, p += CANVAS_W)
        for (uint32_t i = 0; i < w; i++) p[i] = (uint8_t)(p[i] + v);
}

void canvas_copy(const canvas_t *c, uint32_t room, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t *dst)
{
    const uint8_t *p = c->pix + (size_t)room * ROOM_BYTES + (size_t)y * CANVAS_W + x;
    for (uint32_t r = 0; r < h; r++, p += CANVAS_W, dst += w) memcpy(dst, p, w);
}

#define FNV_OFFSET 0xcbf29ce484222325ull
#define FNV_PRIME  0x100000001b3ull

static uint64_t fnv(uint64_t hsh, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) { hsh ^= p[i]; hsh *= FNV_PRIME; }
    return hsh;
}

uint64_t canvas_checksum(const canvas_t *c, uint32_t room)
{
    return fnv(FNV_OFFSET, c->pix + (size_t)room * ROOM_BYTES, ROOM_BYTES);
}

uint64_t canvas_checksum_all(const canvas_t *c)
{
    return fnv(FNV_OFFSET, c->pix, (size_t)c->nrooms * ROOM_BYTES);
}

int canvas_save_pgm(const canvas_t *c, uint32_t room, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P5\n%u %u\n255\n", CANVAS_W, CANVAS_H);
    size_t w = fwrite(c->pix + (size_t)room * ROOM_BYTES, 1, ROOM_BYTES, f);
    int rc = (w == ROOM_BYTES && fflush(f) == 0) ? 0 : -1;
    if (fclose(f) != 0) rc = -1;
    return rc;
}
