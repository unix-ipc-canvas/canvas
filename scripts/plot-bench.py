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
IPCS = ['pipe', 'upipe', 'mq', 'shm']          # 그래프와 표에 그리는 순서
RANKED = ['pipe', 'mq', 'shm']                 # 순위는 과제의 세 IPC 로만 매긴다. upipe(익명 pipe)는 FIFO 와 비교하는 대조군
COLOR = {'pipe': '#2563eb', 'upipe': '#7c3aed', 'mq': '#d97706', 'shm': '#059669'}
MARK = {'pipe': 'o', 'upipe': 'D', 'mq': 's', 'shm': '^'}
LABEL = {'pipe': 'pipe (FIFO)', 'upipe': 'pipe (anonymous)', 'mq': 'mq', 'shm': 'shm'}


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


def core(vals):
    """실행별 값에서 좋은 쪽과 나쁜 쪽을 4분의 1씩 뺀 범위 (5번이면 가운데 3번, 20번이면 가운데 10번).
    4번 미만이면 전체 범위"""
    v = sorted(vals)
    k = len(v) // 4
    if k:
        v = v[k:-k]
    return v[0], v[-1]


def order_groups(data, key, col, low_better):
    """순위. data[(ipc,) + key] 의 실행별 값으로 중앙값 순서를 정하고, 앞 묶음과 가운데 범위(core)가
    겹치면 같은 묶음(≈)에 넣는다. 범위가 겹치는 차이는 실행 사이의 흔들림보다 작아 순위를 매기지 않는다.
    돌려주는 값: 좋은 쪽부터 묶음 목록 [[ipc, ...], ...]"""
    vals = {i: [num(r[col]) for r in data[(i,) + key]] for i in RANKED if (i,) + key in data}
    order = sorted(vals, key=lambda i: statistics.median(vals[i]), reverse=not low_better)
    groups = []
    for i in order:
        if groups:
            lo, hi = core(vals[i])
            if low_better:
                apart = max(core(vals[g])[1] for g in groups[-1]) < lo
            else:
                apart = min(core(vals[g])[0] for g in groups[-1]) > hi
            if not apart:
                groups[-1].append(i)
                continue
        groups.append([i])
    return groups


def order_str(groups, low_better):
    return (' < ' if low_better else ' > ').join(' ≈ '.join(sorted(g, key=RANKED.index)) for g in groups)


def group_rank(groups):
    rank, out = 1, {}
    for g in groups:
        for i in g:
            out[i] = rank
        rank += len(g)
    return out


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
    ax.plot(xs, [med(lat[(ipc, s, 1)], 'lat_p50_us') for s in xs], marker=MARK[ipc], color=COLOR[ipc],
            label=f'{LABEL[ipc]} p50')
    band = [core([num(r['lat_p50_us']) for r in lat[(ipc, s, 1)]]) for s in xs]
    ax.fill_between(xs, [b[0] for b in band], [b[1] for b in band], color=COLOR[ipc], alpha=0.12, lw=0)
    ax.plot(xs, [med(lat[(ipc, s, 1)], 'lat_p99_us') for s in xs], ls='--', marker=MARK[ipc], mfc='none',
            color=COLOR[ipc], alpha=0.7, label=f'{LABEL[ipc]} p99')
ax.set_yscale('log')
ax.set_ylabel('Round-trip latency (µs, log)')
ax.set_title('Round-trip latency vs message size (1 client, 1 in flight)')
ax.text(0.99, 0.02, 'shaded: p50 range over runs, best and worst quarter dropped', transform=ax.transAxes,
        ha='right', va='bottom', fontsize=7, color='0.35')
size_axis(ax)
ax.legend(ncol=2, fontsize=8)
save(fig, 'latency_size.png')

