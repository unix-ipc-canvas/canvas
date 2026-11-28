#!/usr/bin/env python3
"""1단계 보강(클라이언트 수 확장) 그래프와 요약표. results/exp-clients/bench_latency.csv 에서만 만든다.

    python3 scripts/plot-clients.py [결과폴더] [그림폴더]
    기본: results/exp-clients/  docs/assets/bench/exp-clients/

모든 실행이 동시 요청 1(응답을 받아야 다음 요청을 보낸다)이라, 처리량은 같은 실행의
(잰 메시지 수) / (가장 먼저 시작한 클라이언트부터 가장 늦게 끝난 클라이언트까지)다.
같은 조건을 여러 번 잰 값은 실행마다의 값의 중앙값이다. 순위는 실행별 값에서 좋은 쪽과 나쁜 쪽을
4분의 1씩 뺀 범위(30번이면 가운데 15번)가 겹치면 ≈ 로 묶는다(plot-bench.py 와 같은 규칙).
요약표는 그림 폴더의 summary.md 에 쓴다. 손으로 고치지 않는다. matplotlib 이 필요하다.
"""
import csv
import os
import statistics
import sys
from collections import defaultdict

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt  # noqa: E402

RES = sys.argv[1] if len(sys.argv) > 1 else 'results/exp-clients'
FIG = sys.argv[2] if len(sys.argv) > 2 else 'docs/assets/bench/exp-clients'
IPCS = ['pipe', 'mq', 'shm']
COLOR = {'pipe': '#2563eb', 'mq': '#d97706', 'shm': '#059669'}
MARK = {'pipe': 'o', 'mq': 's', 'shm': '^'}
STAGE1_MAX = 8          # 1단계 측정(results/stage1-r20/)이 잰 가장 많은 클라이언트 수


def num(v):
    return float(v) if v not in ('', None) else None


def size_label(b):
    b = int(b)
    if b >= 1024 and b % 1024 == 0:
        return f'{b >> 10}KB'
    return f'{b}B'


def count_label(v):
    if v >= 1e6:
        return f'{v / 1e6:.2f}M'
    if v >= 1e4:
        return f'{v / 1e3:.1f}k'
    return f'{v:,.0f}'


def us_label(v):
    if v >= 1e6:
        return f'{v / 1e6:.2f}s'
    if v >= 1e3:
        return f'{v / 1e3:.1f}ms'
    return f'{v:.1f}µs'


path = os.path.join(RES, 'bench_latency.csv')
if not os.path.exists(path):
    sys.exit(f'{path} 가 없다')
with open(path, newline='', encoding='utf-8') as f:
    rows = [r for r in csv.DictReader(f) if r['stage'] == 'bench']

cli = defaultdict(list)            # (ipc, size, clients) -> client 줄
srv = {}                           # (ipc, size, clients, run) -> server 줄
for r in rows:
    key = (r['ipc'], int(r['msg_size']), int(r['clients']))
    if r['role'] == 'client':
        cli[key].append(r)
    elif r['role'] == 'server':
        srv[key + (r['run'],)] = r

sizes = sorted({k[1] for k in cli})
clients = sorted({k[2] for k in cli})


def vals(key, col):
    return [num(r[col]) for r in cli[key] if num(r[col]) is not None]


def med(key, col):
    v = vals(key, col)
    return statistics.median(v) if v else None


def csw_per_msg(key):
    """실행마다 (클라이언트 합 + 서버) 자발적 문맥 교환 / 잰 메시지 수, 그 중앙값"""
    out = []
    for r in cli[key]:
        s = srv.get(key + (r['run'],))
        n = num(r['n'])
        if s and n:
            out.append((num(r['nvcsw']) + num(s['nvcsw'])) / n)
    return statistics.median(out) if out else None


def core(v):
    v = sorted(v)
    k = len(v) // 4
    if k:
        v = v[k:-k]
    return v[0], v[-1]


def order_str(size, c, col, low_better):
    data = {i: vals((i, size, c), col) for i in IPCS if (i, size, c) in cli}
    order = sorted(data, key=lambda i: statistics.median(data[i]), reverse=not low_better)
    groups = []
    for i in order:
        if groups:
            lo, hi = core(data[i])
            if low_better:
                apart = max(core(data[g])[1] for g in groups[-1]) < lo
            else:
                apart = min(core(data[g])[0] for g in groups[-1]) > hi
            if not apart:
                groups[-1].append(i)
                continue
        groups.append([i])
    return (' < ' if low_better else ' > ').join(' ≈ '.join(sorted(g, key=IPCS.index)) for g in groups)


os.makedirs(FIG, exist_ok=True)

