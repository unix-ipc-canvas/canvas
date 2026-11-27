/*
 * draw.c - 그리기 클라이언트: 사람이 터미널에서 마우스로 그림판에 그린다 (요구사항 F-10)
 *
 *   draw_<ipc> [-k 그림판] [-i 번호] [-I 갱신ms]
 *
 * 마우스를 지원하는 터미널(Ptyxis, GNOME 터미널, xterm 등)에서 쓴다. 외부 라이브러리 없이 ANSI 이스케이프로 그린다.
 * 같은 그림판의 다른 클라이언트(사람, 부하 클라이언트)가 그린 것은 보이는 영역을 주기적으로 SNAPSHOT 해서 보여 준다.
 * 표준 입력이 터미널이 아니면 명령 줄 모드다 (시험·스크립트용): draw X Y W H V | snap X Y W H | room K | stats |
 * badtype | sleep MS | quit. 명령마다 응답을 기다린 뒤 결과 한 줄을 쓴다.
 *
 * 스레드 (docs/design/README.md#스레드-설계)
 *   입력     터미널의 마우스·키(또는 명령 줄)를 읽어 DRAW 를 보낸다
 *   수신     응답을 받아 SNAPSHOT 내용을 거울(그림판 사본)에 쓰고 왕복 지연을 기록한다
 *   갱신     -I ms 마다 보이는 영역을 256칸씩 나눠 SNAPSHOT 하고 화면을 다시 그린다. 1초마다 STATS
 *   신호     SIGINT·SIGTERM(끝), SIGWINCH(창 크기)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "proto.h"
#include "transport.h"
#include "util.h"

#define NSLOT 128u
#define SLOT_BITS 10

/* ---- 요청 표 (부하 클라이언트와 같은 방식) ---- */

typedef struct {
    atomic_int state;
    uint64_t   req_id, t0;
    uint32_t   type, room;
    uint16_t   x, y, w, h;
} slot_t;

static slot_t   slots[NSLOT];
static uint32_t free_stack[NSLOT], free_top;
static pthread_mutex_t free_mtx = PTHREAD_MUTEX_INITIALIZER;
static sem_t free_sem;
static uint64_t id_base;
static atomic_ullong seq;

static uint32_t my_id, room;
static atomic_bool quitting, dead;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;   /* 거울, 화면 상태, 아래 값들 */
static pthread_cond_t  cond = PTHREAD_COND_INITIALIZER;
static uint32_t inflight;

/* 거울: 보고 있는 그림판의 사본 */
static uint8_t mirror[CANVAS_W * CANVAS_H];
static bool dirty = true;

/* 상태 줄 */
static double last_rtt_us, avg_rtt_us;
static uint64_t n_rtt;
static stats_resp_t stats;
static bool have_stats;

/* 명령 줄 모드의 마지막 결과 */
static uint32_t last_err;
static uint64_t snap_sum, snap_nz, snap_fnv;

static bool tty;
static int refresh_ms = 150;

/* ---- 보내기 ---- */

static int send_req(uint32_t type, const void *payload, uint32_t plen, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    if (atomic_load(&dead)) return -1;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 3;
    while (sem_timedwait(&free_sem, &ts) < 0) {
        if (errno == EINTR) continue;
        return -1;                                   /* 3초 동안 칸이 비지 않는다 (서버가 멈췄다) */
    }
    if (atomic_load(&dead)) { sem_post(&free_sem); return -1; }
    pthread_mutex_lock(&free_mtx);
    uint32_t k = free_stack[--free_top];
    pthread_mutex_unlock(&free_mtx);
    pthread_mutex_lock(&mtx);
    inflight++;
    pthread_mutex_unlock(&mtx);

    slot_t *s = &slots[k];
    msg_hdr_t hd = { .type = type, .client_id = my_id, .req_id = id_base | (atomic_fetch_add(&seq, 1) << SLOT_BITS) | k,
                     .payload_len = plen, .room = room };
    s->req_id = hd.req_id;
    s->type = type;
    s->room = room;
    s->x = x; s->y = y; s->w = w; s->h = h;
    s->t0 = now_ns();
    atomic_store_explicit(&s->state, 1, memory_order_release);
    if (tp_send_req(&hd, payload) < 0) {
        atomic_store(&s->state, 0);
        pthread_mutex_lock(&free_mtx);
        free_stack[free_top++] = k;
        pthread_mutex_unlock(&free_mtx);
        pthread_mutex_lock(&mtx);
        inflight--;
        pthread_cond_broadcast(&cond);
        pthread_mutex_unlock(&mtx);
        sem_post(&free_sem);
        atomic_store(&dead, true);
        return -1;
    }
    return 0;
}

