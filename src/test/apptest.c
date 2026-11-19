/*
 * apptest.c - 2단계 서버 시험 도구 (scripts/test-app.sh 가 쓴다)
 *
 * 세 IPC 버전에 똑같이 링크되어 bin/apptest_<ipc> 가 된다. transport.h 의 함수만 부른다.
 * 정상 클라이언트(client.c)가 보내지 않는 요청과, 응답 내용까지 확인해야 하는 검사를 맡는다.
 *
 *   apptest bad ID                    잘못된 요청(캔버스 밖, 크기 0, payload 길이 틀림, 모르는 종류)을 보내고
 *                                     모두 RESP_ERROR(서버 시각만)로 오는지 본다 (T-11)
 *   apptest stats ID                  STATS 를 보내 "n_draw n_snapshot bytes_sent n_clients n_workers" 를 쓴다 (T-16)
 *   apptest snap ID K N M R S F CNT   클라이언트 K 개(기본 시드 1..K)가 -n N -m M -r R -S S 로 그린 정답 캔버스를
 *                                     검증기처럼 만들어 두고, 캔버스 전체(1MB) SNAPSHOT 을 CNT 개, 동시에 F 개씩 보내
 *                                     모든 응답이 정답과 같은지 본다 (T-15)
 *
 * 모든 응답의 서버 시각이 t_recv ≤ t_deq ≤ t_lock ≤ t_send 인지도 본다 (T-16).
 * 종료 코드: 0 성공, 2 열기 실패, 3 송수신 실패, 5 응답이 틀림
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "canvas.h"
#include "transport.h"
#include "util.h"
#include "workload.h"

static uint64_t next_id;
static uint32_t my_id;
static uint8_t *rbuf;

static int send_req(uint32_t type, const void *p, uint32_t len, uint64_t *id)
{
    msg_hdr_t h = { .type = type, .client_id = my_id, .req_id = next_id++, .payload_len = len };
    if (id) *id = h.req_id;
    return tp_send_req(&h, p);
}

/* 응답 하나. 서버 시각 순서가 틀리면 -2 */
static ssize_t recv_resp(msg_hdr_t *h)
{
    ssize_t r = tp_recv_resp(h, rbuf, MAX_PAYLOAD);
    if (r < 0) return -1;
    if ((size_t)r < sizeof(srv_ts_t)) return -2;
    srv_ts_t ts;
    memcpy(&ts, rbuf, sizeof ts);
    if (!(ts.t_recv <= ts.t_deq && ts.t_deq <= ts.t_lock && ts.t_lock <= ts.t_send)) return -2;
    return r;
}

static int hello(void)
{
    msg_hdr_t h;
    uint64_t id;
    if (send_req(REQ_HELLO, NULL, 0, &id) < 0) return -1;
    for (;;) {
        ssize_t r = recv_resp(&h);
        if (r < 0) return -1;
        if (h.req_id == id) return h.type == RESP_OK ? 0 : -1;
    }
}

static int bad_mode(void)
{
    struct { uint32_t type; uint8_t p[16]; uint32_t len; const char *what; } cases[8];
    int n = 0;
    draw_req_t d;
    snap_req_t s;

#define ADD_DRAW(X, Y, W, H, WHAT) do { memset(&d, 0, sizeof d); d.x = X; d.y = Y; d.w = W; d.h = H; d.v = 7; \
        cases[n].type = REQ_DRAW; memcpy(cases[n].p, &d, sizeof d); cases[n].len = sizeof d; cases[n++].what = WHAT; } while (0)
#define ADD_SNAP(X, Y, W, H, WHAT) do { s.x = X; s.y = Y; s.w = W; s.h = H; \
        cases[n].type = REQ_SNAPSHOT; memcpy(cases[n].p, &s, sizeof s); cases[n].len = sizeof s; cases[n++].what = WHAT; } while (0)

    ADD_DRAW(1000, 0, 100, 1, "DRAW 오른쪽 밖");
    ADD_DRAW(0, 1023, 1, 2, "DRAW 아래쪽 밖");
    ADD_DRAW(10, 10, 0, 5, "DRAW 폭 0");
    ADD_SNAP(0, 0, 1025, 1, "SNAPSHOT 폭 초과");
    ADD_SNAP(5, 5, 3, 0, "SNAPSHOT 높이 0");
    ADD_DRAW(0, 0, 4, 4, "DRAW payload 길이 틀림");
    cases[n - 1].len = 8;
    cases[n].type = 77; cases[n].len = 0; cases[n++].what = "모르는 종류";

    int bad = 0;
    for (int i = 0; i < n; i++) {
        uint64_t id;
        msg_hdr_t h;
        if (send_req(cases[i].type, cases[i].p, cases[i].len, &id) < 0) return 3;
        ssize_t r;
        do r = recv_resp(&h); while (r >= 0 && h.req_id != id);
        if (r == -1) return 3;
        if (r == -2 || h.type != RESP_ERROR || (size_t)r != sizeof(srv_ts_t)) {
            fprintf(stderr, "apptest: %s 에 오류 응답이 오지 않았다\n", cases[i].what);
            bad++;
        }
    }
    printf("bad %d/%d\n", n - bad, n);
    return bad ? 5 : 0;
}

