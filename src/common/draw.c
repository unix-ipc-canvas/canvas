/*
 * draw.c - 사람이 직접 그리는 대화형 클라이언트 (draw_pipe, draw_mq, draw_shm)
 *
 * 터미널에서 마우스로 캔버스에 그리고, 다른 클라이언트가 그린 것을 거의 실시간으로 본다.
 * 여러 사람이 각자 터미널에서 실행하면 같은 캔버스에 동시에 그린다. 외부 라이브러리 없이 터미널 제어 문자만 쓴다.
 *
 *   ./bin/draw_shm               번호는 비어 있는 것을 고른다 (-i 로 정할 수 있다)
 *   왼쪽 버튼 끌기   붓으로 칠하기 (붓 모드) / 사각형 그리기 (사각형 모드)
 *   휠, + -, 0       확대·축소, 전체 보기       방향키·hjkl  보는 곳 옮기기
 *   1~9             색(더할 값) 고르기         b / r       붓 / 사각형 모드      [ ]  붓 크기
 *   q, Ctrl+C       끝내기
 *
 * 캔버스의 연산은 응용 그대로 "더하기(mod 256)"다. 같은 곳에 겹쳐 그리면 값이 더해져 색이 바뀐다.
 * 보이는 그림은 주기적으로(-I, 기본 150ms) 보이는 영역을 SNAPSHOT 으로 받아 그린다.
 *
 * 표준 입력이 터미널이 아니면 명령 줄을 읽는다 (시험·스크립트용, 화면을 그리지 않는다).
 *   draw X Y W H V    사각형 그리기 (응답을 기다리지 않는다)
 *   sync              보낸 요청의 응답을 모두 받을 때까지 기다린다
 *   snap X Y W H      영역을 받아 "snap <바이트 수> <FNV-1a 64>" 를 출력한다. 전체 영역의 값은 서버 종료 체크섬과 같다
 *   stats             "stats clients=<접속 수> draw=<서버가 처리한 DRAW 수>" 출력
 *   sleep MS          기다리기
 *   quit              끝내기 (입력이 끝나도 끝낸다)
 *
 * 스레드
 *   입력   키·마우스(또는 명령 줄)를 읽어 DRAW 를 보내고 화면 상태를 바꾼다
 *   수신   응답을 받는다. SNAPSHOT 응답은 보기 버퍼에 넣고 다시 그리라고 알린다
 *   갱신   주기마다 보이는 영역의 SNAPSHOT 과 1초마다 STATS 를 보낸다 (SNAPSHOT 은 한 번에 하나만 띄운다)
 *   화면   다시 그리라는 알림을 받으면 화면 전체를 한 번의 write 로 그린다
 *   신호   SIGINT·SIGTERM(끝내기), SIGWINCH(터미널 크기 바뀜)을 sigwait 로 받는다
 *   main   접속, 스레드 시작과 정리
 * 공유 상태(보기 버퍼, 화면 상태, 통계)는 mutex 하나로 보호하고, 다시 그리기와 응답 대기는 조건변수로 알린다.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
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

#include "canvas.h"
#include "proto.h"
#include "transport.h"
#include "util.h"

static const char *IPC_NAME = "?";       /* 실행 파일 이름(draw_shm)에서 정한다 */

#define SLOTS 4096u                     /* 띄워 둔 요청 기록 (req_id % SLOTS) */
#define TOP 1                           /* 화면 위 상태 줄 수 */
#define BOTTOM 1                        /* 화면 아래 도움말 줄 수 */

/* 더할 값 9가지. 서로 더해도 0(바탕)이 잘 나오지 않게 고른 값 */
static const uint8_t PALETTE[9] = { 40, 72, 104, 136, 168, 200, 232, 24, 56 };

typedef struct { uint8_t type; uint64_t t_send; uint16_t x, y, w, h; bool used; } slot_t;

static struct {
    /* 설정 */
    uint32_t id;
    int interval_ms;
    bool tty;

    /* 전송 */
    _Atomic uint64_t next_id;
    uint64_t base_id;
    uint8_t *rbuf;

    /* 아래는 mtx 로 보호한다 */
    pthread_mutex_t mtx;
    pthread_cond_t redraw;              /* 화면 스레드를 깨운다 */
    pthread_cond_t idle;                /* 띄운 요청이 0 이 되었다 */
    slot_t slot[SLOTS];
    uint64_t outstanding;
    bool quit, dirty, server_gone;
    uint64_t last_id;                   /* 마지막 STATS 의 req_id. 이 응답을 받으면 수신 스레드가 끝난다 */
    bool last_sent;

    int cols, rows;                     /* 터미널 크기 */
    uint32_t vx, vy, scale;             /* 보이는 영역의 왼쪽 위(캔버스 좌표)와 확대 배율(화면 점 하나 = 캔버스 scale 칸) */
    uint8_t *view;                      /* 마지막으로 받은 영역 */
    uint32_t sx, sy, sw, sh;            /* view 가 담은 영역 */
    bool have_view, snap_busy;

    int color;                          /* PALETTE 번호 */
    bool brush_mode;
    int brush;                          /* 붓 크기 (화면 점) */
    int mx, my;                         /* 마우스 위치 (화면 점) */
    bool drag;
    int ax, ay;                         /* 사각형 시작점 (화면 점) */
    int lx, ly;                         /* 붓이 마지막으로 칠한 점 (화면 점) */

    uint32_t n_clients;
    uint64_t srv_draw, my_draw, my_err;
    double rtt_us;
    char note[96];

