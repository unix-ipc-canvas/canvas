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

#endif /* CANVAS_MODULE_H */
