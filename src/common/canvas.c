/*
 * canvas.c - 캔버스와 타일 락 (서버와 검증기가 함께 쓴다)
 *
 * 1024 × 1024칸, 한 칸 1바이트. 락 방식과 타일 크기는 서버 실행 옵션(-l, -t)으로 정한다 (요구사항 C-9).
 * 설계의 원본은 docs/design/README.md 의 "공유 자원과 동기화", "캔버스와 락" 절이다.
 * IPC 고유 호출은 넣지 않는다 (src/common/ 규칙).
 *
 * 교착 방지: 여러 타일에 걸친 영역은 타일 번호 오름차순으로 잡는다. 아래 for_tiles 가 행(ty) 바깥, 열(tx) 안쪽으로
 * 돌기 때문에 방문 순서가 곧 tile_of 의 오름차순이다. 모든 워커가 같은 순서로 잡으므로 원형 대기가 생기지 않는다.
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "canvas.h"
#include "util.h"

struct canvas {
    uint8_t     *px;            /* CANVAS_W * CANVAS_H 바이트, 행 우선 */
    lock_mode_t  mode;
    uint32_t     tile;          /* 타일 한 변 */
    uint32_t     ntiles;
    pthread_mutex_t   global;   /* LOCK_GLOBAL */
    pthread_mutex_t  *mtx;      /* LOCK_TILE: 타일마다 1개 */
    pthread_rwlock_t *rw;       /* LOCK_TILE_RW: 타일마다 1개 */
};

/* ---- 락 방식 이름 ---- */

static const char *const mode_names[] = {
    [LOCK_GLOBAL]  = "global",
    [LOCK_TILE]    = "tile",
    [LOCK_TILE_RW] = "tile-rw",
    [LOCK_NONE]    = "none",
};

int lock_mode_parse(const char *s, lock_mode_t *out)
{
    for (size_t i = 0; i < sizeof mode_names / sizeof mode_names[0]; i++) {
        if (s && strcmp(s, mode_names[i]) == 0) {
            *out = (lock_mode_t)i;
            return 0;
        }
    }
    return -1;
}

const char *lock_mode_name(lock_mode_t m)
{
    return (unsigned)m < sizeof mode_names / sizeof mode_names[0] ? mode_names[m] : "?";
}

/* ---- 만들기와 없애기 ---- */

canvas_t *canvas_create(lock_mode_t mode, uint32_t tile)
{
    if ((unsigned)mode > LOCK_NONE || !tile_size_ok(tile)) { errno = EINVAL; return NULL; }

    canvas_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->mode = mode;
    c->tile = tile;
    c->ntiles = tile_count(tile);

    c->px = calloc(CANVAS_W * CANVAS_H, 1);     /* 0 으로 초기화 (F-1) */
    if (!c->px) goto fail;

    switch (mode) {
    case LOCK_GLOBAL:
        pthread_mutex_init(&c->global, NULL);
        break;
    case LOCK_TILE:
        c->mtx = malloc(sizeof *c->mtx * c->ntiles);
        if (!c->mtx) goto fail;
        for (uint32_t i = 0; i < c->ntiles; i++) pthread_mutex_init(&c->mtx[i], NULL);
        break;
    case LOCK_TILE_RW:
        c->rw = malloc(sizeof *c->rw * c->ntiles);
        if (!c->rw) goto fail;
        for (uint32_t i = 0; i < c->ntiles; i++) pthread_rwlock_init(&c->rw[i], NULL);
        break;
    case LOCK_NONE:
        break;
    }
    return c;

fail:
    free(c->px);
    free(c->mtx);
    free(c->rw);
    free(c);
    errno = ENOMEM;
    return NULL;
}

void canvas_destroy(canvas_t *c)
{
    if (!c) return;
    switch (c->mode) {
    case LOCK_GLOBAL:  pthread_mutex_destroy(&c->global); break;
    case LOCK_TILE:    for (uint32_t i = 0; i < c->ntiles; i++) pthread_mutex_destroy(&c->mtx[i]); break;
    case LOCK_TILE_RW: for (uint32_t i = 0; i < c->ntiles; i++) pthread_rwlock_destroy(&c->rw[i]); break;
    case LOCK_NONE:    break;
    }
    free(c->mtx);
    free(c->rw);
    free(c->px);
    free(c);
}

lock_mode_t canvas_lock_mode(const canvas_t *c) { return c->mode; }
uint32_t    canvas_tile(const canvas_t *c)      { return c->tile; }

