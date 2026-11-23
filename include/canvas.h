/*
 * canvas.h - 캔버스 모듈 (서버와 검증기가 함께 쓴다)
 *
 * 서버 담당이 canvas.c 를 구현하면서 함수 선언을 이 파일에 더한다. proto.h, transport.h 와 달리
 * owner 승인 없이 담당이 바꾼다.
 *
 * 타일은 락의 단위다. 타일 한 변은 서버 실행 옵션(-t)으로 정한다 (요구사항 C-9: 설계 선택은 실행 옵션).
 * 다시 빌드하지 않고 잠금 분할도를 바꿔 잴 수 있다. 여기에는 기본값, 허용 범위, 타일 번호 계산만 둔다.
 */
#ifndef CANVAS_MODULE_H   /* CANVAS_H 는 proto.h 의 캔버스 높이라서 다른 이름을 쓴다 */
#define CANVAS_MODULE_H

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "proto.h"

#define TILE_DEFAULT 64u            /* -t 기본값. 64 × 64칸 타일 256개 */
#define TILE_MIN     16u            /* 타일 4096개 */
#define TILE_MAX     CANVAS_W       /* 타일 1개 = 캔버스 전체 */

/* 타일 한 변으로 쓸 수 있는 값인가: TILE_MIN 이상 TILE_MAX 이하의 2의 거듭제곱 (1024 를 나누어떨어지게 한다) */
static inline bool tile_size_ok(uint32_t t)
{
    return t >= TILE_MIN && t <= TILE_MAX && (t & (t - 1)) == 0;
}

static inline uint32_t tile_cols(uint32_t t)  { return CANVAS_W / t; }
static inline uint32_t tile_count(uint32_t t) { return (CANVAS_W / t) * (CANVAS_H / t); }

/* (x, y) 칸이 속한 타일 번호. 행 우선. 여러 타일을 잡을 때는 이 번호의 오름차순으로 잡는다 (교착 방지) */
static inline uint32_t tile_of(uint32_t t, uint32_t x, uint32_t y)
{
    return (y / t) * (CANVAS_W / t) + (x / t);
}

/* ---- 락 방식 (서버 옵션 -l) ---- */

typedef enum {
    LOCK_GLOBAL,    /* 캔버스 전체에 mutex 1개 */
    LOCK_TILE,      /* 타일마다 mutex */
    LOCK_TILE_RW,   /* 타일마다 rwlock. DRAW 는 쓰기, SNAPSHOT 은 읽기로 잡는다 */
    LOCK_NONE,      /* 잠그지 않는다. 경쟁 상태 시연(T-13)과 스레드 하나로 도는 검증기 전용 */
} lock_mode_t;

/* "global", "tile", "tile-rw", "none" → lock_mode_t. 성공 0, 모르는 이름이면 -1 */
int lock_mode_parse(const char *s, lock_mode_t *out);
const char *lock_mode_name(lock_mode_t m);

/* ---- 캔버스 ---- */

typedef struct canvas canvas_t;

/*
 * 0 으로 채운 캔버스와 락을 만든다 (요구사항 F-1). tile 은 tile_size_ok 를 만족해야 한다.
 * 실패하면 NULL (errno: 잘못된 값 EINVAL, 메모리 ENOMEM).
 * 서버 시작 때 한 번만 부른다. 측정 구간 밖이다.
 */
canvas_t *canvas_create(lock_mode_t mode, uint32_t tile);
void      canvas_destroy(canvas_t *c);

lock_mode_t canvas_lock_mode(const canvas_t *c);
uint32_t    canvas_tile(const canvas_t *c);

/* 영역이 캔버스 안에 있고 w, h 가 0 이 아닌가 (요구사항 F-5) */
bool canvas_rect_ok(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/*
 * DRAW: 영역의 각 칸에 v 를 더한다 (256 으로 나눈 나머지, 요구사항 F-2).
 * 영역에 걸친 락을 타일 번호 오름차순으로 모두 잡은 뒤 그리고, 그린 뒤 바로 놓는다.
 * t_lock 이 NULL 이 아니면 락을 모두 잡은 시각(now_ns)을 쓴다 (요구사항 F-6).
 * 영역이 잘못되면 캔버스를 바꾸지 않고 -1, errno = EINVAL. 락은 잡지 않으며 t_lock 에는 부른 시각을 쓴다.
 * 여러 워커가 동시에 불러도 된다 (LOCK_NONE 제외).
 */
int canvas_draw(canvas_t *c, const draw_req_t *d, uint64_t *t_lock);

/*
 * SNAPSHOT: 영역을 out 에 행 순서대로 w × h 바이트 복사한다 (요구사항 F-3).
 * 락은 복사하는 동안만 쥔다. 응답은 락을 놓은 뒤 보낸다 (설계 명세의 "락을 쥔 채 전송").
 * out 은 w × h 바이트 이상이어야 한다. t_lock 과 오류는 canvas_draw 와 같다.
 */
int canvas_snapshot(canvas_t *c, const snap_req_t *s, void *out, uint64_t *t_lock);

/*
 * 캔버스 전체 1MB 의 체크섬. FNV-1a 64비트 (offset basis 0xcbf29ce484222325, prime 0x100000001b3).
 * 서버와 검증기가 이 함수 하나를 쓴다. 0 으로 채운 캔버스의 값은 CANVAS_EMPTY_CHECKSUM 이다.
 * 락을 잡지 않는다. 그리는 스레드가 모두 끝난 뒤에 부른다 (서버 종료 때, 검증기).
 */
uint64_t canvas_checksum(const canvas_t *c);

/* 방 여러 개의 체크섬을 하나로. n == 1 이면 sums[0] 그대로, 아니면 방 순서대로 이은 체크섬 값 8B 들의 FNV-1a */
uint64_t canvas_checksum_rooms(const uint64_t *sums, uint32_t n);

/* 캔버스를 1024 × 1024 회색조 PGM(P5) 파일로 저장한다 (요구사항 F-9). 락을 잡지 않는다. 성공 0, 실패 -1 */
int canvas_save_pgm(const canvas_t *c, const char *path);

/* 0 으로 채운 캔버스(요청 없이 끝낸 서버)의 체크섬. T-17 이 검증기의 빈 캔버스 값과 비교한다 */
#define CANVAS_EMPTY_CHECKSUM UINT64_C(0xa96777069d622325)

/* 체크섬을 출력할 때 쓰는 형식: printf("checksum " CANVAS_CHECKSUM_FMT "\n", v) */
#define CANVAS_CHECKSUM_FMT "%016" PRIx64

#endif /* CANVAS_MODULE_H */