# 2. 크기별 처리량 (클라이언트 1, 동시 요청 INFLIGHT)
if thr:
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(11, 4.5))
    infl = next(iter(thr.values()))[0]['inflight']
    for ipc in IPCS:
        xs = [s for s in sizes if (ipc, s, 1) in thr]
        if not xs:
            continue
        a1.plot(xs, [med(thr[(ipc, s, 1)], 'thr_mb_s') for s in xs], marker=MARK[ipc], color=COLOR[ipc], label=LABEL[ipc])
        a2.plot(xs, [med(thr[(ipc, s, 1)], 'thr_msg_s') for s in xs], marker=MARK[ipc], color=COLOR[ipc], label=LABEL[ipc])
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
                ax.plot(xs, [med(data[(ipc, s, c)], col) for c in xs], marker=MARK[ipc], color=COLOR[ipc], label=LABEL[ipc])
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
        a1.plot(xs, cs, marker=MARK[ipc], color=COLOR[ipc], label=LABEL[ipc])
        a2.plot(xs, cpu, marker=MARK[ipc], color=COLOR[ipc], label=LABEL[ipc])
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
  '같은 조건을 여러 번 잰 값은 실행마다의 값의 중앙값이다. 지연 단위는 µs, 처리량은 MB/s (1 MB = 10^6 B). '
  '순위는 pipe, mq, shm 사이에서만 매긴다. upipe(익명 pipe)는 FIFO 와 이름 유무만 다른 대조군이다. '
  '순위를 매길 때는 실행별 값에서 좋은 쪽과 나쁜 쪽을 4분의 1씩 뺀 범위(20번이면 가운데 10번)를 보고, '
  '두 IPC 의 범위가 겹치면 차이가 실행 사이의 흔들림보다 작은 것이므로 ≈ 로 묶어 같은 순위로 둔다.\n')

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
    groups = order_groups(lat, (s, 1), 'lat_p50_us', True)
    rank_lat[s] = order_str(groups, True)
    ranks = group_rank(groups)
    for ipc in IPCS:
        if (ipc, s, 1) not in lat:
            continue
        r = lat[(ipc, s, 1)]
        sp = spread(r, 'lat_p50_us')
        rank = ranks.get(ipc, '-')
        w(f'| {size_label(s)} | {ipc} | {med(r, "lat_mean_us"):.2f} | {med(r, "lat_p50_us"):.2f} | '
          f'{med(r, "lat_p99_us"):.2f} | {med(r, "lat_max_us"):.2f} | {rank} | '
          f'{"-" if sp is None else f"{sp * 100:.0f}%"} |')

if thr:
    infl = next(iter(thr.values()))[0]['inflight']
    w(f'\n## 처리량, 클라이언트 1, 동시 요청 {infl}\n')
    w('| 크기 | ' + ' | '.join(f'{i} MB/s' for i in IPCS) + ' | ' + ' | '.join(f'{i} msg/s' for i in IPCS) + ' | 순위 |')
    w('| --- |' + ' ---: |' * (2 * len(IPCS)) + ' :---: |')
    for s in sizes:
        mb = {i: med(thr[(i, s, 1)], 'thr_mb_s') for i in IPCS if (i, s, 1) in thr}
        ms = {i: med(thr[(i, s, 1)], 'thr_msg_s') for i in IPCS if (i, s, 1) in thr}
        order = order_str(order_groups(thr, (s, 1), 'thr_mb_s', False), False)
        w(f'| {size_label(s)} | ' + ' | '.join(f'{mb[i]:.1f}' if i in mb else '-' for i in IPCS) + ' | '
          + ' | '.join(f'{ms[i]:.0f}' if i in ms else '-' for i in IPCS) + ' | ' + order + ' |')


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
            order = order_str(order_groups(data, (s, c), col, best_low), best_low)
            w(f'| {size_label(s)} | {c} | ' + ' | '.join(fmt.format(v[i]) if i in v else '-' for i in IPCS)
              + ' | ' + order + ' |')


client_table(lat, 'lat_p50_us', '클라이언트 수별 지연 중앙값 (µs, 동시 요청 1)', '{:.2f}')
if thr:
    client_table(thr, 'thr_mb_s', '클라이언트 수별 처리량 (MB/s)', '{:.1f}')

