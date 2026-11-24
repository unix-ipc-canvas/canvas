#!/usr/bin/env python3
"""T-19 사람이 그리는 클라이언트(draw) 시험. scripts/verify.sh 가 IPC 마다 부른다.

    python3 scripts/test-draw.py <ipc> <작업 폴더>

서버를 띄우고 세 가지를 본다. 결과는 마지막 줄에 "ok ..." 또는 "ng ..." 로 쓰고 종료 코드 0/1 을 돌려준다.
  1. 명령 줄 모드: draw 클라이언트 둘이 동시에 사각형 3,000개씩 그린다. 전체 SNAPSHOT 의 체크섬,
     서버 종료 체크섬, 이 스크립트가 같은 사각형을 다시 그려 낸 정답이 모두 같아야 한다.
  2. 화면 모드: 가상 터미널(pty)에서 draw 둘을 띄워 마우스로 붓 칠과 사각형을 그린다. 상태 줄에 접속 2명과
     보낸 DRAW 가 서버 처리 수로 보이고, q 로 끝나면 종료 코드 0, 화면 설정(대체 화면·마우스)이 되돌려져야 한다.
  3. 멈춘 서버: 서버를 kill -STOP 한 채 명령 줄 모드에 quit 을 주면 10초 안에, SIGINT 를 두 번 보내면 2초 안에
     종료 코드 1 로 끝나야 한다(마지막 STATS 의 응답을 정해진 시간까지만 기다린다). 서버를 다시 움직인 뒤에는
     입력을 기다리던 명령 줄 모드가 SIGINT 한 번에 3초 안에 종료 코드 0 으로 끝나야 한다.
"""
import os
import pty
import random
import re
import select
import signal
import struct
import subprocess
import sys
import time
import fcntl
import termios

ipc, work = sys.argv[1], sys.argv[2]
DRAW = f'bin/draw_{ipc}'


def fnv(buf):
    h = 0xcbf29ce484222325
    for b in buf:
        h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return h


def start_server():
    err = open(os.path.join(work, 'dsrv.err'), 'w')
    out = open(os.path.join(work, 'dsrv.out'), 'w')
    p = subprocess.Popen([f'bin/server_{ipc}', '-l', 'tile', '-p', os.path.join(work, 'd.pgm')], stdout=out, stderr=err)
    for _ in range(50):
        time.sleep(0.1)
        if '시작' in open(os.path.join(work, 'dsrv.err')).read():
            return p
    p.kill()
    raise SystemExit(f'ng {ipc}: 서버가 시작하지 않음')


def stop_server(p):
    p.send_signal(signal.SIGINT)
    try:
        rc = p.wait(10)
    except subprocess.TimeoutExpired:
        p.kill()
        rc = 99
    m = re.search(r'checksum ([0-9a-f]{16})', open(os.path.join(work, 'dsrv.out')).read())
    return rc, m.group(1) if m else None