static int send_draw(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t v)
{
    if (x >= CANVAS_W || y >= CANVAS_H) return 0;
    if (x + w > CANVAS_W) w = CANVAS_W - x;
    if (y + h > CANVAS_H) h = CANVAS_H - y;
    draw_req_t d = { .x = (uint16_t)x, .y = (uint16_t)y, .w = (uint16_t)w, .h = (uint16_t)h, .v = v };
    return send_req(REQ_DRAW, &d, sizeof d, d.x, d.y, d.w, d.h);
}

static int send_snap(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    snap_req_t s = { .x = (uint16_t)x, .y = (uint16_t)y, .w = (uint16_t)w, .h = (uint16_t)h };
    return send_req(REQ_SNAPSHOT, &s, sizeof s, s.x, s.y, s.w, s.h);
}

/* 띄운 요청의 응답이 모두 올 때까지 기다린다. ms 를 넘기면 false */
static bool drain(int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&mtx);
    int rc = 0;
    while (inflight > 0 && !atomic_load(&dead) && rc == 0) rc = pthread_cond_timedwait(&cond, &mtx, &ts);
    bool ok = inflight == 0;
    pthread_mutex_unlock(&mtx);
    return ok;
}

/* ---- 수신 ---- */

static void *recv_main(void *arg)
{
    (void)arg;
    uint8_t *buf = malloc(APP_MAX_RESP);
    msg_hdr_t hd;
    while (buf) {
        ssize_t n = tp_recv_resp(&hd, buf, APP_MAX_RESP);
        uint64_t t5 = now_ns();
        if (n < 0) {
            if (errno == EMSGSIZE) continue;
            break;
        }
        uint32_t k = (uint32_t)(hd.req_id & ((1u << SLOT_BITS) - 1));
        if (k >= NSLOT || atomic_load_explicit(&slots[k].state, memory_order_acquire) != 1 || slots[k].req_id != hd.req_id)
            continue;
        slot_t *s = &slots[k];
        pthread_mutex_lock(&mtx);
        if (hd.type == RESP_ERROR && (size_t)n >= sizeof(srv_ts_t) + sizeof(uint32_t)) {
            memcpy(&last_err, buf + sizeof(srv_ts_t), sizeof last_err);
        } else if (hd.type == RESP_OK) {
            last_err = 0;
            if (s->type == REQ_SNAPSHOT && (size_t)n == sizeof(srv_ts_t) + (size_t)s->w * s->h && s->room == room) {
                const uint8_t *p = buf + sizeof(srv_ts_t);
                uint64_t sum = 0, nz = 0, f = 0xcbf29ce484222325ull;
                for (uint32_t r = 0; r < s->h; r++) {
                    memcpy(mirror + (size_t)(s->y + r) * CANVAS_W + s->x, p + (size_t)r * s->w, s->w);
                    for (uint32_t c = 0; c < s->w; c++) {
                        uint8_t v = p[(size_t)r * s->w + c];
                        sum += v; nz += v != 0;
                        f = (f ^ v) * 0x100000001b3ull;
                    }
                }
                snap_sum = sum; snap_nz = nz; snap_fnv = f;
                dirty = true;
            } else if (s->type == REQ_DRAW) {
                last_rtt_us = (double)(t5 - s->t0) / 1000.0;
                n_rtt++;
                avg_rtt_us += (last_rtt_us - avg_rtt_us) / (double)(n_rtt < 100 ? n_rtt : 100);
            } else if (s->type == REQ_STATS && (size_t)n >= sizeof(srv_ts_t) + sizeof stats) {
                memcpy(&stats, buf + sizeof(srv_ts_t), sizeof stats);
                have_stats = true;
            }
        }
        bool bye = s->type == REQ_BYE;
        atomic_store_explicit(&s->state, 0, memory_order_release);
        pthread_mutex_lock(&free_mtx);
        free_stack[free_top++] = k;
        pthread_mutex_unlock(&free_mtx);
        if (--inflight == 0) pthread_cond_broadcast(&cond);
        pthread_mutex_unlock(&mtx);
        sem_post(&free_sem);
        if (bye) break;
    }
    free(buf);
    atomic_store(&dead, true);
    pthread_mutex_lock(&mtx);
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&mtx);
    for (uint32_t i = 0; i < NSLOT; i++) sem_post(&free_sem);
    return NULL;
}