# ---- 그림: 크기마다 한 줄, 왼쪽 처리량, 오른쪽 왕복 지연(중앙값 실선, 상위 99% 점선) ----
fig, axes = plt.subplots(len(sizes), 2, figsize=(11, 3.9 * len(sizes)), squeeze=False)
for row, s in enumerate(sizes):
    a1, a2 = axes[row]
    for ipc in IPCS:
        xs = [c for c in clients if (ipc, s, c) in cli]
        if not xs:
            continue
        a1.plot(xs, [med((ipc, s, c), 'thr_msg_s') for c in xs], marker=MARK[ipc], color=COLOR[ipc], label=ipc)
        band = [core(vals((ipc, s, c), 'thr_msg_s')) for c in xs]
        a1.fill_between(xs, [b[0] for b in band], [b[1] for b in band], color=COLOR[ipc], alpha=0.13, lw=0)
        a2.plot(xs, [med((ipc, s, c), 'lat_p50_us') for c in xs], marker=MARK[ipc], color=COLOR[ipc],
                label=f'{ipc} p50')
        a2.plot(xs, [med((ipc, s, c), 'lat_p99_us') for c in xs], marker=MARK[ipc], mfc='none', ls='--',
                color=COLOR[ipc], alpha=0.75, label=f'{ipc} p99')
    a1.set_ylabel('Throughput (messages/s, log)')
    a2.set_ylabel('Round-trip latency (µs, log)')
    a1.set_title(f'{size_label(s)} echo: throughput')
    a2.set_title(f'{size_label(s)} echo: latency')
    for a in (a1, a2):
        a.set_xscale('log', base=2)
        a.set_yscale('log')
        a.set_xticks(clients)
        a.set_xticklabels([str(c) for c in clients])
        a.set_xlabel('Concurrent clients (1 in flight each)')
        a.axvspan(clients[0] / 1.3, STAGE1_MAX * 1.3, color='0.5', alpha=0.08, lw=0)
        a.grid(True, which='both', alpha=0.3)
    a1.text(0.02, 0.03, f'shaded gray: stage-1 range (clients <= {STAGE1_MAX})\ncolored band: middle half of runs',
            transform=a1.transAxes, fontsize=7, color='0.35', va='bottom')
    a1.legend(fontsize=8)
    a2.legend(fontsize=7, ncol=3)
fig.suptitle('Stage-1 echo benchmark extended to 1024 clients (median of runs)')
fig.tight_layout()
fig.savefig(os.path.join(FIG, 'clients.png'), dpi=150)
plt.close(fig)

# ---- 요약표 ----
out = []
w = out.append
w('# 1단계 보강 측정 요약 (클라이언트 수 확장)\n')
w(f'`scripts/plot-clients.py` 가 `{RES}/bench_latency.csv` 에서 만든 파일이다. 손으로 고치지 않는다. '
  '모든 실행은 동시 요청 1이고, 처리량은 같은 실행의 (잰 메시지 수) / (걸린 시간)이다. 값은 실행마다의 값의 중앙값이다. '
  '순위는 실행별 값에서 좋은 쪽과 나쁜 쪽을 4분의 1씩 뺀 범위가 겹치면 ≈ 로 묶는다. '
  '문맥 교환은 클라이언트 전체와 서버의 자발적 문맥 교환을 더해 잰 메시지 수로 나눈 값이다.\n')
env = os.path.join(RES, 'bench_env.txt')
if os.path.exists(env):
    keep = ('date:', 'commit:', 'reps:', 'power:', 'Model name', 'CPU(s):', '## 측정 끝')
    lines = [l.rstrip() for l in open(env, encoding='utf-8') if l.startswith(keep)]
    w(f'측정 환경 (`{RES}/bench_env.txt` 에서 일부)\n')
    w('```text\n' + '\n'.join(lines) + '\n```\n')
runs = {k: len(v) for k, v in cli.items()}
w(f'조건·IPC 마다 실행 {min(runs.values())}~{max(runs.values())}번, 모든 줄 {len(rows)}개.\n')
for s in sizes:
    w(f'\n## {size_label(s)}\n')
    w('| 클라이언트 | 처리량 pipe / mq / shm (msg/s) | 처리량 순위 | 지연 중앙값 pipe / mq / shm | '
      '상위 99% pipe / mq / shm | 문맥 교환/메시지 pipe / mq / shm |')
    w('| ---: | --- | --- | --- | --- | --- |')
    for c in clients:
        if not any((i, s, c) in cli for i in IPCS):
            continue

        def cells(f):
            return ' / '.join(f(i) if (i, s, c) in cli else '-' for i in IPCS)
        w(f'| {c} | {cells(lambda i: count_label(med((i, s, c), "thr_msg_s")))} | '
          f'{order_str(s, c, "thr_msg_s", False)} | '
          f'{cells(lambda i: us_label(med((i, s, c), "lat_p50_us")))} | '
          f'{cells(lambda i: us_label(med((i, s, c), "lat_p99_us")))} | '
          f'{cells(lambda i: f"{csw_per_msg((i, s, c)):.1f}" if csw_per_msg((i, s, c)) is not None else "-")} |')

w('\n## 그림\n')
w('![1단계 echo, 클라이언트 1~1024. 왼쪽 처리량, 오른쪽 왕복 지연(실선 중앙값, 점선 상위 99%). '
  '회색 바탕은 1단계 측정이 잰 범위(8개 이하)](clients.png)\n')
with open(os.path.join(FIG, 'summary.md'), 'w', encoding='utf-8') as f:
    f.write('\n'.join(out) + '\n')
print('\n'.join(out))
