#!/usr/bin/env python3
"""pipe 원인 확인 실험(T8, T9)의 요약표를 만든다. matplotlib 없이 돈다. 손으로 고치지 않는다.

    python3 scripts/pipe-experiments.py lock results/exp-pipe-lockwait 출력.md
    python3 scripts/pipe-experiments.py cpu  results/exp-pipecpu/bench_pipe.csv 출력.md

lock: exp-stage2.sh 의 pipelock 결과. 실행 폴더(<조건>-r<번>/)마다 클라이언트가 남긴 송신 잠금 기록
  (lw.<id>.<pid>: 기다리기 시작, 프로세스 안 mutex 를 얻음, fcntl 잠금을 얻음)을 모아
  - fcntl 대기 = 얻은 시각 - mutex 를 얻은 시각 (다른 프로세스와 다툰 시간)
  - 추월 = 내가 fcntl 을 기다리는 동안 다른 프로세스가 잠금을 얻은 횟수.
    먼저 기다린 순서대로 준다면(공정) 클라이언트 c 개에서 c - 1 을 넘지 않는다.
  를 세고, 같은 실행의 bench 지연(p99, 최대)과 나란히 놓는다. 값은 실행별 값의 중앙값(최대는 실행들 가운데 최댓값도 함께).
cpu: exp-stage2.sh 의 pipecpu 결과. 버퍼 크기 × 메시지 크기 × 동시 요청을 CPU 하나(taskset -c 0)와
  P코어 4개(taskset -c 0-3)에서 잰 bench_pipe client 줄. 버퍼 64KB 대비 지연 중앙값의 비율(>1 이면 느려짐)과
  메시지당 컨텍스트 스위치·CPU 시간을 보인다.
"""
import bisect
import csv
import glob
import os
import re
import statistics
import sys
from collections import defaultdict


def med(v):
    return statistics.median(v) if v else float('nan')


def pct(v, q):
    if not v:
        return float('nan')
    s = sorted(v)
    return s[min(len(s) - 1, int(q * len(s)))]


def lock_run(d):
    """실행 폴더 하나의 기록 → (클라이언트 수, fcntl 대기 ns 목록, 추월 목록)"""
    recs = []           # (pid, mtx, got)
    for path in glob.glob(os.path.join(d, 'lw.*')):
        pid = int(path.rsplit('.', 1)[1])
        with open(path, encoding='utf-8') as f:
            for line in f:
                if line.startswith('#') or not line.strip():
                    continue
                _, m, g = (int(x) for x in line.split())
                recs.append((pid, m, g))
    pids = {r[0] for r in recs}
    got = sorted((g, pid) for pid, _, g in recs)
    gt = [g for g, _ in got]
    waits, over = [], []
    for pid, m, g in recs:
        waits.append(g - m)
        lo, hi = bisect.bisect_right(gt, m), bisect.bisect_left(gt, g)
        over.append(sum(1 for i in range(lo, hi) if got[i][1] != pid))
    return len(pids), waits, over


def bench_rows(csv_path):
    out = {}
    if not os.path.exists(csv_path):
        return out
    with open(csv_path, newline='', encoding='utf-8') as f:
        for r in csv.DictReader(f):
            if r['role'] == 'client':
                out.setdefault((r['experiment'], r['run']), []).append(r)
    return out


