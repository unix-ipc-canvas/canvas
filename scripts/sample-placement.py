#!/usr/bin/env python3
"""측정 중인 서버·클라이언트 스레드가 어느 CPU 에서 도는지와 CPU 주파수를 표본으로 모은다. 원인 확인 실험용.

    python3 scripts/sample-placement.py 출력.txt &      측정 스크립트를 돌리는 동안 함께 띄우고, 끝나면 kill
    python3 scripts/sample-placement.py --summary 출력.txt

0.1초마다 /proc 에서 server_<ipc>, client_<ipc> 의 스레드 가운데 실행 중(R)인 것의 CPU 번호와
/sys 의 CPU 별 현재 주파수를 한 줄씩 적는다. 서버 명령줄의 -w 값(워커 수)과 클라이언트 수를 함께 적어
조건별로 나눠 볼 수 있게 한다. 측정 대상의 동작은 바꾸지 않는다(읽기만 한다).
요약은 조건마다 실행 중 스레드 표본의 P코어(CPU 0~3) 비율, 그 스레드가 있던 CPU 의 평균 주파수를 낸다.
"""
import glob
import json
import os
import sys
import time
from collections import defaultdict

PCORES = {0, 1, 2, 3}          # 측정용 컴퓨터(Core 5 120U)의 P코어 논리 CPU. docs/measurement/README.md


def procs():
    out = []
    for d in glob.glob('/proc/[0-9]*'):
        try:
            with open(d + '/comm') as f:
                comm = f.read().strip()
        except OSError:
            continue
        if comm.startswith(('server_', 'client_')):
            out.append((d, comm))
    return out


def freqs():
    fr = {}
    for p in glob.glob('/sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_cur_freq'):
        cpu = int(p.split('/')[5][3:])
        try:
            with open(p) as f:
                fr[cpu] = int(f.read()) // 1000
        except OSError:
            pass
    return fr


def sample(path):
    with open(path, 'w') as out:
        while True:
            ps = procs()
            workers, nclient, run = None, 0, []
            for d, comm in ps:
                if comm.startswith('server_'):
                    try:
                        with open(d + '/cmdline', 'rb') as f:
                            args = f.read().split(b'\0')
                        workers = int(args[args.index(b'-w') + 1]) if b'-w' in args else 4
                    except (OSError, ValueError, IndexError):
                        pass
                else:
                    nclient += 1
                for t in glob.glob(d + '/task/*/stat'):
                    try:
                        with open(t) as f:
                            st = f.read().rsplit(')', 1)[1].split()
                    except OSError:
                        continue
                    if st[0] == 'R':
                        run.append((comm.split('_')[0], int(st[36])))
            if workers is not None and run:
                out.write(json.dumps({'t': round(time.time(), 2), 'w': workers, 'c': nclient,
                                      'run': run, 'f': freqs()}) + '\n')
                out.flush()
            time.sleep(0.1)


def summary(path):
    g = defaultdict(lambda: {'n': 0, 'p': 0, 'f': 0, 'srv': 0, 'srv_p': 0, 'samples': 0, 'running': 0})
    with open(path) as f:
        for line in f:
            r = json.loads(line)
            k = (r['c'], r['w'])
            s = g[k]
            s['samples'] += 1
            s['running'] += len(r['run'])
            for who, cpu in r['run']:
                s['n'] += 1
                s['p'] += cpu in PCORES
                s['f'] += r['f'].get(str(cpu), 0)
                if who == 'server':
                    s['srv'] += 1
                    s['srv_p'] += cpu in PCORES
    print('| 클라이언트 | 워커 | 표본 | 실행 중 스레드 평균 | P코어 비율 | 서버 스레드의 P코어 비율 | 그 CPU 의 평균 주파수 MHz |')
    print('| --- | --- | --- | --- | --- | --- | --- |')
    for k in sorted(g):
        s = g[k]
        if not s['n']:
            continue
        print(f"| {k[0]} | {k[1]} | {s['samples']} | {s['running'] / s['samples']:.1f} | {s['p'] / s['n'] * 100:.0f}% | "
              f"{(s['srv_p'] / s['srv'] * 100 if s['srv'] else 0):.0f}% | {s['f'] / s['n']:.0f} |")


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--summary':
        summary(sys.argv[2])
    elif len(sys.argv) == 2:
        sample(sys.argv[1])
    else:
        sys.exit(__doc__)