    /* 명령 줄 모드 */
    pthread_cond_t got;                 /* snap·stats 결과가 왔다 */
    bool want_snap, snap_done, want_stats, stats_done;
    uint64_t snap_sum;
    size_t snap_len;
} D = { .mtx = PTHREAD_MUTEX_INITIALIZER, .redraw = PTHREAD_COND_INITIALIZER,
        .idle = PTHREAD_COND_INITIALIZER, .got = PTHREAD_COND_INITIALIZER };

static struct termios term_saved;
static bool term_raw;

/* ---- 터미널 ---- */

static void out_all(const char *s, size_t n)
{
    while (n) {
        ssize_t w = write(STDOUT_FILENO, s, n);
        if (w < 0) { if (errno == EINTR) continue; return; }
        s += w; n -= (size_t)w;
    }
}
static void out(const char *s) { out_all(s, strlen(s)); }

static void term_restore(void)
{
    if (!term_raw) return;
    out("\033[?1006l\033[?1002l\033[?1000l\033[?7h\033[?25h\033[0m\033[?1049l");
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &term_saved);
    term_raw = false;
}

static int term_setup(void)
{
    if (tcgetattr(STDIN_FILENO, &term_saved) < 0) return -1;
    struct termios t = term_saved;
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG | IEXTEN);
    t.c_iflag &= ~(tcflag_t)(IXON | ICRNL);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) < 0) return -1;
    term_raw = true;
    atexit(term_restore);
    /* 대체 화면, 커서 숨김, 마우스(누름·뗌·끌기, SGR 형식) */
    out("\033[?1049h\033[?25l\033[?7l\033[?1000h\033[?1002h\033[?1006h");     /* ?7l: 줄 끝에서 넘기지 않는다 */
    return 0;
}

static void term_size(int *cols, int *rows)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 20 && ws.ws_row > 6) {
        *cols = ws.ws_col; *rows = ws.ws_row;
    } else {
        *cols = 80; *rows = 24;
    }
}

/* ---- 보기 영역 (mtx 를 쥐고 부른다) ---- */

static int view_w(void) { return D.cols; }                          /* 화면 점 가로 = 칸 수 */
static int view_h(void) { return 2 * (D.rows - TOP - BOTTOM); }     /* 위아래 반칸 문자(▀)로 칸 하나에 점 둘 */

static uint32_t fit_scale(void)
{
    /* 캔버스 전체가 들어가는 가장 작은 정수 배율 (화면을 되도록 넓게 쓴다) */
    uint32_t w = view_w() > 0 ? (uint32_t)view_w() : 1, h = view_h() > 0 ? (uint32_t)view_h() : 1;
    uint32_t sx = (CANVAS_W + w - 1) / w, sy = (CANVAS_H + h - 1) / h;
    uint32_t s = sx > sy ? sx : sy;
    return s < 1 ? 1 : s;
}

static void clamp_view(void)
{
    uint32_t w = (uint32_t)view_w() * D.scale, h = (uint32_t)view_h() * D.scale;
    if (w >= CANVAS_W) D.vx = 0; else if (D.vx + w > CANVAS_W) D.vx = CANVAS_W - w;
    if (h >= CANVAS_H) D.vy = 0; else if (D.vy + h > CANVAS_H) D.vy = CANVAS_H - h;
}

/* 보이는 영역 (캔버스 좌표, 캔버스 안으로 자름) */
static void view_rect(uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h)
{
    uint32_t vw = (uint32_t)view_w() * D.scale, vh = (uint32_t)view_h() * D.scale;
    *x = D.vx; *y = D.vy;
    *w = D.vx + vw > CANVAS_W ? CANVAS_W - D.vx : vw;
    *h = D.vy + vh > CANVAS_H ? CANVAS_H - D.vy : vh;
}

/* 화면 점 → 캔버스 좌표. 캔버스 밖이면 false */
static bool to_canvas(int px, int py, uint32_t *cx, uint32_t *cy)
{
    if (px < 0 || py < 0) return false;
    uint64_t x = D.vx + (uint64_t)px * D.scale, y = D.vy + (uint64_t)py * D.scale;
    if (x >= CANVAS_W || y >= CANVAS_H) return false;
    *cx = (uint32_t)x; *cy = (uint32_t)y;
    return true;
}

/* ---- 보내기 ---- */

/* 요청 하나를 보낸다. snap 이면 영역을 기록해 둔다. mtx 를 쥐지 않고 부른다 */
static int send_req(uint32_t type, const void *payload, uint32_t len, const snap_req_t *area, bool last)
{
    msg_hdr_t h;
    memset(&h, 0, sizeof h);
    h.type = type;
    h.client_id = D.id;
    h.req_id = atomic_fetch_add(&D.next_id, 1);
    h.payload_len = len;

    pthread_mutex_lock(&D.mtx);
    slot_t *s = &D.slot[h.req_id % SLOTS];
    if (s->used) {                       /* 4096개가 밀려 있다: 서버가 매우 느리다. 이번 요청은 보내지 않는다 */
        pthread_mutex_unlock(&D.mtx);
        errno = EAGAIN;
        return -1;
    }
    *s = (slot_t){ .type = (uint8_t)type, .t_send = now_ns(), .used = true };
    if (area) { s->x = area->x; s->y = area->y; s->w = area->w; s->h = area->h; }
    if (last) { D.last_id = h.req_id; D.last_sent = true; }     /* 응답보다 먼저 세운다 */
    D.outstanding++;
    pthread_mutex_unlock(&D.mtx);

    if (tp_send_req(&h, payload) < 0) {
        int e = errno;
        pthread_mutex_lock(&D.mtx);
        s->used = false;
        D.outstanding--;
        if (last) D.last_sent = false;
        if (e == EPIPE) { D.server_gone = true; D.quit = true; pthread_cond_broadcast(&D.redraw); }
        pthread_cond_broadcast(&D.idle);
        pthread_mutex_unlock(&D.mtx);
        errno = e;
        return -1;
    }
    return 0;
}

