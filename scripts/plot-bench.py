#!/usr/bin/env python3
"""1단계 결과 그래프와 요약표. results/bench_*.csv 에서만 만든다.

    python3 scripts/plot-bench.py [결과폴더] [그림폴더]
    기본: results/  docs/assets/bench/

같은 조건을 여러 번 잰 값은 실행마다의 값(평균, 중앙값, 상위 99%, 최대, 처리량)의 중앙값으로 그린다.
요약표는 그림 폴더의 summary.md 에 쓴다. 손으로 고치지 않는다.
matplotlib 이 필요하다 (pip install matplotlib).
"""
import csv
import os
import statistics
import sys
from collections import defaultdict

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt  # noqa: E402

RES = sys.argv[1] if len(sys.argv) > 1 else 'results'
FIG = sys.argv[2] if len(sys.argv) > 2 else 'docs/assets/bench'
IPCS = ['pipe', 'mq', 'shm']
COLOR = {'pipe': '#2563eb', 'mq': '#d97706', 'shm': '#059669'}
MARK = {'pipe': 'o', 'mq': 's', 'shm': '^'}


def load(name):
    path = os.path.join(RES, name)
    if not os.path.exists(path):
        return []
    with open(path, newline='') as f:
        return list(csv.DictReader(f))


def num(v):
    return float(v) if v not in ('', None) else None


def size_label(b):
    b = int(b)
    if b >= 1 << 20 and b % (1 << 20) == 0:
        return f'{b >> 20}MB'
    if b >= 1024 and b % 1024 == 0:
        return f'{b >> 10}KB'
    return f'{b}B'


def group(rows, role):
    """(ipc, size, clients) -> 실행별 행 목록"""
    g = defaultdict(list)
    for r in rows:
        if r['stage'] == 'bench' and r['role'] == role:
            g[(r['ipc'], int(r['msg_size']), int(r['clients']))].append(r)
    return g


def med(runs, col):
    vals = [num(r[col]) for r in runs if num(r[col]) is not None]
    return statistics.median(vals) if vals else None


def spread(runs, col):
    """실행 사이의 편차: (최대 - 최소) / 중앙값"""
    vals = [num(r[col]) for r in runs if num(r[col]) is not None]
    if len(vals) < 2:
        return None
    m = statistics.median(vals)
    return (max(vals) - min(vals)) / m if m else None


lat = group(load('bench_latency.csv'), 'client')
lat_srv = group(load('bench_latency.csv'), 'server')
thr = group(load('bench_throughput.csv'), 'client')
clock = [r for r in load('bench_clock.csv') if r['experiment'] == 'clock']
if not lat:
    sys.exit(f'{RES}/bench_latency.csv 가 없다')

sizes = sorted({k[1] for k in lat})
clients = sorted({k[2] for k in lat})
os.makedirs(FIG, exist_ok=True)


def size_axis(ax):
    ax.set_xscale('log', base=2)
    ax.set_xticks(sizes)
    ax.set_xticklabels([size_label(s) for s in sizes])
    ax.set_xlabel('Message size (payload)')
    ax.grid(True, which='both', alpha=0.3)


def save(fig, name):
    fig.tight_layout()
    fig.savefig(os.path.join(FIG, name), dpi=150)
    plt.close(fig)


# 1. 크기별 왕복 지연 (클라이언트 1, 동시 요청 1)
fig, ax = plt.subplots(figsize=(7.5, 4.8))
for ipc in IPCS:
    xs = [s for s in sizes if (ipc, s, 1) in lat]
    if not xs:
        continue
    ax.plot(xs, [med(lat[(ipc, s, 1)], 'lat_p50_us') for s in xs], marker=MARK[ipc], color=COLOR[ipc], label=f'{ipc} p50')
    ax.plot(xs, [med(lat[(ipc, s, 1)], 'lat_p99_us') for s in xs], ls='--', marker=MARK[ipc], mfc='none',
            color=COLOR[ipc], alpha=0.7, label=f'{ipc} p99')
ax.set_yscale('log')
ax.set_ylabel('Round-trip latency (µs, log)')
ax.set_title('Round-trip latency vs message size (1 client, 1 in flight)')
size_axis(ax)
ax.legend(ncol=3, fontsize=8)
save(fig, 'latency_size.png')

