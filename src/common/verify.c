/*
 * verify.c - 검증기: 클라이언트들이 보낸 측정 요청을 스레드 하나로 다시 그려 그림판별 정답 체크섬을 낸다 (요구사항 F-11)
 *
 *   verify [-K 그림판] -M 목록                      실행기(loadgen -M)가 남긴 목록: "번호 시드 그림판 요청수 DRAW% 변 변" 줄들
 *   verify [-K 그림판] [-c N | -s 시드 ...] [-n 요청] [-m DRAW%] [-r 변] [-S 변]
 *          -c N 이면 클라이언트 0..N-1 (시드 번호+1, 그림판 번호 mod 8 → 다시 K 로 나눈 나머지)
 *
 * DRAW 의 더하기는 순서와 무관하므로 처리 순서를 몰라도 정답이 하나로 정해진다. 워밍업은 v = 0 이라 재생하지 않는다.
 * 출력은 서버가 끝날 때 쓰는 줄과 같은 형식이다: "checksum room=<k> <16진수>" 들과 "checksum <16진수>".
 */
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proto.h"
#include "canvas.h"
#include "workload.h"

static void replay(canvas_t *c, const wl_cfg_t *w, uint32_t room, uint64_t n)
{
    wl_req_t r;
    for (uint64_t i = 0; i < n; i++) {
        wl_gen(w, i, &r);
        if (r.type == REQ_DRAW) canvas_draw(c, room % c->nrooms, r.x, r.y, r.w, r.h, r.v);
    }
}

int main(int argc, char **argv)
{
    uint32_t rooms = 8, nc = 0, ns = 0;
    uint64_t seeds[MAX_CLIENTS], n = 10000;
    wl_cfg_t w = { 0, WL_DRAW_PCT, WL_RECT, WL_SNAP };
    const char *manifest = NULL;
    int c;
    while ((c = getopt(argc, argv, "K:M:c:s:n:m:r:S:h")) != -1) {
        switch (c) {
        case 'K': rooms = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'M': manifest = optarg; break;
        case 'c': nc = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 's': if (ns < MAX_CLIENTS) seeds[ns++] = strtoull(optarg, NULL, 10); break;
        case 'n': n = strtoull(optarg, NULL, 10); break;
        case 'm': w.draw_pct = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'r': w.rect = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'S': w.snap = (uint32_t)strtoul(optarg, NULL, 10); break;
        default:
            fprintf(stderr, "사용법: %s [-K 그림판] -M 목록 | [-c N | -s 시드 ...] [-n 요청] [-m DRAW%%] [-r 변] [-S 변]\n", argv[0]);
            return c == 'h' ? 0 : 2;
        }
    }
    if (rooms < 1 || rooms > MAX_ROOMS || nc > MAX_CLIENTS || w.draw_pct > 100 || w.rect < 1 || w.rect > CANVAS_W ||
        w.snap < 1 || w.snap > SNAP_MAX_SIDE) {
        fprintf(stderr, "verify: 잘못된 옵션\n");
        return 2;
    }
    canvas_t cv;
    if (canvas_init(&cv, rooms, TILE_DEFAULT, LOCK_NONE) < 0) { perror("verify"); return 1; }

    if (manifest) {
        FILE *f = fopen(manifest, "r");
        if (!f) { perror(manifest); return 1; }
        unsigned id, room, dp, rc, sc;
        unsigned long long seed, cnt;
        while (fscanf(f, "%u %llu %u %llu %u %u %u", &id, &seed, &room, &cnt, &dp, &rc, &sc) == 7) {
            wl_cfg_t m = { seed, dp, rc, sc };
            replay(&cv, &m, room, cnt);
        }
        fclose(f);
    } else {
        for (uint32_t i = 0; i < nc; i++) {
            w.seed = i + 1ull;
            replay(&cv, &w, (i % 8) % rooms, n);
        }
        for (uint32_t i = 0; i < ns; i++) {
            w.seed = seeds[i];
            replay(&cv, &w, i % rooms, n);
        }
    }
    for (uint32_t r = 0; r < rooms; r++) printf("checksum room=%u %016llx\n", r, (unsigned long long)canvas_checksum(&cv, r));
    printf("checksum %016llx\n", (unsigned long long)canvas_checksum_all(&cv));
    canvas_free(&cv);
    return 0;
}