static int stats_mode(void)
{
    uint64_t id;
    msg_hdr_t h;
    if (send_req(REQ_STATS, NULL, 0, &id) < 0) return 3;
    ssize_t r;
    do r = recv_resp(&h); while (r >= 0 && h.req_id != id);
    if (r < 0) return r == -2 ? 5 : 3;
    if (h.type != RESP_OK || (size_t)r != sizeof(srv_ts_t) + sizeof(stats_resp_t)) return 5;
    stats_resp_t st;
    memcpy(&st, rbuf + sizeof(srv_ts_t), sizeof st);
    printf("%" PRIu64 " %" PRIu64 " %" PRIu64 " %u %u\n", st.n_draw, st.n_snapshot, st.bytes_sent, st.n_clients, st.n_workers);
    return 0;
}

static int snap_mode(unsigned k, uint64_t n, unsigned m, unsigned rect, unsigned snap, unsigned f, unsigned cnt)
{
    /* 정답 캔버스: verify.c 와 같은 재생 */
    canvas_t *ref = canvas_create(LOCK_NONE, TILE_DEFAULT);
    uint8_t *want = malloc(CANVAS_W * CANVAS_H);
    if (!ref || !want) return 3;
    for (unsigned c = 0; c < k; c++) {
        workload_t wl;
        workload_init(&wl, c + 1, m, rect, snap);
        for (uint64_t i = 0; i < n; i++) {
            wl_req_t r;
            workload_next(&wl, &r);
            if (r.type == REQ_DRAW) canvas_draw(ref, &r.p.draw, NULL);
        }
    }
    snap_req_t all = { 0, 0, CANVAS_W, CANVAS_H };
    canvas_snapshot(ref, &all, want, NULL);

    unsigned sent = 0, got = 0, bad = 0;
    while (got < cnt) {
        while (sent < cnt && sent - got < f) {
            if (send_req(REQ_SNAPSHOT, &all, sizeof all, NULL) < 0) return 3;
            sent++;
        }
        msg_hdr_t h;
        ssize_t r = recv_resp(&h);
        if (r == -1) return 3;
        got++;
        if (r == -2 || h.type != RESP_OK || (size_t)r != sizeof(srv_ts_t) + CANVAS_W * CANVAS_H ||
            memcmp(rbuf + sizeof(srv_ts_t), want, CANVAS_W * CANVAS_H) != 0)
            bad++;
    }
    printf("snap %u/%u\n", cnt - bad, cnt);
    free(want);
    canvas_destroy(ref);
    return bad ? 5 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "사용법: %s bad ID | stats ID | snap ID K N M R S F CNT\n", argv[0]);
        return 64;
    }
    my_id = (uint32_t)strtoul(argv[2], NULL, 10);
    next_id = (uint64_t)getpid() << 32;
    rbuf = malloc(MAX_PAYLOAD);
    if (!rbuf) return 3;
    if (tp_client_open(my_id) < 0) {
        fprintf(stderr, "apptest: 열지 못함: %s\n", strerror(errno));
        return 2;
    }
    int rc;
    if (hello() < 0) rc = 3;
    else if (strcmp(argv[1], "bad") == 0) rc = bad_mode();
    else if (strcmp(argv[1], "stats") == 0) rc = stats_mode();
    else if (strcmp(argv[1], "snap") == 0 && argc == 10)
        rc = snap_mode((unsigned)atoi(argv[3]), strtoull(argv[4], NULL, 10), (unsigned)atoi(argv[5]),
                       (unsigned)atoi(argv[6]), (unsigned)atoi(argv[7]), (unsigned)atoi(argv[8]), (unsigned)atoi(argv[9]));
    else rc = 64;
    msg_hdr_t bye = { .type = REQ_BYE, .client_id = my_id, .req_id = next_id++ };
    tp_send_req(&bye, NULL);
    tp_client_close();
    free(rbuf);
    return rc;
}