/* ---- 화면 ---- */

static int cols = 80, rows = 24;
static int zoom = 4;                         /* 화면 점 하나 = zoom × zoom 칸 */
static int vx, vy;                           /* 보이는 영역의 왼쪽 위 (그림판 좌표) */
static uint8_t color = 3;                    /* 1 ~ 9 */
static int brush = 8;
static bool rect_mode;
static struct termios saved;
static atomic_bool resized = true;

static uint8_t color_value(int k) { return (uint8_t)(k * 28); }

/* 값 → RGB. 0 은 배경, 나머지는 색상환을 따라 고르게 */
static void value_rgb(uint8_t v, int *r, int *g, int *b)
{
    if (v == 0) { *r = 30; *g = 30; *b = 46; return; }
    double h = (double)v / 256.0 * 6.0, s = 0.70, val = 0.95;
    int i = (int)h;
    double f = h - i, p = val * (1 - s), q = val * (1 - s * f), t = val * (1 - s * (1 - f));
    double rr, gg, bb;
    switch (i % 6) {
    case 0: rr = val; gg = t; bb = p; break;
    case 1: rr = q; gg = val; bb = p; break;
    case 2: rr = p; gg = val; bb = t; break;
    case 3: rr = p; gg = q; bb = val; break;
    case 4: rr = t; gg = p; bb = val; break;
    default: rr = val; gg = p; bb = q; break;
    }
    *r = (int)(rr * 255); *g = (int)(gg * 255); *b = (int)(bb * 255);
}

static int view_w(void) { return cols * zoom; }
static int view_h(void) { return (rows - 1) * 2 * zoom; }

static void clamp_view(void)
{
    if (vx > (int)CANVAS_W - view_w()) vx = (int)CANVAS_W - view_w();
    if (vy > (int)CANVAS_H - view_h()) vy = (int)CANVAS_H - view_h();
    if (vx < 0) vx = 0;
    if (vy < 0) vy = 0;
}

/* 화면 점 하나에 해당하는 zoom × zoom 칸 중 가장 큰 값 (가는 선이 줄여 보여도 사라지지 않게) */
static uint8_t sample(int px, int py)
{
    int x0 = vx + px * zoom, y0 = vy + py * zoom;
    if (x0 >= (int)CANVAS_W || y0 >= (int)CANVAS_H) return 0;
    uint8_t m = 0;
    for (int y = y0; y < y0 + zoom && y < (int)CANVAS_H; y++)
        for (int x = x0; x < x0 + zoom && x < (int)CANVAS_W; x++)
            if (mirror[(size_t)y * CANVAS_W + x] > m) m = mirror[(size_t)y * CANVAS_W + x];
    return m;
}

static char *frame;
static size_t frame_cap;

static void put(size_t *len, const char *s, size_t n)
{
    if (*len + n > frame_cap) return;
    memcpy(frame + *len, s, n);
    *len += n;
}