static void send_draw(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t v)
{
    if (w == 0 || h == 0 || x >= CANVAS_W || y >= CANVAS_H) return;
    if (x + w > CANVAS_W) w = CANVAS_W - x;
    if (y + h > CANVAS_H) h = CANVAS_H - y;
    draw_req_t d = { .x = (uint16_t)x, .y = (uint16_t)y, .w = (uint16_t)w, .h = (uint16_t)h, .v = v };
    send_req(REQ_DRAW, &d, sizeof d, NULL, false);
}

static int send_snapshot(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    snap_req_t a = { .x = (uint16_t)x, .y = (uint16_t)y, .w = (uint16_t)w, .h = (uint16_t)h };
    return send_req(REQ_SNAPSHOT, &a, sizeof a, &a, false);
}

/* ---- 수신 스레드 ---- */

static uint64_t fnv1a(const uint8_t *p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

static void *recv_main(void *arg)
{
    (void)arg;
    for (;;) {
        msg_hdr_t h;
        ssize_t r = tp_recv_resp(&h, D.rbuf, MAX_PAYLOAD);
        if (r < 0) {
            if (errno == EMSGSIZE) continue;
            pthread_mutex_lock(&D.mtx);
            D.server_gone = true;
            D.quit = true;
            D.outstanding = 0;
            snprintf(D.note, sizeof D.note, "서버가 끝났다");
            pthread_cond_broadcast(&D.redraw);
            pthread_cond_broadcast(&D.idle);
            pthread_cond_broadcast(&D.got);
            pthread_mutex_unlock(&D.mtx);
            return NULL;
        }
        uint64_t t = now_ns();
        pthread_mutex_lock(&D.mtx);
        slot_t *s = &D.slot[h.req_id % SLOTS];
        if (h.req_id < D.base_id || !s->used) {     /* 예전 세션의 응답이거나 이미 받은 번호: 버린다 */
            pthread_mutex_unlock(&D.mtx);
            continue;
        }
        bool ok = h.type == RESP_OK;
        const uint8_t *body = D.rbuf + sizeof(srv_ts_t);
        size_t blen = (size_t)r > sizeof(srv_ts_t) ? (size_t)r - sizeof(srv_ts_t) : 0;
        switch (s->type) {
        case REQ_DRAW:
            if (ok) D.my_draw++; else D.my_err++;
            D.rtt_us = (double)(t - s->t_send) / 1e3;
            break;
        case REQ_SNAPSHOT:
            D.snap_busy = false;
            if (ok && blen == (size_t)s->w * s->h) {
                if (D.want_snap) {
                    D.snap_sum = fnv1a(body, blen);
                    D.snap_len = blen;
                    D.want_snap = false;
                    D.snap_done = true;
                    pthread_cond_broadcast(&D.got);
                } else {
                    memcpy(D.view, body, blen);
                    D.sx = s->x; D.sy = s->y; D.sw = s->w; D.sh = s->h;
                    D.have_view = true;
                    D.dirty = true;
                    pthread_cond_signal(&D.redraw);
                }
            }
            break;
        case REQ_STATS:
            if (ok && blen >= sizeof(stats_resp_t)) {
                stats_resp_t st;
                memcpy(&st, body, sizeof st);
                D.n_clients = st.n_clients;
                D.srv_draw = st.n_draw;
                if (D.want_stats) { D.want_stats = false; D.stats_done = true; pthread_cond_broadcast(&D.got); }
                D.dirty = true;
                pthread_cond_signal(&D.redraw);
            }
            break;
        }
        s->used = false;
        D.outstanding--;
        if (D.outstanding == 0) pthread_cond_broadcast(&D.idle);
        bool last = D.last_sent && h.req_id == D.last_id;
        pthread_mutex_unlock(&D.mtx);
        if (last) return NULL;
    }
}

/* ---- 갱신 스레드 ---- */

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) { }
}

static void *refresh_main(void *arg)
{
    (void)arg;
    uint64_t last_stats = 0;
    for (;;) {
        pthread_mutex_lock(&D.mtx);
        if (D.quit) { pthread_mutex_unlock(&D.mtx); return NULL; }
        uint32_t x, y, w, h;
        view_rect(&x, &y, &w, &h);
        bool snap = !D.snap_busy;
        if (snap) D.snap_busy = true;
        pthread_mutex_unlock(&D.mtx);
        if (snap && send_snapshot(x, y, w, h) < 0) {
            pthread_mutex_lock(&D.mtx);
            D.snap_busy = false;
            pthread_mutex_unlock(&D.mtx);
        }
        uint64_t t = now_ns();
        if (t - last_stats > 1000000000ull) { send_req(REQ_STATS, NULL, 0, NULL, false); last_stats = t; }
        sleep_ms(D.interval_ms);
    }
}

/* ---- 화면 스레드 ---- */

/* 값 → 색. 0 은 바탕, 나머지는 색상환을 돈다 (같은 값은 늘 같은 색) */
static void color_of(uint8_t v, int *r, int *g, int *b)
{
    if (v == 0) { *r = 24; *g = 27; *b = 38; return; }
    double hue = (double)v / 256.0 * 6.0;
    int i = (int)hue;
    double f = hue - i, q = 1 - f;
    double rr, gg, bb;
    switch (i % 6) {
    case 0: rr = 1; gg = f; bb = 0; break;
    case 1: rr = q; gg = 1; bb = 0; break;
    case 2: rr = 0; gg = 1; bb = f; break;
    case 3: rr = 0; gg = q; bb = 1; break;
    case 4: rr = f; gg = 0; bb = 1; break;
    default: rr = 1; gg = 0; bb = q; break;
    }
    *r = (int)(60 + rr * 195); *g = (int)(60 + gg * 195); *b = (int)(60 + bb * 195);
}

