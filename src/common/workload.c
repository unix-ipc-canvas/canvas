/*
 * workload.c - 시드로 요청열을 만드는 생성기
 *
 * splitmix64 로 (시드, 번호)에서 64비트 난수 두 개를 얻어 종류, 위치, 값을 정한다.
 * DRAW 위치는 사각형이 그림판 안에 들어가는 범위에서 균등, 값은 1~255 균등. SNAPSHOT 위치도 균등.
 */
#include "proto.h"
#include "workload.h"

static uint64_t splitmix64(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

void wl_gen(const wl_cfg_t *cfg, uint64_t i, wl_req_t *out)
{
    uint64_t a = splitmix64(cfg->seed * 0x2545f4914f6cdd1dull + i * 2);
    uint64_t b = splitmix64(cfg->seed * 0x2545f4914f6cdd1dull + i * 2 + 1);
    int draw = (uint32_t)(a % 100) < cfg->draw_pct;
    uint32_t side = draw ? cfg->rect : cfg->snap;
    uint32_t span = CANVAS_W - side + 1;             /* 왼쪽 위 칸이 놓일 수 있는 수 */
    out->type = draw ? REQ_DRAW : REQ_SNAPSHOT;
    out->x = (uint16_t)((b & 0xffffffffu) % span);
    out->y = (uint16_t)((b >> 32) % span);
    out->w = out->h = (uint16_t)side;
    out->v = draw ? (uint8_t)(1 + (a >> 32) % 255) : 0;
}
