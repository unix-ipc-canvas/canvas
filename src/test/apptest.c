/*
 * apptest.c - 서버 시험 도구 (scripts/verify.sh 의 T-11, T-15 가 쓴다)
 *
 * 세 IPC 버전에 똑같이 링크되어 bin/apptest_<ipc> 가 된다. transport.h 의 함수만 부른다.
 * 응용 클라이언트(client.c)가 만들지 않는 요청을 보내 서버를 확인한다.
 *
 *   apptest bad ID
 *       잘못된 요청(영역 밖, 크기 0, payload 길이 틀림, 모르는 종류)마다 RESP_ERROR 와 서버 시각만 든 응답이 오는지,
 *       그 뒤에도 정상 SNAPSHOT 에 RESP_OK 가 오는지 본다. 캔버스는 바꾸지 않는다(체크섬은 verify.sh 가 본다).
 *   apptest snap ID COUNT F K N
 *       시드 1..K 의 클라이언트가 요청 N 개씩 그린 캔버스(client.c 기본 옵션, verify 와 같은 재생)를 정답으로 만들고,
 *       SNAPSHOT COUNT 개를 동시에 F 개씩 띄워 보내 응답 내용을 정답의 같은 영역과 바이트 단위로 비교한다.
 *       짝수 번째는 캔버스 전체(1MB), 홀수 번째는 번호로 정해지는 임의 영역이다.
 *
 * 종료 코드  0 모두 맞음, 1 사용법, 2 열기 실패, 3 송신 실패, 4 수신 실패, 5 응답이 틀림
 * 어느 경로로 끝나든 tp_client_close 를 부른다 (transport.h 의 약속). 결과는 stdout 에 한 줄로 쓴다.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "canvas.h"
#include "transport.h"
#include "util.h"
#include "workload.h"

#define MAX_COUNT 100000u
#define MAX_F     64u

static uint32_t id;
static uint64_t base;
static uint8_t *rbuf;
static char eb[128];

static int send_req_room(uint32_t type, uint64_t rid, uint32_t room, const void *p, uint32_t len)
{
    msg_hdr_t h = { .type = type, .client_id = id, .req_id = rid, .payload_len = len, .reserved = room };
    if (tp_send_req(&h, p) < 0) {
        printf("FAIL 요청 보내기: %s\n", err_str(errno, eb, sizeof eb));
        return -1;
    }
    return 0;
}

static int send_req(uint32_t type, uint64_t rid, const void *p, uint32_t len)
{
    return send_req_room(type, rid, 0, p, len);
}

/* 응답 하나를 받는다. 서버 시각 순서까지 본다. 실패하면 -1 (출력은 여기서 한다) */
static ssize_t recv_resp(msg_hdr_t *h)
{
    ssize_t r = tp_recv_resp(h, rbuf, MAX_PAYLOAD);
    if (r < 0) {
        printf("FAIL 응답 받기: %s\n", err_str(errno, eb, sizeof eb));
        return -1;
    }
    srv_ts_t ts;
    if ((size_t)r < sizeof ts) {
        printf("FAIL 응답이 서버 시각(32B)보다 짧다: %zd B\n", r);
        return -1;
    }
    memcpy(&ts, rbuf, sizeof ts);
    if (!(ts.t_recv && ts.t_recv <= ts.t_deq && ts.t_deq <= ts.t_lock && ts.t_lock <= ts.t_send)) {
        printf("FAIL 서버 시각 순서가 맞지 않는다 (req_id %llu)\n", (unsigned long long)(h->req_id - base));
        return -1;
    }
    return r;
}

/* HELLO 를 보내고 응답을 기다린다 */
static int hello(void)
{
    msg_hdr_t h;
    if (send_req(REQ_HELLO, base, NULL, 0) < 0) return 3;
    if (recv_resp(&h) < 0) return 4;
    if (h.type != RESP_OK || h.req_id != base) {
        printf("FAIL HELLO 응답 type %u req_id %llu\n", h.type, (unsigned long long)(h.req_id - base));
        return 5;
    }
    return 0;
}