/* 화면 점 (px, py) 에 보일 값. 그 점이 덮는 캔버스 칸들 중 가장 큰 값 (작은 그림도 보이게) */
static uint8_t sample(int px, int py)
{
    uint32_t cx, cy;
    if (!to_canvas(px, py, &cx, &cy) || !D.have_view) return 0;
    if (cx < D.sx || cy < D.sy) return 0;
    uint32_t ox = cx - D.sx, oy = cy - D.sy;
    if (ox >= D.sw || oy >= D.sh) return 0;
    uint32_t step = D.scale > 4 ? D.scale / 4 : 1, m = 0;
    for (uint32_t j = 0; j < D.scale && oy + j < D.sh; j += step)
        for (uint32_t i = 0; i < D.scale && ox + i < D.sw; i += step) {
            uint8_t v = D.view[(size_t)(oy + j) * D.sw + ox + i];
            if (v > m) m = v;
        }
    return (uint8_t)m;
}

typedef struct { char *p; size_t n, cap; } buf_t;
static void bput(buf_t *b, const char *s, size_t n)
{
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 1 << 16;
        while (c < b->n + n) c *= 2;
        char *p = realloc(b->p, c);
        if (!p) return;
        b->p = p; b->cap = c;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
}
static void bprintf(buf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void bprintf(buf_t *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) bput(b, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

/* 사각형 미리보기·붓 위치에 겹쳐 그릴 테두리인가 */
static bool overlay(int px, int py)
{
    if (D.drag && !D.brush_mode) {
        int x0 = D.ax < D.mx ? D.ax : D.mx, x1 = D.ax < D.mx ? D.mx : D.ax;
        int y0 = D.ay < D.my ? D.ay : D.my, y1 = D.ay < D.my ? D.my : D.ay;
        return px >= x0 && px <= x1 && py >= y0 && py <= y1 && (px == x0 || px == x1 || py == y0 || py == y1);
    }
    if (D.brush_mode) {
        int h = D.brush / 2;
        int x0 = D.mx - h, y0 = D.my - h, x1 = x0 + D.brush - 1, y1 = y0 + D.brush - 1;
        return px >= x0 && px <= x1 && py >= y0 && py <= y1 && (px == x0 || px == x1 || py == y0 || py == y1);
    }
    return false;
}

static void render(buf_t *b)
{
    b->n = 0;
    bput(b, "\033[H", 3);
    /* 위 상태 줄 */
    uint32_t cx = 0, cy = 0;
    bool in = to_canvas(D.mx, D.my, &cx, &cy);
    int r, g, bl;
    color_of(PALETTE[D.color], &r, &g, &bl);
    char line[512];
    int n = snprintf(line, sizeof line,
        " draw_%s  #%u  접속 %u명  서버 DRAW %llu  내 DRAW %llu  왕복 %.2fms  %s  확대 1/%u  %s%u,%u  %s",
        IPC_NAME, D.id, D.n_clients, (unsigned long long)D.srv_draw, (unsigned long long)D.my_draw, D.rtt_us / 1000.0,
        D.brush_mode ? "붓" : "사각형", D.scale, in ? "" : "(밖) ", cx, cy, D.note);
    (void)n;
    bprintf(b, "\033[0m\033[48;2;40;44;60m\033[38;2;230;233;240m%s \033[48;2;%d;%d;%dm  \033[48;2;40;44;60m 색 %d", line, r, g, bl, D.color + 1);
    bput(b, "\033[K", 3);
    /* 그림 */
    int pr = -1, pg = -1, pb = -1, qr = -1, qg = -1, qb = -1;
    for (int row = 0; row < D.rows - TOP - BOTTOM; row++) {
        bprintf(b, "\033[%d;1H", row + 1 + TOP);
        for (int col = 0; col < D.cols; col++) {
            int ur, ug, ub, lr, lg, lb;
            int py = row * 2;
            uint32_t ox, oy;
            if (overlay(col, py)) { ur = ug = ub = 255; }
            else if (!to_canvas(col, py, &ox, &oy)) { ur = 10; ug = 11; ub = 14; }        /* 캔버스 밖 */
            else color_of(sample(col, py), &ur, &ug, &ub);
            if (overlay(col, py + 1)) { lr = lg = lb = 255; }
            else if (!to_canvas(col, py + 1, &ox, &oy)) { lr = 10; lg = 11; lb = 14; }
            else color_of(sample(col, py + 1), &lr, &lg, &lb);
            if (ur != pr || ug != pg || ub != pb) { bprintf(b, "\033[38;2;%d;%d;%dm", ur, ug, ub); pr = ur; pg = ug; pb = ub; }
            if (lr != qr || lg != qg || lb != qb) { bprintf(b, "\033[48;2;%d;%d;%dm", lr, lg, lb); qr = lr; qg = lg; qb = lb; }
            bput(b, "\xe2\x96\x80", 3);          /* ▀ : 위 반칸 = 글자색, 아래 반칸 = 바탕색 */
        }
    }
    /* 아래 도움말 줄 */
    bprintf(b, "\033[%d;1H\033[0m\033[48;2;40;44;60m\033[38;2;170;178;196m", D.rows);
    bprintf(b, " 끌기:%s  1-9:색  b/r:붓/사각형  [ ]:붓 %d  휠·+/-:확대  0:전체  방향키:이동  q:끝   (겹치면 값이 더해져 색이 바뀐다)",
            D.brush_mode ? "칠하기" : "사각형", D.brush);
    bput(b, "\033[K\033[0m", 7);
}

static void *screen_main(void *arg)
{
    (void)arg;
    buf_t b = { 0 };
    pthread_mutex_lock(&D.mtx);
    for (;;) {
        while (!D.dirty && !D.quit) pthread_cond_wait(&D.redraw, &D.mtx);
        if (D.quit) break;
        D.dirty = false;
        render(&b);
        pthread_mutex_unlock(&D.mtx);
        out_all(b.p, b.n);              /* 그리는 동안은 잠그지 않는다 */
        pthread_mutex_lock(&D.mtx);
    }
    pthread_mutex_unlock(&D.mtx);
    free(b.p);
    return NULL;
}

/* ---- 신호 스레드 ---- */

static sigset_t sigs;

static void *signal_main(void *arg)
{
    (void)arg;
    for (;;) {
        int sig;
        if (sigwait(&sigs, &sig) != 0) continue;
        pthread_mutex_lock(&D.mtx);
        if (sig == SIGWINCH) {
            term_size(&D.cols, &D.rows);
            D.scale = D.scale ? D.scale : fit_scale();
            clamp_view();
            D.dirty = true;
            pthread_cond_signal(&D.redraw);
            pthread_mutex_unlock(&D.mtx);
            continue;
        }
        if (sig == SIGUSR1) { pthread_mutex_unlock(&D.mtx); return NULL; }   /* main 이 끝내라고 보냄 */
        D.quit = true;
        pthread_cond_broadcast(&D.redraw);
        pthread_cond_broadcast(&D.got);
        pthread_mutex_unlock(&D.mtx);
    }
}

/* ---- 입력 스레드 (터미널) ---- */

/* 한 번 끄는 동안 이미 칠한 화면 점. 응용의 DRAW는 값을 더하므로 붓 정사각형이 겹친 곳이
 * 두 번 더해지지 않게, 칠하지 않은 점만 보낸다. 입력 스레드만 mtx 를 쥐고 쓴다 */
static struct {
    uint8_t *bits;
    int w, h;
    uint32_t vx, vy, scale;
} M;

static void stroke_begin(void)
{
    int w = view_w(), h = view_h();
    if (w <= 0 || h <= 0) return;
    if (!M.bits || M.w != w || M.h != h) {
        free(M.bits);
        M.bits = malloc((size_t)w * (size_t)h);
        M.w = M.bits ? w : 0; M.h = M.bits ? h : 0;
    }
    if (M.bits) memset(M.bits, 0, (size_t)w * (size_t)h);
    M.vx = D.vx; M.vy = D.vy; M.scale = D.scale;
}

static void stroke(int px, int py)
{
    /* 붓: 마우스 위치를 가운데로 붓 크기 정사각형. 줄마다 아직 칠하지 않은 점의 구간을 사각형 하나로 보낸다 */
    if (!M.bits || M.w != view_w() || M.h != view_h() || M.vx != D.vx || M.vy != D.vy || M.scale != D.scale)
        stroke_begin();                                         /* 끄는 중에 보기가 바뀌었으면 새로 시작 */
    if (!M.bits) return;
    int half = D.brush / 2;
    int x0 = px - half, y0 = py - half, x1 = x0 + D.brush, y1 = y0 + D.brush;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > M.w) x1 = M.w;
    if (y1 > M.h) y1 = M.h;
    uint8_t v = PALETTE[D.color];
    for (int y = y0; y < y1; y++) {
        int x = x0;
        while (x < x1) {
            while (x < x1 && M.bits[(size_t)y * M.w + x]) x++;
            int a = x;
            while (x < x1 && !M.bits[(size_t)y * M.w + x]) M.bits[(size_t)y * M.w + x++] = 1;
            if (x == a) continue;
            uint32_t cx, cy;
            if (!to_canvas(a, y, &cx, &cy)) continue;
            uint64_t ex = D.vx + (uint64_t)x * D.scale, ey = D.vy + (uint64_t)(y + 1) * D.scale;
            if (ex > CANVAS_W) ex = CANVAS_W;
            if (ey > CANVAS_H) ey = CANVAS_H;
            uint32_t w = (uint32_t)(ex - cx), hh = (uint32_t)(ey - cy);
            pthread_mutex_unlock(&D.mtx);
            send_draw(cx, cy, w, hh, v);
            pthread_mutex_lock(&D.mtx);
        }
    }
}

static void zoom(int dir, int px, int py)
{
    uint32_t cx, cy;
    bool in = to_canvas(px, py, &cx, &cy);
    uint32_t ns = dir > 0 ? (D.scale > 1 ? D.scale / 2 : 1) : (D.scale < fit_scale() ? D.scale * 2 : D.scale);
    if (ns == D.scale) return;
    D.scale = ns;
    if (in) {   /* 마우스 아래 칸이 그대로 그 자리에 있게 */
        int64_t vx = (int64_t)cx - (int64_t)px * ns, vy = (int64_t)cy - (int64_t)py * ns;
        D.vx = vx < 0 ? 0 : (uint32_t)vx;
        D.vy = vy < 0 ? 0 : (uint32_t)vy;
    }
    clamp_view();
    D.have_view = false;            /* 배율이 바뀐 영역을 새로 받을 때까지 비워 둔다 */
}

static void pan(int dx, int dy)
{
    int64_t step = (int64_t)D.scale * 8;
    int64_t vx = (int64_t)D.vx + dx * step, vy = (int64_t)D.vy + dy * step;
    D.vx = vx < 0 ? 0 : (uint32_t)vx;
    D.vy = vy < 0 ? 0 : (uint32_t)vy;
    clamp_view();
}

/* SGR 마우스: ESC [ < b ; x ; y (M|m) */
static void mouse(int b, int x, int y, bool press)
{
    int px = x - 1, py = (y - 1 - TOP) * 2;     /* 칸의 위쪽 점. 반칸 정밀도는 쓰지 않는다 */
    D.mx = px; D.my = py;
    bool motion = b & 32;
    int btn = b & 3;
    if (b & 64) { zoom(btn == 0 ? 1 : -1, px, py); return; }    /* 휠 */
    if (btn != 0) return;                                       /* 왼쪽 버튼만 */
    if (press && !motion) {                                     /* 누름 */
        D.drag = true; D.ax = px; D.ay = py; D.lx = px; D.ly = py;
        if (D.brush_mode) { stroke_begin(); stroke(px, py); }
    } else if (press && motion) {                               /* 끌기 */
        if (D.drag && D.brush_mode) {
            /* 마우스를 빨리 움직이면 보고 사이가 벌어진다. 마지막 점부터 붓 크기의 절반 간격으로 이어 칠한다 */
            int dx = px - D.lx, dy = py - D.ly;
            int n = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
            int gap = D.brush / 2 > 0 ? D.brush / 2 : 1;
            int steps = (n + gap - 1) / gap;
            int lx = D.lx, ly = D.ly;
            for (int k = 1; k <= steps; k++) stroke(lx + dx * k / steps, ly + dy * k / steps);
            D.lx = px; D.ly = py;
        }
    } else if (!press && D.drag) {                              /* 뗌 */
        D.drag = false;
        if (!D.brush_mode) {
            int x0 = D.ax < px ? D.ax : px, x1 = D.ax < px ? px : D.ax;
            int y0 = D.ay < py ? D.ay : py, y1 = (D.ay < py ? py : D.ay) + 1;   /* 칸 하나 = 점 둘, 아래 점까지 */
            /* 사각형의 캔버스 좌표 [X0, X1) × [Y0, Y1). 캔버스 밖으로 끈 부분은 캔버스 끝까지로 자른다 */
            int64_t X0 = (int64_t)D.vx + (int64_t)x0 * D.scale, X1 = (int64_t)D.vx + (int64_t)(x1 + 1) * D.scale;
            int64_t Y0 = (int64_t)D.vy + (int64_t)y0 * D.scale, Y1 = (int64_t)D.vy + (int64_t)(y1 + 1) * D.scale;
            if (X0 < 0) X0 = 0;
            if (Y0 < 0) Y0 = 0;
            if (X1 > CANVAS_W) X1 = CANVAS_W;
            if (Y1 > CANVAS_H) Y1 = CANVAS_H;
            if (X1 > X0 && Y1 > Y0) {
                uint32_t cx = (uint32_t)X0, cy = (uint32_t)Y0, w = (uint32_t)(X1 - X0), h = (uint32_t)(Y1 - Y0);
                uint8_t v = PALETTE[D.color];
                pthread_mutex_unlock(&D.mtx);
                send_draw(cx, cy, w, h, v);
                pthread_mutex_lock(&D.mtx);
            }
        }
    }
}

static void *input_tty_main(void *arg)
{
    (void)arg;
    unsigned char buf[256];
    size_t have = 0;
    for (;;) {
        ssize_t r = read(STDIN_FILENO, buf + have, sizeof buf - have);
        pthread_mutex_lock(&D.mtx);
        if (D.quit || r == 0 || (r < 0 && errno != EINTR)) { D.quit = true; pthread_cond_broadcast(&D.redraw); pthread_mutex_unlock(&D.mtx); return NULL; }
        if (r < 0) { pthread_mutex_unlock(&D.mtx); continue; }
        have += (size_t)r;
        size_t i = 0;
        while (i < have) {
            unsigned char c = buf[i];
            if (c == 27) {
                if (i + 2 >= have) break;                           /* 나머지가 아직 안 왔다 */
                if (buf[i + 1] == '[' && buf[i + 2] == '<') {        /* 마우스 */
                    size_t j = i + 3;
                    while (j < have && buf[j] != 'M' && buf[j] != 'm') j++;
                    if (j >= have) { if (have - i > 64) i = have; break; }
                    int b = 0, x = 0, y = 0;
                    char tmp[64];
                    size_t len = j - (i + 3) < sizeof tmp - 1 ? j - (i + 3) : sizeof tmp - 1;
                    memcpy(tmp, buf + i + 3, len); tmp[len] = 0;
                    if (sscanf(tmp, "%d;%d;%d", &b, &x, &y) == 3) mouse(b, x, y, buf[j] == 'M');
                    i = j + 1;
                } else if (buf[i + 1] == '[') {                     /* 방향키 */
                    switch (buf[i + 2]) {
                    case 'A': pan(0, -1); break;
                    case 'B': pan(0, 1); break;
                    case 'C': pan(1, 0); break;
                    case 'D': pan(-1, 0); break;
                    }
                    i += 3;
                } else {
                    i += 2;
                }
                continue;
            }
            switch (c) {
            case 'q': case 3: D.quit = true; break;                 /* 3 = Ctrl+C (원시 모드라 신호가 아니다) */
            case 'b': D.brush_mode = true; break;
            case 'r': D.brush_mode = false; break;
            case '[': if (D.brush > 1) D.brush--; break;
            case ']': if (D.brush < 32) D.brush++; break;
            case '+': case '=': zoom(1, view_w() / 2, view_h() / 2); break;
            case '-': zoom(-1, view_w() / 2, view_h() / 2); break;
            case '0': D.scale = fit_scale(); D.vx = D.vy = 0; D.have_view = false; break;
            case 'h': pan(-1, 0); break;
            case 'j': pan(0, 1); break;
            case 'k': pan(0, -1); break;
            case 'l': pan(1, 0); break;
            default:
                if (c >= '1' && c <= '9') D.color = c - '1';
            }
            i++;
        }
        memmove(buf, buf + i, have - i);
        have -= i;
        D.dirty = true;
        pthread_cond_signal(&D.redraw);
        bool q = D.quit;
        if (q) pthread_cond_broadcast(&D.redraw);
        pthread_mutex_unlock(&D.mtx);
        if (q) return NULL;
    }
}

/* ---- 입력 스레드 (명령 줄) ---- */

static bool wait_idle_locked(void)
{
    while (D.outstanding > 0 && !D.server_gone) pthread_cond_wait(&D.idle, &D.mtx);
    return !D.server_gone;
}

static void *input_cmd_main(void *arg)
{
    (void)arg;
    char line[256];
    while (fgets(line, sizeof line, stdin)) {
        unsigned x, y, w, h, v;
        int ms;
        pthread_mutex_lock(&D.mtx);
        bool gone = D.quit;
        pthread_mutex_unlock(&D.mtx);
        if (gone) break;
        if (sscanf(line, "draw %u %u %u %u %u", &x, &y, &w, &h, &v) == 5) {
            if (canvas_rect_ok(x, y, w, h) && v < 256) send_draw(x, y, w, h, (uint8_t)v);
            else printf("error draw %u %u %u %u\n", x, y, w, h);
        } else if (strncmp(line, "sync", 4) == 0) {
            pthread_mutex_lock(&D.mtx);
            bool ok = wait_idle_locked();
            printf("sync %s draw_ok=%llu draw_err=%llu\n", ok ? "ok" : "server-gone",
                   (unsigned long long)D.my_draw, (unsigned long long)D.my_err);
            pthread_mutex_unlock(&D.mtx);
        } else if (sscanf(line, "snap %u %u %u %u", &x, &y, &w, &h) == 4) {
            if (!canvas_rect_ok(x, y, w, h)) { printf("error snap\n"); fflush(stdout); continue; }
            pthread_mutex_lock(&D.mtx);
            wait_idle_locked();                 /* 앞서 보낸 DRAW 가 반영된 뒤에 받는다 */
            D.want_snap = true; D.snap_done = false;
            pthread_mutex_unlock(&D.mtx);
            int sr = send_snapshot(x, y, w, h);
            pthread_mutex_lock(&D.mtx);
            if (sr < 0) { D.want_snap = false; printf("error snap send\n"); }
            while (sr == 0 && !D.snap_done && !D.server_gone) pthread_cond_wait(&D.got, &D.mtx);
            if (D.snap_done) printf("snap %zu %016llx\n", D.snap_len, (unsigned long long)D.snap_sum);
            pthread_mutex_unlock(&D.mtx);
        } else if (strncmp(line, "stats", 5) == 0) {
            pthread_mutex_lock(&D.mtx);
            wait_idle_locked();
            D.want_stats = true; D.stats_done = false;
            pthread_mutex_unlock(&D.mtx);
            int sr = send_req(REQ_STATS, NULL, 0, NULL, false);
            pthread_mutex_lock(&D.mtx);
            if (sr < 0) { D.want_stats = false; printf("error stats send\n"); }
            while (sr == 0 && !D.stats_done && !D.server_gone) pthread_cond_wait(&D.got, &D.mtx);
            if (D.stats_done) printf("stats clients=%u draw=%llu\n", D.n_clients, (unsigned long long)D.srv_draw);
            pthread_mutex_unlock(&D.mtx);
        } else if (sscanf(line, "sleep %d", &ms) == 1) {
            if (ms > 0) sleep_ms(ms);
        } else if (strncmp(line, "quit", 4) == 0) {
            break;
        } else if (line[0] != '\n' && line[0] != '#') {
            printf("error unknown: %s", line);
        }
        fflush(stdout);
    }
    pthread_mutex_lock(&D.mtx);
    D.quit = true;
    pthread_cond_broadcast(&D.redraw);
    pthread_mutex_unlock(&D.mtx);
    return NULL;
}

/* ---- main ---- */

static int hello(void)
{
    msg_hdr_t h;
    memset(&h, 0, sizeof h);
    h.type = REQ_HELLO;
    h.client_id = D.id;
    h.req_id = atomic_fetch_add(&D.next_id, 1);
    uint64_t want = h.req_id;
    if (tp_send_req(&h, NULL) < 0) return -1;
    for (;;) {
        if (tp_recv_resp(&h, D.rbuf, MAX_PAYLOAD) < 0) return -1;
        if (h.req_id != want) continue;
        if (h.type != RESP_OK) { errno = EPROTO; return -1; }
        return 0;
    }
}

static void usage(const char *p)
{
    fprintf(stderr,
        "사용법: %s [-i 번호] [-I 갱신ms]\n"
        "  -i N   클라이언트 번호 0~%u (기본: 비어 있는 번호를 고른다)\n"
        "  -I MS  보이는 영역을 다시 받는 주기 (기본 150, 20~5000)\n"
        "  표준 입력이 터미널이면 마우스로 그리는 화면, 아니면 명령 줄(draw/sync/snap/stats/sleep/quit)을 읽는다\n",
        p, MAX_CLIENTS - 1);
}

int main(int argc, char **argv)
{
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    sigaddset(&sigs, SIGWINCH);
    sigaddset(&sigs, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &sigs, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    const char *base = strrchr(argv[0], '_');
    if (base && base[1]) IPC_NAME = base + 1;
    long id = -1;
    D.interval_ms = 150;
    int opt;
    while ((opt = getopt(argc, argv, "i:I:h")) != -1) {
        char *end;
        switch (opt) {
        case 'i': id = strtol(optarg, &end, 10); if (*end || id < 0 || id >= (long)MAX_CLIENTS) { usage(argv[0]); return 2; } break;
        case 'I': D.interval_ms = (int)strtol(optarg, &end, 10); if (*end || D.interval_ms < 20 || D.interval_ms > 5000) { usage(argv[0]); return 2; } break;
        default: usage(argv[0]); return opt == 'h' ? 0 : 2;
        }
    }
    D.tty = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    D.rbuf = malloc(MAX_PAYLOAD);
    D.view = malloc((size_t)CANVAS_W * CANVAS_H);
    if (!D.rbuf || !D.view) { perror("malloc"); return 1; }

    /* 접속: 번호를 주지 않았으면 0부터 비어 있는 번호를 찾는다 */
    char eb[128];
    int rc = -1;
    for (long i = id < 0 ? 0 : id; i < (long)MAX_CLIENTS; i++) {
        D.id = (uint32_t)i;
        rc = tp_client_open(D.id);
        if (rc == 0 || errno != EADDRINUSE || id >= 0) break;
    }
    if (rc < 0) {
        fprintf(stderr, "draw_%s: 접속 실패: %s\n", IPC_NAME,
                errno == ECONNREFUSED ? "서버가 없다" : errno == EADDRINUSE ? "빈 번호가 없다" : err_str(errno, eb, sizeof eb));
        return 1;
    }
    D.base_id = ((uint64_t)getpid() << 32) | 1;
    atomic_store(&D.next_id, D.base_id);
    if (hello() < 0) {
        fprintf(stderr, "draw_%s: HELLO 실패: %s\n", IPC_NAME, err_str(errno, eb, sizeof eb));
        tp_client_close();
        return 1;
    }
    D.base_id = atomic_load(&D.next_id);

    D.color = 0;
    D.brush_mode = true;
    D.brush = 3;
    if (D.tty) {
        if (term_setup() < 0) { perror("tcsetattr"); tp_client_close(); return 1; }
        term_size(&D.cols, &D.rows);
        D.scale = fit_scale();
        D.dirty = true;
    } else {
        D.cols = 64; D.rows = 34;
        D.scale = 16;
    }

    pthread_t t_recv, t_sig, t_in, t_ref = 0, t_scr = 0;
    pthread_create(&t_recv, NULL, recv_main, NULL);
    pthread_create(&t_sig, NULL, signal_main, NULL);
    if (D.tty) {
        pthread_create(&t_ref, NULL, refresh_main, NULL);
        pthread_create(&t_scr, NULL, screen_main, NULL);
        pthread_create(&t_in, NULL, input_tty_main, NULL);
    } else {
        pthread_create(&t_in, NULL, input_cmd_main, NULL);
    }

    /* 끝내기: 입력 스레드가 끝나거나(q, 입력 끝) 신호 스레드가 quit 을 세운다 */
    pthread_mutex_lock(&D.mtx);
    while (!D.quit) pthread_cond_wait(&D.redraw, &D.mtx);
    pthread_mutex_unlock(&D.mtx);
    if (D.tty) {
        pthread_join(t_ref, NULL);
        pthread_join(t_scr, NULL);
        term_restore();
        /* 입력 스레드는 키 하나를 더 기다리며 read 에 막혀 있을 수 있다. 분리해 두고 프로세스 종료로 정리한다 */
        pthread_detach(t_in);
    } else {
        pthread_join(t_in, NULL);
    }

    /* 띄운 요청의 응답을 기다린 뒤 마지막 STATS 를 보내고, 그 응답을 받으면 수신 스레드가 끝난다 */
    pthread_mutex_lock(&D.mtx);
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += 3;
    while (D.outstanding > 0 && !D.server_gone)
        if (pthread_cond_timedwait(&D.idle, &D.mtx, &dl) == ETIMEDOUT) break;
    bool gone = D.server_gone;
    pthread_mutex_unlock(&D.mtx);
    /* 마지막 STATS 를 보내지 못하면(서버가 끝남) 수신 스레드는 ECONNRESET 으로 끝난다 */
    if (gone || send_req(REQ_STATS, NULL, 0, NULL, true) < 0) {
        pthread_mutex_lock(&D.mtx);
        gone = D.server_gone;
        pthread_mutex_unlock(&D.mtx);
    }
    pthread_join(t_recv, NULL);
    if (!gone) {
        msg_hdr_t h;
        memset(&h, 0, sizeof h);
        h.type = REQ_BYE;
        h.client_id = D.id;
        h.req_id = atomic_fetch_add(&D.next_id, 1);
        tp_send_req(&h, NULL);
    }
    tp_client_close();
    pthread_kill(t_sig, SIGUSR1);
    pthread_join(t_sig, NULL);

    if (D.tty)
        printf("draw_%s: #%u 끝. 내가 그린 DRAW %llu개, 오류 %llu개%s\n", IPC_NAME, D.id,
               (unsigned long long)D.my_draw, (unsigned long long)D.my_err, gone ? " (서버가 먼저 끝났다)" : "");
    free(D.rbuf);
    free(D.view);
    free(M.bits);
    return gone && D.tty ? 1 : 0;
}