def cmd_lock(root, out):
    bench = bench_rows(os.path.join(root, 'bench_pipe.csv'))
    groups = defaultdict(list)
    for d in sorted(glob.glob(os.path.join(root, '*-r*'))):
        m = re.match(r'(.+)-r(\d+)$', os.path.basename(d))
        if not m or not os.path.isdir(d):
            continue
        cond, run = m.group(1), m.group(2)
        nc, waits, over = lock_run(d)
        if not waits:
            continue
        b = bench.get(('lw-' + cond, run), [])
        p99 = max((float(r['lat_p99_us']) for r in b), default=float('nan'))
        mx = max((float(r['lat_max_us']) for r in b), default=float('nan'))
        groups[cond].append(dict(nc=nc, n=len(waits), w50=pct(waits, .5) / 1e3, w99=pct(waits, .99) / 1e3,
                                 wmax=max(waits) / 1e3, omax=max(over), ofair=sum(o > nc - 1 for o in over) / len(over),
                                 p99=p99, lmax=mx, share=(max(waits) / 1e3) / mx if mx == mx and mx > 0 else float('nan')))

    def key(c):
        m = re.match(r'c(\d+)-s(\d+)', c)
        return (int(m.group(2)), int(m.group(1))) if m else (0, 0)

    lines = ['# pipe 송신 잠금 대기와 긴 꼬리 (pipelock)', '',
             '`scripts/pipe-experiments.py lock` 이 만든 표. 값은 실행별 값의 중앙값이고, 괄호는 실행들 가운데 최댓값이다.', '',
             '| 크기 | 클라이언트 | 실행 | 잠금 횟수 | fcntl 대기 p50 µs | p99 µs | 최대 µs | 최대 추월 | 공정하면 넘지 않을 추월(c-1)을 넘은 비율 | bench 지연 p99 µs | bench 지연 최대 µs | 최대 대기 / 최대 지연 |',
             '| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |']
    for c in sorted(groups, key=key):
        g = groups[c]
        s, nc = key(c)
        f = lambda k, d=1: f"{med([x[k] for x in g]):.{d}f}"
        mx = lambda k, d=1: f"{max(x[k] for x in g):.{d}f}"
        lines.append(f"| {s} | {nc} | {len(g)} | {f('n', 0)} | {f('w50')} | {f('w99')} | {f('wmax')} ({mx('wmax')}) | "
                     f"{f('omax', 0)} ({mx('omax', 0)}) | {100 * med([x['ofair'] for x in g]):.2f}% | {f('p99')} | "
                     f"{f('lmax')} ({mx('lmax')}) | {f('share', 2)} |")
    lines += ['', '- 추월이 클라이언트 수 - 1 을 넘으면 먼저 기다린 클라이언트를 나중에 온 다른 클라이언트가 앞질렀다는 뜻이다.',
              '- 최대 대기 / 최대 지연이 1 에 가까우면 가장 느린 요청의 시간 대부분이 송신 잠금 대기였다는 뜻이다.']
    with open(out, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print('\n'.join(lines))


def cmd_cpu(csv_path, out):
    rows = defaultdict(list)
    with open(csv_path, newline='', encoding='utf-8') as f:
        for r in csv.DictReader(f):
            if r['role'] != 'client':
                continue
            m = re.match(r'cpu-(\w+)-buf(\d+)$', r['experiment'])
            if not m:
                continue
            rows[(m.group(1), int(r['msg_size']), int(r['inflight']), int(m.group(2)))].append(r)
    places = sorted({k[0] for k in rows}, key=lambda p: (p != 'one', p))
    bufs = sorted({k[3] for k in rows})
    lines = ['# pipe 버퍼 크기 × CPU 수 (pipecpu)', '',
             '`scripts/pipe-experiments.py cpu` 가 만든 표. bench_pipe 클라이언트 1개, 실행별 값의 중앙값.',
             '비율은 같은 배치·크기·동시 요청에서 버퍼 64KB 대비 지연 p50 (>1 이면 느려짐).', '']
    for p in places:
        lines += [f'## {"CPU 하나 (taskset -c 0)" if p == "one" else "P코어 4개 (taskset -c 0-3)" if p == "pcore" else p}', '',
                  '| 메시지 | 동시 요청 | ' + ' | '.join(f'버퍼 {b // 1024}KB 지연 p50 µs' for b in bufs) + ' | ' +
                  ' | '.join(f'{b // 1024}KB 비율' for b in bufs[1:]) + ' | ' +
                  ' | '.join(f'{b // 1024}KB 전환/메시지' for b in bufs) + ' | ' +
                  ' | '.join(f'{b // 1024}KB CPU µs/메시지' for b in bufs) + ' |',
                  '| ---: | ---: |' + ' ---: |' * (len(bufs) * 4 - 1)]
        for s in sorted({k[1] for k in rows if k[0] == p}):
            for fl in sorted({k[2] for k in rows if k[0] == p and k[1] == s}):
                lat, csw, cpu = {}, {}, {}
                for b in bufs:
                    rs = rows.get((p, s, fl, b), [])
                    lat[b] = med([float(r['lat_p50_us']) for r in rs])
                    csw[b] = med([(float(r['nvcsw']) + float(r['nivcsw'])) / float(r['n']) for r in rs])
                    cpu[b] = med([(float(r['utime_s']) + float(r['stime_s'])) * 1e6 / float(r['n']) for r in rs])
                base = lat[bufs[0]]
                lines.append(f'| {s // 1024}KB | {fl} | ' + ' | '.join(f'{lat[b]:.1f}' for b in bufs) + ' | ' +
                             ' | '.join(f'{lat[b] / base:.2f}' for b in bufs[1:]) + ' | ' +
                             ' | '.join(f'{csw[b]:.2f}' for b in bufs) + ' | ' +
                             ' | '.join(f'{cpu[b]:.1f}' for b in bufs) + ' |')
        lines.append('')
    lines += ['- 전환/메시지와 CPU 시간은 클라이언트 프로세스의 값이다(서버 줄은 같은 CSV 의 server 줄).']
    with open(out, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print('\n'.join(lines))


if __name__ == '__main__':
    if len(sys.argv) != 4 or sys.argv[1] not in ('lock', 'cpu'):
        print(__doc__)
        sys.exit(2)
    (cmd_lock if sys.argv[1] == 'lock' else cmd_cpu)(sys.argv[2], sys.argv[3])