static void render(void)
{
    size_t need = (size_t)cols * rows * 48 + 1024;
    if (need > frame_cap) { free(frame); frame = malloc(need); frame_cap = frame ? need : 0; }
    if (!frame) return;
    size_t len = 0;
    char tmp[96];
    put(&len, "\x1b[H", 3);
    pthread_mutex_lock(&mtx);
    for (int r = 0; r < rows - 1; r++) {
        int lf = -1, lb = -1;
        snprintf(tmp, sizeof tmp, "\x1b[%d;1H", r + 1);
        put(&len, tmp, strlen(tmp));
        for (int c = 0; c < cols; c++) {
            uint8_t up = sample(c, r * 2), dn = sample(c, r * 2 + 1);
            int a, b2, cc;
            if (up != lf) {
                value_rgb(up, &a, &b2, &cc);
                int k = snprintf(tmp, sizeof tmp, "\x1b[38;2;%d;%d;%dm", a, b2, cc);
                put(&len, tmp, (size_t)k);
                lf = up;
            }
            if (dn != lb) {
                value_rgb(dn, &a, &b2, &cc);
                int k = snprintf(tmp, sizeof tmp, "\x1b[48;2;%d;%d;%dm", a, b2, cc);
                put(&len, tmp, (size_t)k);
                lb = dn;
            }
            put(&len, "\xe2\x96\x80", 3);            /* ▀ 위 반쪽 = 글자색, 아래 반쪽 = 배경색 */
        }
    }
    int a, b2, cc;
    value_rgb(color_value(color), &a, &b2, &cc);
    int k = snprintf(tmp, sizeof tmp, "\x1b[%d;1H\x1b[0m\x1b[2K\x1b[48;2;%d;%d;%dm  \x1b[0m ", rows, a, b2, cc);
    put(&len, tmp, (size_t)k);
    char st[320];
    int sl = snprintf(st, sizeof st,
                      "draw_%s #%u 그림판 %u | 접속 %u | DRAW 왕복 %.0fµs (평균 %.0f) | 색 %u %s %d | 1:%d (%d,%d) | q 끝",
                      tp_name(), my_id, room, have_stats ? stats.n_clients : 0, last_rtt_us, avg_rtt_us, color,
                      rect_mode ? "사각형" : "붓", brush, zoom, vx, vy);
    put(&len, st, (size_t)sl);
    dirty = false;
    pthread_mutex_unlock(&mtx);
    ssize_t w = write(STDOUT_FILENO, frame, len);
    (void)w;
}

static void query_size(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 10 && ws.ws_row > 3) {
        pthread_mutex_lock(&mtx);
        cols = ws.ws_col;
        rows = ws.ws_row;
        clamp_view();
        dirty = true;
        pthread_mutex_unlock(&mtx);
    }
}

