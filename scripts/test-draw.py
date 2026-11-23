#!/usr/bin/env python3
"""T-19 사람이 그리는 클라이언트(draw) 시험. scripts/verify.sh 가 IPC 마다 부른다.

    python3 scripts/test-draw.py <ipc> <작업 폴더>

서버를 띄우고 두 가지를 본다. 결과는 마지막 줄에 "ok ..." 또는 "ng ..." 로 쓰고 종료 코드 0/1 을 돌려준다.
  1. 명령 줄 모드: draw 클라이언트 둘이 동시에 사각형 3,000개씩 그린다. 전체 SNAPSHOT 의 체크섬,
     서버 종료 체크섬, 이 스크립트가 같은 사각형을 다시 그려 낸 정답이 모두 같아야 한다.
  2. 화면 모드: 가상 터미널(pty)에서 draw 둘을 띄워 마우스로 붓 칠과 사각형을 그린다. 상태 줄에 접속 2명과
     보낸 DRAW 가 서버 처리 수로 보이고, q 로 끝나면 종료 코드 0, 화면 설정(대체 화면·마우스)이 되돌려져야 한다.
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
    mouse(a, 0, 10, 10, True)                       # 붓으로 끌기: 누름 1 + 끌기 25
    for x in range(10, 60, 2):
        mouse(a, 32, x, 10 + (x - 10) // 3, True)
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
    good = rc == 0 and codes == [0, 0] and restored and last == ('2', '27')
    return good, f'화면: 마우스 붓 칠 26번 + 사각형 1개, 상태 줄 접속 {last[0]}명·서버 DRAW {last[1]}, 종료 코드 {codes}, 터미널 복구 {restored}'


ok1, m1 = cmd_mode()
ok2, m2 = tty_mode()
print(('ok ' if ok1 and ok2 else 'ng ') + f'{ipc}: {m1}; {m2}')
sys.exit(0 if ok1 and ok2 else 1)