static void bye(void)
{
    msg_hdr_t h = { .type = REQ_BYE, .client_id = id, .req_id = base + MAX_COUNT + 1 };
    tp_send_req(&h, NULL);
}

/* ---- bad ---- */

static int expect_err(const char *what, uint32_t type, uint64_t rid, uint32_t room, const void *p, uint32_t len)
{
    if (send_req_room(type, rid, room, p, len) < 0) return 3;
    msg_hdr_t h;
    ssize_t r = recv_resp(&h);
    if (r < 0) return 4;
    if (h.type != RESP_ERROR || r != (ssize_t)sizeof(srv_ts_t) || h.req_id != rid) {
        printf("FAIL %s: type %u, %zd B (RESP_ERROR, 32B 이어야 함)\n", what, h.type, r);
        return 5;
    }
    return 0;
}

static int run_bad(void)
{
    uint64_t rid = base + 1;
    /* 1. 없는 방으로 HELLO → 거부되어야 하며 접속 등록도 되지 않아야 한다 */
    int rc = expect_err("HELLO 없는 방 1", REQ_HELLO, rid++, 1, NULL, 0);
    if (rc) return rc;
    /* 2. HELLO 성공 전(미등록 상태)의 DRAW·SNAPSHOT → 모두 거부되고 캔버스가 변하면 안 된다 */
    draw_req_t d_ok = { .x = 0, .y = 0, .w = 4, .h = 4, .v = 9 };
    snap_req_t s_ok = { .x = 0, .y = 0, .w = 4, .h = 4 };
    if ((rc = expect_err("HELLO 전 DRAW", REQ_DRAW, rid++, 0, &d_ok, sizeof d_ok))) return rc;
    if ((rc = expect_err("HELLO 전 SNAPSHOT", REQ_SNAPSHOT, rid++, 0, &s_ok, sizeof s_ok))) return rc;

    /* 3. 정상 HELLO 로 방 0 에 등록 */
    if ((rc = hello())) return rc;

    /* 4. 등록 뒤 잘못된 요청들 (좌표·크기·길이·없는 방·모르는 종류) */
    struct { const char *what; uint32_t type; uint32_t room; draw_req_t d; uint32_t len; } bad[] = {
        { "DRAW x+w > 1024",      REQ_DRAW,     0, { .x = 1000, .y = 0, .w = 25, .h = 1, .v = 7 }, sizeof(draw_req_t) },
        { "DRAW y+h > 1024",      REQ_DRAW,     0, { .x = 0, .y = 1023, .w = 1, .h = 2, .v = 7 }, sizeof(draw_req_t) },
        { "DRAW x = 1024",        REQ_DRAW,     0, { .x = 1024, .y = 0, .w = 1, .h = 1, .v = 7 }, sizeof(draw_req_t) },
        { "DRAW 65535",           REQ_DRAW,     0, { .x = 65535, .y = 65535, .w = 65535, .h = 65535, .v = 7 }, sizeof(draw_req_t) },
        { "DRAW w = 0",           REQ_DRAW,     0, { .x = 5, .y = 5, .w = 0, .h = 3, .v = 7 }, sizeof(draw_req_t) },
        { "DRAW h = 0",           REQ_DRAW,     0, { .x = 5, .y = 5, .w = 3, .h = 0, .v = 7 }, sizeof(draw_req_t) },
        { "DRAW payload 8B",      REQ_DRAW,     0, { .x = 0, .y = 0, .w = 4, .h = 4, .v = 7 }, 8 },
        { "DRAW 없는 방 1",        REQ_DRAW,     1, { .x = 0, .y = 0, .w = 4, .h = 4, .v = 7 }, sizeof(draw_req_t) },
        { "SNAPSHOT x+w > 1024",  REQ_SNAPSHOT, 0, { .x = 1000, .y = 0, .w = 25, .h = 1 }, sizeof(snap_req_t) },
        { "SNAPSHOT y+h > 1024",  REQ_SNAPSHOT, 0, { .x = 0, .y = 1023, .w = 1, .h = 2 }, sizeof(snap_req_t) },
        { "SNAPSHOT w = 0",       REQ_SNAPSHOT, 0, { .x = 5, .y = 5, .w = 0, .h = 3 }, sizeof(snap_req_t) },
        { "SNAPSHOT h = 0",       REQ_SNAPSHOT, 0, { .x = 5, .y = 5, .w = 3, .h = 0 }, sizeof(snap_req_t) },
        { "SNAPSHOT payload 4B",  REQ_SNAPSHOT, 0, { .x = 0, .y = 0, .w = 4, .h = 4 }, 4 },
        { "SNAPSHOT 없는 방 1",    REQ_SNAPSHOT, 1, { .x = 0, .y = 0, .w = 4, .h = 4 }, sizeof(snap_req_t) },
        { "STATS payload 4B",     REQ_STATS,    0, { 0 }, 4 },
        { "모르는 종류 77",        77,           0, { 0 }, 0 },
    };
    unsigned n = sizeof bad / sizeof bad[0];
    for (unsigned i = 0; i < n; i++) {
        /* snap_req_t 는 draw_req_t 의 앞 8B 와 같은 배치다(x, y, w, h) */
        if ((rc = expect_err(bad[i].what, bad[i].type, rid++, bad[i].room, &bad[i].d, bad[i].len))) return rc;
    }
    /* 오류 뒤에도 정상 요청은 처리된다 */
    snap_req_t s = { 1020, 1020, 4, 4 };
    if (send_req(REQ_SNAPSHOT, base + 100, &s, sizeof s) < 0) return 3;
    msg_hdr_t h;
    ssize_t r = recv_resp(&h);
    if (r < 0) return 4;
    if (h.type != RESP_OK || r != (ssize_t)(sizeof(srv_ts_t) + 16)) {
        printf("FAIL 오류 뒤 정상 SNAPSHOT: type %u, %zd B\n", h.type, r);
        return 5;
    }
    printf("ok   잘못된 요청 %u종 모두 RESP_ERROR, 그 뒤 정상 요청 RESP_OK\n", n + 3);
    return 0;
}