bool canvas_rect_ok(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    /* uint32_t 로 더하므로 uint16_t 좌표의 합이 넘치지 않는다 */
    return w > 0 && h > 0 && x + w <= CANVAS_W && y + h <= CANVAS_H;
}

/* ---- 영역에 걸친 락 ---- */

enum { WANT_READ, WANT_WRITE };

/* 영역에 걸친 타일을 번호 오름차순으로 잡는다 */
static void lock_rect(canvas_t *c, uint32_t x, uint32_t y, uint32_t w, uint32_t h, int want)
{
    if (c->mode == LOCK_NONE) return;
    if (c->mode == LOCK_GLOBAL) { pthread_mutex_lock(&c->global); return; }

    uint32_t t = c->tile, cols = tile_cols(t);
    for (uint32_t ty = y / t; ty <= (y + h - 1) / t; ty++) {
        for (uint32_t tx = x / t; tx <= (x + w - 1) / t; tx++) {
            uint32_t i = ty * cols + tx;
            if (c->mode == LOCK_TILE)
                pthread_mutex_lock(&c->mtx[i]);
            else if (want == WANT_WRITE)
                pthread_rwlock_wrlock(&c->rw[i]);
            else
                pthread_rwlock_rdlock(&c->rw[i]);
        }
    }
}

/* 잡은 것을 모두 놓는다. 놓는 순서는 교착과 상관없다 */
static void unlock_rect(canvas_t *c, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (c->mode == LOCK_NONE) return;
    if (c->mode == LOCK_GLOBAL) { pthread_mutex_unlock(&c->global); return; }

    uint32_t t = c->tile, cols = tile_cols(t);
    for (uint32_t ty = y / t; ty <= (y + h - 1) / t; ty++) {
        for (uint32_t tx = x / t; tx <= (x + w - 1) / t; tx++) {
            uint32_t i = ty * cols + tx;
            if (c->mode == LOCK_TILE) pthread_mutex_unlock(&c->mtx[i]);
            else                      pthread_rwlock_unlock(&c->rw[i]);
        }
    }
}

/* ---- DRAW, SNAPSHOT ---- */

int canvas_draw(canvas_t *c, const draw_req_t *d, uint64_t *t_lock)
{
    uint32_t x = d->x, y = d->y, w = d->w, h = d->h;
    if (!canvas_rect_ok(x, y, w, h)) {
        if (t_lock) *t_lock = now_ns();
        errno = EINVAL;
        return -1;
    }

    lock_rect(c, x, y, w, h, WANT_WRITE);
    if (t_lock) *t_lock = now_ns();

    uint8_t v = d->v;
    for (uint32_t r = y; r < y + h; r++) {
        uint8_t *row = c->px + (size_t)r * CANVAS_W + x;
        for (uint32_t k = 0; k < w; k++) row[k] = (uint8_t)(row[k] + v);   /* mod 256 */
    }

    unlock_rect(c, x, y, w, h);
    return 0;
}

int canvas_snapshot(canvas_t *c, const snap_req_t *s, void *out, uint64_t *t_lock)
{
    uint32_t x = s->x, y = s->y, w = s->w, h = s->h;
    if (!canvas_rect_ok(x, y, w, h)) {
        if (t_lock) *t_lock = now_ns();
        errno = EINVAL;
        return -1;
    }

    lock_rect(c, x, y, w, h, WANT_READ);
    if (t_lock) *t_lock = now_ns();

    uint8_t *dst = out;
    if (x == 0 && w == CANVAS_W) {
        memcpy(dst, c->px + (size_t)y * CANVAS_W, (size_t)w * h);     /* 행 전체면 한 번에 */
    } else {
        for (uint32_t r = 0; r < h; r++)
            memcpy(dst + (size_t)r * w, c->px + (size_t)(y + r) * CANVAS_W + x, w);
    }

    unlock_rect(c, x, y, w, h);
    return 0;
}

/* ---- 체크섬, PGM ---- */

uint64_t canvas_checksum(const canvas_t *c)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < (size_t)CANVAS_W * CANVAS_H; i++) {
        h ^= c->px[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

int canvas_save_pgm(const canvas_t *c, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fprintf(f, "P5\n%u %u\n255\n", CANVAS_W, CANVAS_H) > 0
          && fwrite(c->px, 1, (size_t)CANVAS_W * CANVAS_H, f) == (size_t)CANVAS_W * CANVAS_H;
    int e = errno;
    if (fclose(f) != 0) ok = 0;
    if (!ok) { errno = e ? e : EIO; return -1; }
    return 0;
}