if any(k[0] == 'upipe' for k in lat):
    w('\n## FIFO 와 익명 pipe (클라이언트 1)\n')
    w('이름이 있는지만 다르고 커널 안의 pipe 버퍼는 같다. 비율이 1 에 가까우면 이름(경로 탐색·open)은 송수신 비용에 영향이 없다.\n')
    w('| 크기 | pipe p50 (µs) | upipe p50 (µs) | upipe / pipe | pipe MB/s | upipe MB/s |')
    w('| --- | ---: | ---: | ---: | ---: | ---: |')
    for s in sizes:
        a, b = lat.get(('pipe', s, 1)), lat.get(('upipe', s, 1))
        if not a or not b:
            continue
        pa, pb = med(a, 'lat_p50_us'), med(b, 'lat_p50_us')
        ta = med(thr[('pipe', s, 1)], 'thr_mb_s') if ('pipe', s, 1) in thr else None
        tb = med(thr[('upipe', s, 1)], 'thr_mb_s') if ('upipe', s, 1) in thr else None
        w(f'| {size_label(s)} | {pa:.2f} | {pb:.2f} | {pb / pa:.2f} | '
          f'{"-" if ta is None else f"{ta:.1f}"} | {"-" if tb is None else f"{tb:.1f}"} |')

w('\n## 순위가 바뀌는 지점 (클라이언트 1, 지연 중앙값)\n')
w('크기별 순위 (≈ 는 가운데 범위가 겹쳐 구분하지 않은 것):\n')
w('\n'.join(f'- {size_label(s)}: {rank_lat[s]}' for s in sizes))
prev = None
flips = []
for s in sizes:
    if prev and rank_lat[s] != rank_lat[prev]:
        flips.append(f'- {size_label(prev)} → {size_label(s)}: {rank_lat[prev]} 에서 {rank_lat[s]} 로')
    prev = s
w('\n바뀌는 곳:\n')
w('\n'.join(flips) if flips else '- 바뀌지 않음')

# 메시지당 자발적 컨텍스트 스위치 (cost_size.png 의 왼쪽 그림을 표로)
w('\n## 메시지당 자발적 컨텍스트 스위치 (클라이언트 1, 동시 요청 1)\n')
w('클라이언트 프로세스와 서버 프로세스의 `nvcsw` 를 더해 잰 메시지 수로 나눈 값이다(실행별 값의 중앙값). '
  '왕복 한 번에 막혀서 잠든 횟수로 볼 수 있다.\n')
w('| 크기 | ' + ' | '.join(IPCS) + ' |')
w('| --- |' + ' ---: |' * len(IPCS))
for s in sizes:
    cells = []
    for ipc in IPCS:
        c, sv = lat.get((ipc, s, 1)), lat_srv.get((ipc, s, 1))
        cells.append('-' if not c or not sv else f'{(med(c, "nvcsw") + med(sv, "nvcsw")) / med(c, "n"):.1f}')
    w(f'| {size_label(s)} | ' + ' | '.join(cells) + ' |')

# 처리량 실행의 긴 꼬리: 최대가 상위 99% 보다 훨씬 큰 조건
if thr:
    w(f'\n## 처리량 실행의 긴 꼬리 (동시 요청 {infl})\n')
    w('칸의 값은 최대 / 상위 99% (둘 다 실행별 값의 중앙값)와, 평균이 상위 99% 보다 큰 실행 수다. '
      '평균이 상위 99% 보다 크면 1% 도 안 되는 요청이 평균을 끌어올릴 만큼 오래 기다린 것이다.\n')
    w('| 크기 | 클라이언트 | ' + ' | '.join(IPCS) + ' |')
    w('| --- | ---: |' + ' ---: |' * len(IPCS))
    for s in sizes:
        for c in clients:
            cells = []
            for ipc in IPCS:
                r = thr.get((ipc, s, c))
                if not r:
                    cells.append('-')
                    continue
                tail = sum(1 for x in r if num(x['lat_mean_us']) > num(x['lat_p99_us']))
                cells.append(f'{med(r, "lat_max_us") / med(r, "lat_p99_us"):.1f}배 ({tail}/{len(r)})')
            w(f'| {size_label(s)} | {c} | ' + ' | '.join(cells) + ' |')

w('\n## 그림\n')
for name, cap in [('latency_size.png', '크기별 왕복 지연'), ('throughput_size.png', '크기별 처리량'),
                  ('latency_clients.png', '클라이언트 수별 지연'), ('throughput_clients.png', '클라이언트 수별 처리량'),
                  ('cost_size.png', '메시지당 컨텍스트 스위치와 CPU 시간')]:
    if os.path.exists(os.path.join(FIG, name)):
        w(f'![{cap}]({name})\n')

with open(os.path.join(FIG, 'summary.md'), 'w', encoding='utf-8') as f:
    f.write('\n'.join(out) + '\n')
print('\n'.join(out))