/* ---- snap ---- */

static uint64_t mix(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

static snap_req_t rect_of(uint32_t k)
{
    if (k % 2 == 0) return (snap_req_t){ 0, 0, CANVAS_W, CANVAS_H };
    uint64_t r = mix(((uint64_t)id << 32) | k);
    uint16_t w = (uint16_t)(1 + r % CANVAS_W), h = (uint16_t)(1 + (r >> 16) % CANVAS_H);
    uint16_t x = (uint16_t)((r >> 32) % (CANVAS_W - w + 1)), y = (uint16_t)((r >> 48) % (CANVAS_H - h + 1));
    return (snap_req_t){ x, y, w, h };
}

static int run_snap(uint32_t count, uint32_t f, uint32_t k, uint64_t n)
{
    /* 정답 캔버스: verify 와 같이 시드 1..k 의 DRAW 를 재생한다 */
    canvas_t *c = canvas_create(LOCK_NONE, TILE_DEFAULT);
    uint8_t *exp = malloc((size_t)CANVAS_W * CANVAS_H);
    if (!c || !exp) {
        printf("FAIL 메모리 부족\n");
        return 1;
    }
    workload_t wl;
    for (uint32_t s = 1; s <= k; s++) {
        workload_init(&wl, s, WL_DRAW_PCT_DEFAULT, WL_RECT_DEFAULT, WL_SNAP_DEFAULT);
        for (uint64_t i = 0; i < n; i++) {
            wl_req_t r;
            workload_next(&wl, &r);
            if (r.type == REQ_DRAW) canvas_draw(c, &r.p.draw, NULL);
        }
    }
    snap_req_t all = { 0, 0, CANVAS_W, CANVAS_H };
    canvas_snapshot(c, &all, exp, NULL);
    canvas_destroy(c);

    int rc = hello();
    if (rc) { free(exp); return rc; }

    /* 응답이 F 개 이하로 띄워져 있게 하나 받으면 하나 보낸다. 요청은 작아 보내다 막히지 않는다 */
    uint32_t sent = 0, got = 0, full = 0;
    bool *seen = calloc(count, sizeof *seen);
    if (!seen) { free(exp); printf("FAIL 메모리 부족\n"); return 1; }
    for (; sent < count && sent < f; sent++) {
        snap_req_t q = rect_of(sent);
        if (send_req(REQ_SNAPSHOT, base + 1 + sent, &q, sizeof q) < 0) { rc = 3; goto out; }
    }
    while (got < count) {
        msg_hdr_t h;
        ssize_t r = recv_resp(&h);
        if (r < 0) { rc = 4; goto out; }
        uint64_t k1 = h.req_id - base - 1;
        if (h.req_id <= base || k1 >= sent || seen[k1]) {
            printf("FAIL 띄우지 않았거나 두 번 온 응답 (req_id %llu)\n", (unsigned long long)(h.req_id - base));
            rc = 5; goto out;
        }
        seen[k1] = true;
        snap_req_t q = rect_of((uint32_t)k1);
        if (h.type != RESP_OK || r != (ssize_t)(sizeof(srv_ts_t) + (size_t)q.w * q.h)) {
            printf("FAIL %llu 번째 SNAPSHOT (%u,%u,%u,%u): type %u, %zd B\n", (unsigned long long)k1,
                   q.x, q.y, q.w, q.h, h.type, r);
            rc = 5; goto out;
        }
        const uint8_t *p = rbuf + sizeof(srv_ts_t);
        for (uint32_t row = 0; row < q.h; row++) {
            if (memcmp(p + (size_t)row * q.w, exp + (size_t)(q.y + row) * CANVAS_W + q.x, q.w) != 0) {
                printf("FAIL %llu 번째 SNAPSHOT (%u,%u,%u,%u) 의 %u 번째 줄이 정답과 다르다\n",
                       (unsigned long long)k1, q.x, q.y, q.w, q.h, row);
                rc = 5; goto out;
            }
        }
        if (q.w == CANVAS_W && q.h == CANVAS_H) full++;
        got++;
        if (sent < count) {
            snap_req_t nq = rect_of(sent);
            if (send_req(REQ_SNAPSHOT, base + 1 + sent, &nq, sizeof nq) < 0) { rc = 3; goto out; }
            sent++;
        }
    }
    printf("ok   SNAPSHOT %u개(캔버스 전체 %u개) 동시 %u개씩, 모두 정답과 같다\n", count, full, f);
out:
    free(seen);
    free(exp);
    return rc;
}

static bool parse_u(const char *s, unsigned long lo, unsigned long hi, unsigned long *out)
{
    char *end;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno || end == s || *end || s[0] == '-' || v < lo || v > hi) return false;
    *out = v;
    return true;
}