/* 보이는 영역을 256칸 타일로 나눠 SNAPSHOT 한다 */
static void request_view(void)
{
    pthread_mutex_lock(&mtx);
    int x0 = vx, y0 = vy, x1 = vx + view_w(), y1 = vy + view_h();
    pthread_mutex_unlock(&mtx);
    if (x1 > (int)CANVAS_W) x1 = CANVAS_W;
    if (y1 > (int)CANVAS_H) y1 = CANVAS_H;
    for (int y = y0; y < y1; y += SNAP_MAX_SIDE)
        for (int x = x0; x < x1; x += SNAP_MAX_SIDE) {
            int w = x1 - x < (int)SNAP_MAX_SIDE ? x1 - x : (int)SNAP_MAX_SIDE;
            int h = y1 - y < (int)SNAP_MAX_SIDE ? y1 - y : (int)SNAP_MAX_SIDE;
            if (w > 0 && h > 0 && send_snap((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h) < 0) return;
        }
}

static void *refresh_main(void *arg)
{
    (void)arg;
    uint64_t last_stats = 0;
    while (!atomic_load(&quitting) && !atomic_load(&dead)) {
        if (atomic_exchange(&resized, false)) query_size();
        uint64_t t = now_ns();
        request_view();
        if (t - last_stats > 1000000000ull) {
            send_req(REQ_STATS, NULL, 0, 0, 0, 0, 0);
            last_stats = t;
        }
        drain(1000);
        render();
        struct timespec ts = { 0, (long)refresh_ms * 1000000L };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

/* ---- 그리기 입력 ---- */

/* 한 획 안에서 칠한 붓 칸. 같은 칸을 두 번 더하면 색이 바뀌므로(더하기 연산) 칸마다 한 번만 칠한다 */
#define STROKE_CAP 65536u
static uint32_t stroke_keys[STROKE_CAP];
static bool stroke_used[STROKE_CAP];
static uint32_t stroke_n;

static void stroke_reset(void) { memset(stroke_used, 0, sizeof stroke_used); stroke_n = 0; }

static bool stroke_add(uint32_t key)
{
    if (stroke_n > STROKE_CAP / 2) stroke_reset();
    uint32_t h = (key * 2654435761u) % STROKE_CAP;
    while (stroke_used[h]) {
        if (stroke_keys[h] == key) return false;
        h = (h + 1) % STROKE_CAP;
    }
    stroke_used[h] = true;
    stroke_keys[h] = key;
    stroke_n++;
    return true;
}

static void paint_at(int cx, int cy)
{
    if (cx < 0 || cy < 0 || cx >= (int)CANVAS_W || cy >= (int)CANVAS_H) return;
    uint32_t gx = (uint32_t)cx / (uint32_t)brush, gy = (uint32_t)cy / (uint32_t)brush;
    if (!stroke_add(gy * 4096u + gx)) return;
    uint32_t x = gx * (uint32_t)brush, y = gy * (uint32_t)brush;
    uint8_t v = color_value(color);
    /* 기다리지 않고 거울에도 바로 반영한다. 다음 SNAPSHOT 이 서버의 값으로 덮는다 */
    pthread_mutex_lock(&mtx);
    for (uint32_t r = y; r < y + (uint32_t)brush && r < CANVAS_H; r++)
        for (uint32_t c = x; c < x + (uint32_t)brush && c < CANVAS_W; c++) mirror[(size_t)r * CANVAS_W + c] += v;
    dirty = true;
    pthread_mutex_unlock(&mtx);
    send_draw(x, y, (uint32_t)brush, (uint32_t)brush, v);
}

static int last_cx = -1, last_cy = -1, anchor_x, anchor_y;

static void stroke_to(int cx, int cy)
{
    if (last_cx < 0) { paint_at(cx, cy); last_cx = cx; last_cy = cy; return; }
    int dx = cx - last_cx, dy = cy - last_cy;
    int steps = (abs(dx) > abs(dy) ? abs(dx) : abs(dy)) / (brush > 1 ? brush / 2 : 1) + 1;
    for (int i = 1; i <= steps; i++) paint_at(last_cx + dx * i / steps, last_cy + dy * i / steps);
    last_cx = cx;
    last_cy = cy;
}

static void cell_to_canvas(int col, int row, int *cx, int *cy)
{
    *cx = vx + (col - 1) * zoom + zoom / 2;
    *cy = vy + (row - 1) * 2 * zoom + zoom;
}

static void on_mouse(int b, int col, int row, bool release)
{
    if (b == 64 || b == 65) {                        /* 휠: 확대 / 축소 */
        pthread_mutex_lock(&mtx);
        int cx, cy;
        cell_to_canvas(col, row, &cx, &cy);
        int nz = b == 64 ? zoom / 2 : zoom * 2;
        if (nz >= 1 && nz <= 16) {
            zoom = nz;
            vx = cx - (col - 1) * zoom;
            vy = cy - (row - 1) * 2 * zoom;
            clamp_view();
        }
        dirty = true;
        pthread_mutex_unlock(&mtx);
        return;
    }
    if (row >= rows) return;                         /* 상태 줄 */
    int cx, cy;
    cell_to_canvas(col, row, &cx, &cy);
    int btn = b & 3;
    bool motion = b & 32;
    if (btn != 0 && !release) return;                /* 왼쪽 버튼만 */
    if (rect_mode) {
        if (!release && !motion) { anchor_x = cx; anchor_y = cy; }
        else if (release) {
            int x0 = anchor_x < cx ? anchor_x : cx, y0 = anchor_y < cy ? anchor_y : cy;
            int x1 = anchor_x > cx ? anchor_x : cx, y1 = anchor_y > cy ? anchor_y : cy;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            send_draw((uint32_t)x0, (uint32_t)y0, (uint32_t)(x1 - x0 + 1), (uint32_t)(y1 - y0 + 1), color_value(color));
        }
        return;
    }
    if (release) { last_cx = last_cy = -1; stroke_reset(); return; }
    stroke_to(cx, cy);
}

static void on_key(int ch)
{
    pthread_mutex_lock(&mtx);
    int step = view_w() / 4;
    switch (ch) {
    case 'q': case 3: atomic_store(&quitting, true); break;
    case 'b': rect_mode = false; break;
    case 'r': rect_mode = true; break;
    case '[': if (brush > 1) brush /= 2; break;
    case ']': if (brush < 64) brush *= 2; break;
    case '+': case '=': if (zoom > 1) zoom /= 2; break;
    case '-': if (zoom < 16) zoom *= 2; break;
    case '0': zoom = 8; vx = vy = 0; break;
    case 'h': case 'D': vx -= step; break;
    case 'l': case 'C': vx += step; break;
    case 'k': case 'A': vy -= view_h() / 4; break;
    case 'j': case 'B': vy += view_h() / 4; break;
    default:
        if (ch >= '1' && ch <= '9') color = (uint8_t)(ch - '0');
        break;
    }
    clamp_view();
    dirty = true;
    pthread_mutex_unlock(&mtx);
}

/* 터미널 입력: 키와 SGR 마우스 보고(ESC [ < b ; x ; y M/m), 방향키(ESC [ A~D) */
static void *input_tty(void *arg)
{
    (void)arg;
    char buf[256];
    char seq_buf[64];
    int sl = 0;
    bool in_esc = false;
    while (!atomic_load(&quitting) && !atomic_load(&dead)) {
        struct pollfd pf = { STDIN_FILENO, POLLIN, 0 };
        if (poll(&pf, 1, 200) <= 0) continue;
        ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) {
            char ch = buf[i];
            if (!in_esc) {
                if (ch == 0x1b) { in_esc = true; sl = 0; continue; }
                on_key((unsigned char)ch);
                continue;
            }
            if (sl < (int)sizeof seq_buf - 1) seq_buf[sl++] = ch;
            seq_buf[sl] = 0;
            if (sl == 1 && ch != '[') { in_esc = false; continue; }
            if (sl >= 2 && (ch == 'M' || ch == 'm') && seq_buf[1] == '<') {
                int b, x, y;
                if (sscanf(seq_buf + 2, "%d;%d;%d", &b, &x, &y) == 3) on_mouse(b, x, y, ch == 'm');
                in_esc = false;
            } else if (sl == 2 && ch >= 'A' && ch <= 'D') {
                on_key(ch);
                in_esc = false;
            } else if (sl >= 2 && ((ch >= 'A' && ch <= 'Z') || ch == '~') && seq_buf[1] != '<') {
                in_esc = false;
            }
        }
    }
    atomic_store(&quitting, true);
    return NULL;
}

/* 명령 줄 모드 (표준 입력이 터미널이 아닐 때) */
static void *input_cmd(void *arg)
{
    (void)arg;
    char line[256];
    while (!atomic_load(&quitting) && fgets(line, sizeof line, stdin)) {
        unsigned x, y, w, h, v;
        char cmd[16] = "";
        if (sscanf(line, "%15s", cmd) != 1) continue;
        if (strcmp(cmd, "quit") == 0) break;
        if (strcmp(cmd, "draw") == 0 && sscanf(line + 4, "%u %u %u %u %u", &x, &y, &w, &h, &v) == 5) {
            draw_req_t d = { .x = (uint16_t)x, .y = (uint16_t)y, .w = (uint16_t)w, .h = (uint16_t)h, .v = (uint8_t)v };
            if (send_req(REQ_DRAW, &d, sizeof d, d.x, d.y, d.w, d.h) < 0 || !drain(5000)) break;
            pthread_mutex_lock(&mtx);
            if (last_err) printf("error %u\n", last_err); else printf("ok\n");
            pthread_mutex_unlock(&mtx);
        } else if (strcmp(cmd, "snap") == 0 && sscanf(line + 4, "%u %u %u %u", &x, &y, &w, &h) == 4) {
            snap_req_t s = { .x = (uint16_t)x, .y = (uint16_t)y, .w = (uint16_t)w, .h = (uint16_t)h };
            pthread_mutex_lock(&mtx);
            snap_sum = snap_nz = 0; snap_fnv = 0;
            pthread_mutex_unlock(&mtx);
            if (send_req(REQ_SNAPSHOT, &s, sizeof s, s.x, s.y, s.w, s.h) < 0 || !drain(5000)) break;
            pthread_mutex_lock(&mtx);
            if (last_err) printf("error %u\n", last_err);
            else printf("snap %u %u %u %u sum=%llu nz=%llu fnv=%016llx\n", x, y, w, h, (unsigned long long)snap_sum,
                        (unsigned long long)snap_nz, (unsigned long long)snap_fnv);
            pthread_mutex_unlock(&mtx);
        } else if (strcmp(cmd, "room") == 0 && sscanf(line + 4, "%u", &v) == 1) {
            room = v;                                /* 서버의 그림판 수를 넘으면 다음 요청이 오류로 돌아온다 */
            printf("ok\n");
        } else if (strcmp(cmd, "stats") == 0) {
            if (send_req(REQ_STATS, NULL, 0, 0, 0, 0, 0) < 0 || !drain(5000)) break;
            pthread_mutex_lock(&mtx);
            printf("stats draw=%llu snapshot=%llu error=%llu clients=%u rooms=%u workers=%u\n",
                   (unsigned long long)stats.n_draw, (unsigned long long)stats.n_snapshot,
                   (unsigned long long)stats.n_error, stats.n_clients, stats.n_rooms, stats.n_workers);
            pthread_mutex_unlock(&mtx);
        } else if (strcmp(cmd, "badtype") == 0) {
            if (send_req(99, NULL, 0, 0, 0, 0, 0) < 0 || !drain(5000)) break;
            pthread_mutex_lock(&mtx);
            if (last_err) printf("error %u\n", last_err); else printf("ok\n");
            pthread_mutex_unlock(&mtx);
        } else if (strcmp(cmd, "sleep") == 0 && sscanf(line + 5, "%u", &v) == 1) {
            struct timespec ts = { v / 1000, (long)(v % 1000) * 1000000L };
            nanosleep(&ts, NULL);
        } else {
            printf("? %s", line);
        }
        fflush(stdout);
    }
    atomic_store(&quitting, true);
    return NULL;
}

/* ---- 신호 ---- */

static void *sig_main(void *arg)
{
    (void)arg;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGWINCH);
    sigaddset(&set, SIGUSR1);
    for (;;) {
        int sig;
        if (sigwait(&set, &sig) != 0) continue;
        if (sig == SIGUSR1) break;
        if (sig == SIGWINCH) { atomic_store(&resized, true); continue; }
        if (atomic_exchange(&quitting, true)) {          /* 두 번째: 기다리지 않고 끝낸다 */
            if (tty) {
                static const char fin[] = "\x1b[0m\x1b[?1002l\x1b[?1006l\x1b[?25h\x1b[?1049l";
                tcsetattr(STDIN_FILENO, TCSANOW, &saved);
                ssize_t w = write(STDOUT_FILENO, fin, sizeof fin - 1);
                (void)w;
            }
            tp_client_close();
            _exit(1);
        }
    }
    return NULL;
}

/* ---- main ---- */

int main(int argc, char **argv)
{
    int c;
    int want_id = -1;
    while ((c = getopt(argc, argv, "k:i:I:h")) != -1) {
        switch (c) {
        case 'k': room = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'i': want_id = atoi(optarg); break;
        case 'I': refresh_ms = atoi(optarg); if (refresh_ms < 20) refresh_ms = 20; break;
        default:
            fprintf(stderr, "사용법: %s [-k 그림판] [-i 번호] [-I 갱신ms]\n", argv[0]);
            return c == 'h' ? 0 : 2;
        }
    }
    tty = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    id_base = (uint64_t)getpid() << 40;
    for (uint32_t i = 0; i < NSLOT; i++) free_stack[i] = NSLOT - 1 - i;
    free_top = NSLOT;
    sem_init(&free_sem, 0, NSLOT);

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGWINCH);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    /* 번호: -i 가 없으면 1023 부터 내려가며 비어 있는 번호를 고른다 (실행기는 0 부터 쓴다) */
    int rc = -1;
    for (int id = want_id >= 0 ? want_id : (int)MAX_CLIENTS - 1; id >= 0; id--) {
        rc = tp_client_open((uint32_t)id);
        if (rc == 0) { my_id = (uint32_t)id; break; }
        if (errno != EADDRINUSE || want_id >= 0) break;
    }
    if (rc < 0) {
        char eb[128];
        fprintf(stderr, "draw_%s: %s\n", tp_name(),
                errno == ECONNREFUSED ? "서버가 없다" : errno == EADDRINUSE ? "같은 번호의 클라이언트가 실행 중이다"
                                                                            : err_str(errno, eb, sizeof eb));
        return 1;
    }

    pthread_t rth, ith, fth, sth;
    pthread_create(&sth, NULL, sig_main, NULL);
    pthread_create(&rth, NULL, recv_main, NULL);
    if (send_req(REQ_HELLO, NULL, 0, 0, 0, 0, 0) < 0 || !drain(3000)) {
        fprintf(stderr, "draw_%s: 서버가 응답하지 않는다\n", tp_name());
        tp_client_close();
        return 1;
    }

    if (tty) {
        tcgetattr(STDIN_FILENO, &saved);
        struct termios raw = saved;
        raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG | IEXTEN);
        raw.c_iflag &= ~(tcflag_t)(IXON | ICRNL);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        const char *init = "\x1b[?1049h\x1b[?25l\x1b[2J\x1b[?1002h\x1b[?1006h";
        ssize_t w = write(STDOUT_FILENO, init, strlen(init));
        (void)w;
        query_size();
        zoom = 1;                                    /* 첫 화면: 그림판 가로가 화면에 들어가는 가장 작은 배율 */
        while (zoom < 16 && cols * zoom < (int)CANVAS_W) zoom *= 2;
        brush = zoom * 2;
        pthread_create(&fth, NULL, refresh_main, NULL);
        pthread_create(&ith, NULL, input_tty, NULL);
    } else {
        pthread_create(&ith, NULL, input_cmd, NULL);
    }

    pthread_join(ith, NULL);
    atomic_store(&quitting, true);
    if (tty) pthread_join(fth, NULL);
    drain(2000);
    if (!atomic_load(&dead) && send_req(REQ_BYE, NULL, 0, 0, 0, 0, 0) == 0) drain(2000);
    if (tty) {
        const char *fin = "\x1b[0m\x1b[?1002l\x1b[?1006l\x1b[?25h\x1b[?1049l";
        ssize_t w = write(STDOUT_FILENO, fin, strlen(fin));
        (void)w;
        tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    }
    tp_client_close();
    atomic_store(&dead, true);
    pthread_kill(sth, SIGUSR1);
    pthread_join(sth, NULL);
    /* 수신 스레드는 BYE 응답이나 닫힌 채널에서 끝난다 */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 2;
    if (pthread_timedjoin_np(rth, NULL, &ts) != 0) pthread_detach(rth);
    return 0;
}