# 2. 크기별 처리량 (클라이언트 1, 동시 요청 INFLIGHT)
if thr:
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(11, 4.5))
    infl = next(iter(thr.values()))[0]['inflight']
    for ipc in IPCS:
        xs = [s for s in sizes if (ipc, s, 1) in thr]
        if not xs:
            continue
        a1.plot(xs, [med(thr[(ipc, s, 1)], 'thr_mb_s') for s in xs], marker=MARK[ipc], color=COLOR[ipc], label=ipc)
        a2.plot(xs, [med(thr[(ipc, s, 1)], 'thr_msg_s') for s in xs], marker=MARK[ipc], color=COLOR[ipc], label=ipc)
    a1.set_ylabel('Throughput (MB/s, 1 MB = 10^6 B)')
    a2.set_ylabel('Throughput (messages/s, log)')
    a2.set_yscale('log')
    for a in (a1, a2):
        size_axis(a)
        a.legend()
    fig.suptitle(f'Echo throughput vs message size (1 client, {infl} in flight)')
    save(fig, 'throughput_size.png')


def by_clients(data, col, ylabel, title, name, logy):
    show = [s for s in (64, 4096, 65536, 1048576) if s in sizes] or sizes[:4]
    fig, axes = plt.subplots(1, len(show), figsize=(3.6 * len(show), 3.8), squeeze=False)
    for ax, s in zip(axes[0], show):
        for ipc in IPCS:
            xs = [c for c in clients if (ipc, s, c) in data]
            if xs:
                ax.plot(xs, [med(data[(ipc, s, c)], col) for c in xs], marker=MARK[ipc], color=COLOR[ipc], label=ipc)
        ax.set_title(size_label(s))
        ax.set_xscale('log', base=2)
        ax.set_xticks(clients)
        ax.set_xticklabels([str(c) for c in clients])
        ax.set_xlabel('Concurrent clients')
        if logy:
            ax.set_yscale('log')
        ax.grid(True, which='both', alpha=0.3)
    axes[0][0].set_ylabel(ylabel)
    axes[0][0].legend()
    fig.suptitle(title)
    save(fig, name)


# 3, 4. 클라이언트 수별
by_clients(lat, 'lat_p50_us', 'Round-trip latency p50 (µs, log)',
           'Latency vs concurrent clients (1 in flight per client)', 'latency_clients.png', True)
if thr:
    by_clients(thr, 'thr_mb_s', 'Throughput (MB/s)', 'Throughput vs concurrent clients', 'throughput_clients.png', False)

# 5. 메시지 하나당 컨텍스트 스위치와 CPU 시간 (클라이언트 1, 지연 실행, 클라이언트 + 서버)
fig, (a1, a2) = plt.subplots(1, 2, figsize=(11, 4.5))
for ipc in IPCS:
    xs, cs, cpu = [], [], []
    for s in sizes:
        c, sv = lat.get((ipc, s, 1)), lat_srv.get((ipc, s, 1))
        if not c or not sv:
            continue
        n = med(c, 'n')
        xs.append(s)
        cs.append((med(c, 'nvcsw') + med(sv, 'nvcsw')) / n)
        cpu.append((med(c, 'utime_s') + med(c, 'stime_s') + med(sv, 'utime_s') + med(sv, 'stime_s')) / n * 1e6)
    if xs:
        a1.plot(xs, cs, marker=MARK[ipc], color=COLOR[ipc], label=ipc)
        a2.plot(xs, cpu, marker=MARK[ipc], color=COLOR[ipc], label=ipc)
a1.set_ylabel('Voluntary context switches per message')
a2.set_ylabel('CPU time per message (µs, user + sys)')
for a in (a1, a2):
    size_axis(a)
    a.set_yscale('log')
    a.legend()
fig.suptitle('Per-message cost, client + server (1 client, 1 in flight)')
save(fig, 'cost_size.png')

# ---- 요약표 ----
out = []
w = out.append
w('# 1단계 측정 요약\n')
w('`scripts/plot-bench.py` 가 `results/bench_*.csv` 에서 만든 파일이다. 손으로 고치지 않는다. '
  '같은 조건을 여러 번 잰 값은 실행마다의 값의 중앙값이다. 지연 단위는 µs, 처리량은 MB/s (1 MB = 10^6 B).\n')

env = os.path.join(RES, 'bench_env.txt')
if os.path.exists(env):
    keep = ('date:', 'commit:', 'reps:', 'Model name', 'CPU(s):', 'Thread(s) per core', 'Core(s) per socket', 'Mem:')
    lines = [l.rstrip() for l in open(env, encoding='utf-8') if l.startswith(keep)]
    w('측정 환경 (`results/bench_env.txt` 에서 일부)\n')
    w('```text\n' + '\n'.join(lines) + '\n```\n')

if clock:
    w(f'clock_gettime 한 번의 비용: 중앙값 {statistics.median(num(r["lat_p50_us"]) for r in clock) * 1000:.0f} ns, '
      f'평균 {statistics.median(num(r["lat_mean_us"]) for r in clock) * 1000:.1f} ns '
      f'({len(clock)}번 실행, 실행마다 {clock[0]["n"]}개)\n')