int main(int argc, char **argv)
{
    unsigned long v[5];
    bool is_bad = argc == 3 && strcmp(argv[1], "bad") == 0;
    bool is_snap = argc == 7 && strcmp(argv[1], "snap") == 0;
    bool ok = (is_bad || is_snap) && parse_u(argv[2], 0, MAX_CLIENTS - 1, &v[0]);
    if (ok && is_snap)
        ok = parse_u(argv[3], 1, MAX_COUNT, &v[1]) && parse_u(argv[4], 1, MAX_F, &v[2]) &&
             parse_u(argv[5], 0, MAX_CLIENTS, &v[3]) && parse_u(argv[6], 1, 10000000, &v[4]);
    if (!ok) {
        fprintf(stderr, "usage: %s bad ID | %s snap ID COUNT F K N\n", argv[0], argv[0]);
        return 1;
    }
    id = (uint32_t)v[0];
    base = (uint64_t)getpid() << 32;
    rbuf = malloc(MAX_PAYLOAD);
    if (!rbuf) return 1;
    if (tp_client_open(id) < 0) {
        printf("FAIL 연결 (번호 %u): %s\n", id, err_str(errno, eb, sizeof eb));
        free(rbuf);
        return 2;
    }
    int rc = is_bad ? run_bad() : run_snap((uint32_t)v[1], (uint32_t)v[2], (uint32_t)v[3], v[4]);
    if (rc == 0) bye();
    tp_client_close();
    free(rbuf);
    return rc;
}
