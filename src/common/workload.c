/*
 * workload.c - 요청열 생성기
 *
 * 난수는 C 라이브러리의 rand() 대신 splitmix64 를 직접 쓴다. rand() 는 구현마다 수열이 달라
 * 다른 컴퓨터에서 같은 시드로 같은 요청열을 보장하지 못하고, 스레드 안전하지도 않다.
 *
 * DECISION: 요청 하나마다 난수를 늘 4개(종류, x, y, v) 꺼낸다. SNAPSHOT 은 v 를 쓰지 않지만 버린다.
 *   요청 i 가 쓰는 난수의 위치가 앞 요청들의 종류와 상관없이 정해져 요청열을 따라가기 쉽다.
 */
#include <errno.h>
#include <string.h>

#include "workload.h"

static uint64_t splitmix64(uint64_t *s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* 0 ~ n-1 의 정수. 위 32비트에 n 을 곱해 위 32비트를 취한다(나머지 연산보다 치우침이 작다) */
static uint32_t below(uint64_t *s, uint32_t n)
{
    return (uint32_t)(((splitmix64(s) >> 32) * (uint64_t)n) >> 32);
}

int workload_init(workload_t *w, uint64_t seed, uint32_t draw_pct, uint32_t rect, uint32_t snap)
{
    if (draw_pct > 100 || rect < 1 || rect > CANVAS_W || rect > CANVAS_H ||
        snap < 1 || snap > CANVAS_W || snap > CANVAS_H) {
        errno = EINVAL;
        return -1;
    }
    w->state = seed;
    w->draw_pct = draw_pct;
    w->rect = rect;
    w->snap = snap;
    return 0;
}

void workload_next(workload_t *w, wl_req_t *r)
{
    int draw = below(&w->state, 100) < w->draw_pct;
    uint32_t side = draw ? w->rect : w->snap;
    uint16_t x = (uint16_t)below(&w->state, CANVAS_W - side + 1);
    uint16_t y = (uint16_t)below(&w->state, CANVAS_H - side + 1);
    uint8_t  v = (uint8_t)(1 + below(&w->state, 255));

    memset(r, 0, sizeof *r);
    if (draw) {
        r->type = REQ_DRAW;
        r->len = sizeof r->p.draw;
        r->p.draw.x = x;
        r->p.draw.y = y;
        r->p.draw.w = (uint16_t)side;
        r->p.draw.h = (uint16_t)side;
        r->p.draw.v = v;
    } else {
        r->type = REQ_SNAPSHOT;
        r->len = sizeof r->p.snap;
        r->p.snap.x = x;
        r->p.snap.y = y;
        r->p.snap.w = (uint16_t)side;
        r->p.snap.h = (uint16_t)side;
    }
}

size_t workload_resp_len(const wl_req_t *r)
{
    size_t n = sizeof(srv_ts_t);
    if (r->type == REQ_SNAPSHOT)
        n += (size_t)r->p.snap.w * r->p.snap.h;
    return n;
}