def cmd_mode():
    canvas = bytearray(1024 * 1024)
    scripts = []
    for seed in (1, 2):
        r = random.Random(seed)
        lines = []
        for _ in range(3000):
            x, y = r.randrange(0, 1000), r.randrange(0, 1000)
            w, h = min(64, r.randrange(1, 1025 - x)), min(64, r.randrange(1, 1025 - y))
            v = r.randrange(1, 256)
            lines.append(f'draw {x} {y} {w} {h} {v}')
            for j in range(y, y + h):
                row = j * 1024
                for i in range(x, x + w):
                    canvas[row + i] = (canvas[row + i] + v) & 255
        lines.append('sync')
        scripts.append('\n'.join(lines) + '\n')
    want = f'{fnv(canvas):016x}'
    srv = start_server()
    procs = [subprocess.Popen([DRAW], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
             for _ in scripts]
    outs = [p.communicate(s, timeout=60)[0] for p, s in zip(procs, scripts)]
    last = subprocess.run([DRAW], input='snap 0 0 1024 1024\nquit\n', capture_output=True, text=True, timeout=30).stdout
    rc, sum_srv = stop_server(srv)
    m = re.search(r'snap 1048576 ([0-9a-f]{16})', last)
    sum_snap = m.group(1) if m else None
    synced = all('sync ok draw_ok=3000 draw_err=0' in o for o in outs)
    good = rc == 0 and synced and sum_snap == want and sum_srv == want and all(p.returncode == 0 for p in procs)
    return good, f'명령 줄: 둘이 3,000개씩, SNAPSHOT·서버·정답 체크섬 {sum_snap}/{sum_srv}/{want}, 응답 {"모두 OK" if synced else "누락"}'


def tty_mode():
    srv = start_server()
    users = []
    for _ in range(2):
        pid, fd = pty.fork()
        if pid == 0:
            os.execv(DRAW, [os.path.basename(DRAW)])
        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', 40, 120, 0, 0))
        os.kill(pid, signal.SIGWINCH)
        users.append((pid, fd))

    def drain(fd, t):
        out, end = b'', time.time() + t
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.05)
            if r:
                try:
                    out += os.read(fd, 1 << 16)
                except OSError:
                    break
        return out

    def mouse(fd, b, x, y, press):
        os.write(fd, f'\x1b[<{b};{x};{y}{"M" if press else "m"}'.encode())

    for _, fd in users:
        drain(fd, 0.8)
    a, b = users[0][1], users[1][1]
    # 붓으로 끌기: 누름 1 + 끌기 25. 클라이언트는 보고 사이를 붓 크기(기본 3)의 절반 간격으로 이어 칠하고,
    # 한 번 끄는 동안 이미 칠한 화면 점은 다시 보내지 않는다(DRAW는 더하기라 겹치면 두 번 더해진다).
    # 줄마다 칠하지 않은 구간 하나가 DRAW 하나다. 같은 규칙으로 보낼 DRAW 수를 센다.
    # 화면 120 × 40: 보기 120 × 76점, 배율 ceil(1024/76) = 14, 캔버스는 점 0~73
    pts = [(10, 10)] + [(x, 10 + (x - 10) // 3) for x in range(10, 60, 2)]
    pts = [(x - 1, (y - 2) * 2) for x, y in pts]
    vw, vh, sc = 120, 76, 14
    done, want = set(), 0

    def square(px, py):
        nonlocal want
        for y in range(max(py - 1, 0), min(py + 2, vh)):
            x, x1 = max(px - 1, 0), min(px + 2, vw)
            while x < x1:
                while x < x1 and (x, y) in done:
                    x += 1
                a = x
                while x < x1 and (x, y) not in done:
                    done.add((x, y)); x += 1
                if x > a and a * sc < 1024 and y * sc < 1024:
                    want += 1
    square(*pts[0])
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        n = max(abs(x1 - x0), abs(y1 - y0))
        for k in range(1, n + 1):                   # 간격 = 3 // 2 = 1
            square(x0 + (x1 - x0) * k // n, y0 + (y1 - y0) * k // n)
    pts = [(x + 1, y // 2 + 2) for x, y in pts]
    mouse(a, 0, *pts[0], True)
    for x, y in pts[1:]:
        mouse(a, 32, x, y, True)
    mouse(a, 0, 60, 27, False)
    os.write(b, b'r3')                              # 사각형 모드, 색 3
    mouse(b, 0, 30, 5, True)                        # 사각형 (화면 120 × 40 이면 캔버스는 왼쪽 64칸)
    mouse(b, 32, 45, 20, True)
    mouse(b, 0, 100, 30, False)                     # 캔버스 밖에서 떼면 캔버스 끝까지
    screen = drain(a, 1.5).decode('utf-8', 'replace')
    drain(b, 0.1)
    st = re.findall(r'접속 (\d+)명  서버 DRAW (\d+)', screen)
    for _, fd in users:
        os.write(fd, b'q')
    codes, restored = [], True
    for pid, fd in users:
        tail = drain(fd, 1.0)
        restored &= b'\x1b[?1049l' in tail and b'\x1b[?1000l' in tail
        _, s = os.waitpid(pid, 0)
        codes.append(os.waitstatus_to_exitcode(s))
    rc, _ = stop_server(srv)
    last = st[-1] if st else ('?', '?')
    good = rc == 0 and codes == [0, 0] and restored and last == ('2', str(want + 1))
    return good, f'화면: 마우스 붓 보고 26번(이어 칠한 DRAW {want}) + 사각형 1개, 상태 줄 접속 {last[0]}명·서버 DRAW {last[1]}, 종료 코드 {codes}, 터미널 복구 {restored}'


def stopped_server():
    srv = start_server()

    def spawn(first):
        p = subprocess.Popen([DRAW], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        p.stdin.write(first)
        p.stdin.flush()
        return p

    def wait_line(p, pat, t):
        end = time.time() + t
        while time.time() < end:
            r, _, _ = select.select([p.stdout], [], [], 0.1)
            if r:
                line = p.stdout.readline()
                if not line:
                    return False
                if pat in line:
                    return True
        return False

    def ends_within(p, t):
        t0 = time.time()
        try:
            rc = p.wait(t)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
            rc = None
        return rc, time.time() - t0

    # 서버가 살아 있을 때 둘을 붙여 둔다. 그린 것의 응답을 모두 받았으니 띄운 요청은 없다
    a, b = spawn('draw 0 0 8 8 1\nsync\n'), spawn('draw 8 8 8 8 2\nsync\n')
    synced = wait_line(a, 'sync ok', 10) and wait_line(b, 'sync ok', 10)
    srv.send_signal(signal.SIGSTOP)                 # 서버 터미널의 Ctrl+Z 와 같다
    a.stdin.write('quit\n')
    a.stdin.flush()
    rc_q, dt_q = ends_within(a, 10)                 # 마지막 STATS 의 응답을 3초까지만 기다린다
    b.send_signal(signal.SIGINT)                    # 끝내기 시작. 그다음 신호는 기다리지 않고 바로 끝낸다
    time.sleep(0.3)
    t0 = time.time()
    b.send_signal(signal.SIGINT)
    rc_i2, _ = ends_within(b, 2)
    dt_i2 = time.time() - t0
    srv.send_signal(signal.SIGCONT)
    c = spawn('sync\n')                             # 입력을 더 주지 않고 표준 입력을 열어 둔다
    ready = wait_line(c, 'sync ok', 10)
    c.send_signal(signal.SIGINT)
    rc_i1, dt_i1 = ends_within(c, 3)
    for p in (a, b, c):
        try:
            p.stdin.close()
        except OSError:
            pass
    rc, _ = stop_server(srv)
    good = synced and ready and rc_q == 1 and rc_i2 == 1 and rc_i1 == 0 and rc == 0
    return good, (f'멈춘 서버: quit {dt_q:.1f}초 뒤 종료 코드 {rc_q}, SIGINT 두 번 {dt_i2:.1f}초 뒤 {rc_i2}, '
                  f'다시 움직인 뒤 입력 대기 중 SIGINT {dt_i1:.1f}초 뒤 {rc_i1}, 서버 종료 코드 {rc}')


ok1, m1 = cmd_mode()
ok2, m2 = tty_mode()
ok3, m3 = stopped_server()
print(('ok ' if ok1 and ok2 and ok3 else 'ng ') + f'{ipc}: {m1}; {m2}; {m3}')
sys.exit(0 if ok1 and ok2 and ok3 else 1)
