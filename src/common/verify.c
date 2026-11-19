/*
 * verify.c - 정답 체크섬 검증기 (요구사항 F-10)
 *
 * 클라이언트들이 보낸 측정 요청열을 같은 시드와 옵션으로 workload.c 에서 다시 만들고, 스레드 하나로
 * 캔버스에 그려 최종 체크섬을 stdout 에 쓴다. 서버가 끝날 때 출력하는 체크섬과 같은 형식이라 줄째로 비교한다.
 * 서버와 같은 canvas.c 의 canvas_draw, canvas_checksum 을 쓴다 (설계 명세의 캔버스와 락).
 * IPC 를 쓰지 않는다. 어느 버전으로 잰 결과든 이 검증기 하나와 비교한다 (요구사항 F-7).
 *
 * 재생하는 것은 측정 요청뿐이다. 워밍업 요청은 DRAW 의 v 가 0 이라 캔버스를 바꾸지 않고,
 * SNAPSHOT 은 캔버스를 바꾸지 않는다. 그래도 SNAPSHOT 의 요청도 workload_next 로 꺼내야 뒤의 수열이 맞는다.
 * DRAW 는 더하기(256 으로 나눈 나머지)라서 클라이언트 사이의 순서는 결과에 영향이 없다.
 *
 * DECISION: 모든 클라이언트가 같은 -n, -m, -r, -S 로 돌았다고 보고 시드만 클라이언트마다 받는다.
 *   검증 시험(T-10, T-12)과 2단계 실험이 모두 이렇게 돌린다. 옵션이 다른 클라이언트는 검증기를 따로 돌려 비교할 수 없다.
 * DECISION: -c K 는 클라이언트 기본 시드(번호 + 1)를 번호 0 ~ K-1 에 대해 더한다. -s 와 함께 쓰면 둘 다 더한다.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "canvas.h"
#include "workload.h"

#define MAX_REQUESTS 10000000ull    /* client.c 의 -n 한도와 같다 */
#define MAX_SEEDS    64u            /* 시드 개수 한도. 동시 클라이언트(MAX_CLIENTS)보다 넉넉히 */

static void usage(const char *prog)
{
    fprintf(stderr,
            "사용법: %s [-s 시드]... [-c 클라이언트수] [-n 요청수] [-m DRAW%%] [-r 변] [-S 변]\n"
            "  -s N   재생할 클라이언트의 시드. 클라이언트마다 하나씩 여러 번 준다 (최대 %u개)\n"
            "  -c K   클라이언트 번호 0~K-1 의 기본 시드(번호 + 1)를 더한다, 1~%u\n"
            "  -n N   클라이언트마다 잰 요청 수 (기본 20000)\n"
            "  -m N   DRAW 비율 %% (기본 %u)\n"
            "  -r N   DRAW 사각형 한 변 1~%u (기본 %u)\n"
            "  -S N   SNAPSHOT 영역 한 변 1~%u (기본 %u)\n"
            "시드를 하나도 주지 않으면 빈 캔버스의 체크섬을 쓴다.\n"
            "출력: checksum <16자리 16진수>\n",
            prog, MAX_SEEDS, MAX_CLIENTS, WL_DRAW_PCT_DEFAULT,
            CANVAS_W, WL_RECT_DEFAULT, CANVAS_W, WL_SNAP_DEFAULT);
}

static int parse_u64(const char *s, uint64_t *out)
{
    char *end;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno || end == s || *end || s[0] == '-') return -1;
    *out = v;
    return 0;
}

int main(int argc, char **argv)
{
    uint64_t seeds[MAX_SEEDS];
    unsigned nseeds = 0;
    uint64_t n = 20000;
    uint32_t draw_pct = WL_DRAW_PCT_DEFAULT, rect = WL_RECT_DEFAULT, snap = WL_SNAP_DEFAULT;

    uint64_t v;
    int opt;
    while ((opt = getopt(argc, argv, "s:c:n:m:r:S:h")) != -1) {
        switch (opt) {
        case 's':
            if (parse_u64(optarg, &v) < 0 || nseeds >= MAX_SEEDS) goto bad;
            seeds[nseeds++] = v;
            break;
        case 'c':
            if (parse_u64(optarg, &v) < 0 || v < 1 || v > MAX_CLIENTS || nseeds + v > MAX_SEEDS) goto bad;
            for (uint64_t id = 0; id < v; id++) seeds[nseeds++] = id + 1;   /* client.c 의 기본 시드 */
            break;
        case 'n': if (parse_u64(optarg, &v) < 0 || v < 1 || v > MAX_REQUESTS) goto bad; n = v; break;
        case 'm': if (parse_u64(optarg, &v) < 0 || v > 100) goto bad; draw_pct = (uint32_t)v; break;
        case 'r': if (parse_u64(optarg, &v) < 0 || v < 1 || v > CANVAS_W) goto bad; rect = (uint32_t)v; break;
        case 'S': if (parse_u64(optarg, &v) < 0 || v < 1 || v > CANVAS_W) goto bad; snap = (uint32_t)v; break;
        case 'h': usage(argv[0]); return 0;
        default: goto bad;
        }
    }
    if (optind != argc) goto bad;

    workload_t wl;
    if (workload_init(&wl, 0, draw_pct, rect, snap) < 0) goto bad;

    canvas_t *c = canvas_create(LOCK_NONE, TILE_DEFAULT);     /* 스레드 하나라 락이 필요 없다 */
    if (!c) {
        fprintf(stderr, "verify: 캔버스를 만들지 못함: %s\n", strerror(errno));
        return 1;
    }

    uint64_t n_draw = 0;
    for (unsigned k = 0; k < nseeds; k++) {
        workload_init(&wl, seeds[k], draw_pct, rect, snap);
        for (uint64_t i = 0; i < n; i++) {
            wl_req_t r;
            workload_next(&wl, &r);
            if (r.type != REQ_DRAW) continue;
            if (canvas_draw(c, &r.p.draw, NULL) < 0) {      /* 생성기는 잘못된 영역을 만들지 않는다 */
                fprintf(stderr, "verify: 시드 %" PRIu64 "의 %" PRIu64 "번째 요청이 캔버스를 벗어났다\n", seeds[k], i);
                canvas_destroy(c);
                return 1;
            }
            n_draw++;
        }
    }

    printf("checksum " CANVAS_CHECKSUM_FMT "\n", canvas_checksum(c));
    fprintf(stderr, "verify: 시드 %u개 × 요청 %" PRIu64 "개 재생, DRAW %" PRIu64 "개\n", nseeds, nseeds ? n : 0, n_draw);
    canvas_destroy(c);
    return 0;

bad:
    fprintf(stderr, "verify: 잘못된 옵션이다 (도움말: %s -h)\n", argv[0]);
    return 1;
}