reps = max(len(v) for v in lat.values())
w(f'\n## 지연, 클라이언트 1, 동시 요청 1 ({reps}번 실행)\n')
w('| 크기 | IPC | 평균 | 중앙값 | 상위 99% | 최대 | p50 순위 | 실행 사이 p50 편차 |')
w('| --- | --- | ---: | ---: | ---: | ---: | :---: | ---: |')
rank_lat = {}
for s in sizes:
    p50 = {ipc: med(lat[(ipc, s, 1)], 'lat_p50_us') for ipc in IPCS if (ipc, s, 1) in lat}
    order = sorted(p50, key=p50.get)
    rank_lat[s] = order
    for ipc in IPCS:
        if (ipc, s, 1) not in lat:
            continue
        r = lat[(ipc, s, 1)]
        sp = spread(r, 'lat_p50_us')
        w(f'| {size_label(s)} | {ipc} | {med(r, "lat_mean_us"):.2f} | {med(r, "lat_p50_us"):.2f} | '
          f'{med(r, "lat_p99_us"):.2f} | {med(r, "lat_max_us"):.2f} | {order.index(ipc) + 1} | '
          f'{"-" if sp is None else f"{sp * 100:.0f}%"} |')

if thr:
    infl = next(iter(thr.values()))[0]['inflight']
    w(f'\n## 처리량, 클라이언트 1, 동시 요청 {infl}\n')
    w('| 크기 | ' + ' | '.join(f'{i} MB/s' for i in IPCS) + ' | ' + ' | '.join(f'{i} msg/s' for i in IPCS) + ' | 순위 |')
    w('| --- |' + ' ---: |' * (2 * len(IPCS)) + ' :---: |')
    for s in sizes:
        mb = {i: med(thr[(i, s, 1)], 'thr_mb_s') for i in IPCS if (i, s, 1) in thr}
        ms = {i: med(thr[(i, s, 1)], 'thr_msg_s') for i in IPCS if (i, s, 1) in thr}
        order = sorted(mb, key=mb.get, reverse=True)
        w(f'| {size_label(s)} | ' + ' | '.join(f'{mb[i]:.1f}' if i in mb else '-' for i in IPCS) + ' | '
          + ' | '.join(f'{ms[i]:.0f}' if i in ms else '-' for i in IPCS) + ' | ' + ' > '.join(order) + ' |')


def client_table(data, col, title, fmt):
    w(f'\n## {title}\n')
    w('| 크기 | 클라이언트 | ' + ' | '.join(IPCS) + ' | 순위 |')
    w('| --- | ---: |' + ' ---: |' * len(IPCS) + ' :---: |')
    for s in sizes:
        for c in clients:
            v = {i: med(data[(i, s, c)], col) for i in IPCS if (i, s, c) in data}
            if not v:
                continue
            best_low = col.startswith('lat')
            order = sorted(v, key=v.get, reverse=not best_low)
            w(f'| {size_label(s)} | {c} | ' + ' | '.join(fmt.format(v[i]) if i in v else '-' for i in IPCS)
              + ' | ' + (' < ' if best_low else ' > ').join(order) + ' |')


client_table(lat, 'lat_p50_us', '클라이언트 수별 지연 중앙값 (µs, 동시 요청 1)', '{:.2f}')
if thr:
    client_table(thr, 'thr_mb_s', '클라이언트 수별 처리량 (MB/s)', '{:.1f}')

w('\n## 순위가 바뀌는 지점 (클라이언트 1, 지연 중앙값)\n')
prev = None
flips = []
for s in sizes:
    if prev and rank_lat[s] != rank_lat[prev]:
        flips.append(f'- {size_label(prev)} → {size_label(s)}: {" < ".join(rank_lat[prev])} 에서 {" < ".join(rank_lat[s])} 로')
    prev = s
w('\n'.join(flips) if flips else '- 바뀌지 않음')

w('\n## 그림\n')
for name, cap in [('latency_size.png', '크기별 왕복 지연'), ('throughput_size.png', '크기별 처리량'),
                  ('latency_clients.png', '클라이언트 수별 지연'), ('throughput_clients.png', '클라이언트 수별 처리량'),
                  ('cost_size.png', '메시지당 컨텍스트 스위치와 CPU 시간')]:
    if os.path.exists(os.path.join(FIG, name)):
        w(f'![{cap}]({name})\n')

with open(os.path.join(FIG, 'summary.md'), 'w', encoding='utf-8') as f:
    f.write('\n'.join(out) + '\n')
print('\n'.join(out))
