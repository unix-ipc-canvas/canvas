#!/usr/bin/env python3
"""2단계 결과 그래프와 요약표. results/app_*.csv 에서만 만든다.

    python3 scripts/plot-app.py [결과폴더] [그림폴더]
    기본: results/  docs/assets/app/

한 실행(같은 실험, 같은 반복 번호, 같은 조건)의 클라이언트 줄을 먼저 하나로 모은다.
  처리량      모든 클라이언트의 요청 수 합 / 가장 긴 elapsed_s. 클라이언트는 -G 로 측정을 함께 시작한다
  지연        클라이언트마다의 중앙값·상위 99% 의 중앙값 (클라이언트 1 이면 그 값)
  구간별 시간  클라이언트의 구간 평균을 요청 수로 가중 평균
그 다음 같은 조건의 실행들(반복)의 중앙값을 쓰고, 순위는 1단계와 같은 규칙(가운데 범위가 겹치면 ≈)으로 매긴다.
요약표는 그림 폴더의 summary.md 에 쓴다. 손으로 고치지 않는다. matplotlib 이 필요하다.
"""
import csv
import glob
import os
import statistics
import sys
from collections import defaultdict

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt  # noqa: E402

RES = sys.argv[1] if len(sys.argv) > 1 else 'results'
FIG = sys.argv[2] if len(sys.argv) > 2 else 'docs/assets/app'
IPCS = ['pipe', 'mq', 'shm']
COLOR = {'pipe': '#2563eb', 'mq': '#d97706', 'shm': '#059669'}
MARK = {'pipe': 'o', 'mq': 's', 'shm': '^'}
SEGS = [('req_us', 'request'), ('queue_us', 'queue'), ('lock_us', 'lock wait'), ('work_us', 'work'), ('resp_us', 'response')]
KEY = ('clients', 'inflight', 'workers', 'lock', 'tile', 'draw_pct', 'snap')


def f(v):
    return float(v) if v not in ('', None) else None


def summarize(cs, sv):
    """한 실행의 클라이언트 줄 cs 와 서버 줄 sv 를 요약 하나로"""
    n = sum(int(r['n']) for r in cs)
    el = max(f(r['elapsed_s']) for r in cs)
    s = {'thr': n / el, 'p50': statistics.median(f(r['lat_p50_us']) for r in cs),
         'p99': statistics.median(f(r['lat_p99_us']) for r in cs),
         'mean': sum(f(r['lat_mean_us']) * int(r['n']) for r in cs) / n,
         'cl_cpu': sum(f(r['utime_s']) + f(r['stime_s']) for r in cs),
         'cl_nvcsw': sum(int(r['nvcsw']) for r in cs)}
    for col, _ in SEGS:
        s[col] = sum(f(r[col]) * int(r['n']) for r in cs) / n
    if sv:
        s['srv_cpu'] = f(sv['utime_s']) + f(sv['stime_s'])
        s['srv_nvcsw'] = int(sv['nvcsw'])
    return s


def load():
    """실험 -> {(ipc, 조건) -> [실행별 요약 dict]}.
    scripts/app-bench.sh 는 한 실행의 줄을 클라이언트 줄들, 서버 줄 하나 순서로 한꺼번에 덧붙이므로 서버 줄에서 실행을 끊는다"""
    out = {}
    for path in sorted(glob.glob(os.path.join(RES, 'app_*.csv'))):
        data = defaultdict(list)
        cs = []
        with open(path, newline='') as fh:
            for r in csv.DictReader(fh):
                if r['stage'] != 'app':
                    continue
                if r['role'] == 'client':
                    cs.append(r)
                    continue
                keys = {tuple(c[k] for k in KEY) for c in cs}
                ok = (len(keys) == 1 and len(cs) == int(cs[0]['clients'])
                      and all(c['ipc'] == r['ipc'] and c['run'] == r['run'] for c in cs))
                if ok:
                    data[(r['ipc'],) + keys.pop()].append(summarize(cs, r))
                else:
                    print(f'{path}: 짝이 맞지 않는 실행을 건너뛴다 (서버 줄 run={r["run"]}, 클라이언트 줄 {len(cs)}개)', file=sys.stderr)
                cs = []
        if data:
            out[os.path.basename(path)[4:-4]] = data
    return out


def med(runs, col):
    v = [r[col] for r in runs if r.get(col) is not None]
    return statistics.median(v) if v else None


def core(vals):
    v = sorted(vals)
    k = len(v) // 4
    if k:
        v = v[k:-k]
    return v[0], v[-1]


def order(data, key, col, low_better):
    vals = {i: [r[col] for r in data[(i,) + key]] for i in IPCS if (i,) + key in data}
    seq = sorted(vals, key=lambda i: statistics.median(vals[i]), reverse=not low_better)
    groups = []
    for i in seq:
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
    return (' < ' if low_better else ' > ').join(' ≈ '.join(sorted(g, key=IPCS.index)) for g in groups)


def spread(runs, col):
    v = [r[col] for r in runs]
    if len(v) < 2:
        return None
    m = statistics.median(v)
    return (max(v) - min(v)) / m * 100 if m else None


def fmt(v, d=0):
    if v is None:
        return '-'
    return f'{v:,.{d}f}'


def keys_of(data):
    return sorted({k[1:] for k in data}, key=lambda k: tuple(int(x) if x.lstrip('-').isdigit() else x for x in k))


def label(key, vary):
    names = dict(zip(KEY, key))
    return ', '.join(f'{v}={names[v]}' for v in vary)


VARY = {'base': ('inflight',), 'type': ('inflight', 'draw_pct', 'snap'), 'draw': ('clients', 'draw_pct'),
        'clients': ('clients',), 'workers': ('workers',), 'lock': ('lock',), 'tile': ('tile',)}
TITLE = {'base': '기준 조건 (클라이언트 4, DRAW 90%, -S 256)', 'type': '요청 종류별 비용 (클라이언트 1)',
         'draw': 'DRAW 비율', 'clients': '클라이언트 수 (-f 16)', 'workers': '워커 수 (클라이언트 8, -f 16)',
         'lock': '락 방식 (클라이언트 4, -f 16)', 'tile': '타일 크기 (-l tile, 클라이언트 4, -f 16)'}


def table(lines, exp, data):
    vary = VARY.get(exp, KEY)
    nrep = max(len(v) for v in data.values())
    lines += [f'## {TITLE.get(exp, exp)}', '', f'`results/app_{exp}.csv`, 조건마다 {nrep}번 실행의 중앙값. '
              '처리량은 초당 요청 수, 지연은 µs. 편차는 실행 사이의 (최대 - 최소) / 중앙값.', '']
    lines += ['| 조건 | 지표 | pipe | mq | shm | 순위 | 편차 pipe / mq / shm |', '| --- | --- | --- | --- | --- | --- | --- |']
    for key in keys_of(data):
        lab = label(key, vary)
        inflight = int(dict(zip(KEY, key))['inflight'])
        mets = [('thr', '처리량', False, 0)] if inflight > 1 else [('p50', '지연 중앙값', True, 2), ('p99', '지연 상위 99%', True, 1)]
        for col, name, low, d in mets:
            row = [fmt(med(data.get((i,) + key, []), col), d) for i in IPCS]
            sp = ' / '.join(fmt(spread(data[(i,) + key], col)) + '%' if (i,) + key in data else '-' for i in IPCS)
            lines.append(f'| {lab} | {name} | ' + ' | '.join(row) + f' | {order(data, key, col, low)} | {sp} |')
    lines.append('')
    lines += ['구간별 시간 (µs, 요청 하나의 평균. 요청 전송 / 큐 대기 / 락 대기 / 처리 / 응답 전송)과 CPU 시간(s, 클라이언트 합 / 서버)', '',
              '| 조건 | IPC | 요청 | 큐 | 락 | 처리 | 응답 | CPU 클라이언트 | CPU 서버 |', '| --- | --- | --- | --- | --- | --- | --- | --- | --- |']
    for key in keys_of(data):
        for i in IPCS:
            runs = data.get((i,) + key)
            if not runs:
                continue
            seg = ' | '.join(fmt(med(runs, c), 1) for c, _ in SEGS)
            lines.append(f'| {label(key, vary)} | {i} | {seg} | {fmt(med(runs, "cl_cpu"), 2)} | {fmt(med(runs, "srv_cpu"), 2)} |')
    lines.append('')


def line_plot(data, xs_key, fixed, col, xlabel, ylabel, title, name, logx=False):
    fig, ax = plt.subplots(figsize=(6.4, 4.2))
    for i in IPCS:
        pts = []
        for key in keys_of(data):
            kd = dict(zip(KEY, key))
            if any(kd[k] != v for k, v in fixed.items()):
                continue
            runs = data.get((i,) + key)
            if runs:
                vals = [r[col] for r in runs]
                lo, hi = core(vals)
                pts.append((int(kd[xs_key]), statistics.median(vals), lo, hi))
        if not pts:
            continue
        pts.sort()
        x = [p[0] for p in pts]
        y = [p[1] for p in pts]
        ax.plot(x, y, marker=MARK[i], color=COLOR[i], label=i)
        ax.fill_between(x, [p[2] for p in pts], [p[3] for p in pts], color=COLOR[i], alpha=0.15)
    if logx:
        ax.set_xscale('log', base=2)
    xt = sorted({int(dict(zip(KEY, k))[xs_key]) for k in keys_of(data)
                 if all(dict(zip(KEY, k))[a] == b for a, b in fixed.items())})
    ax.set_xticks(xt)
    ax.set_xticklabels([str(v) for v in xt])
    ax.set_xlabel(xlabel)
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.set_ylim(bottom=0)
    ax.grid(True, alpha=0.3)
    ax.legend()
    fig.tight_layout()
    fig.savefig(os.path.join(FIG, name), dpi=150)
    plt.close(fig)
    return name


def bar_plot(data, cat_key, col, ylabel, title, name):
    keys = keys_of(data)
    cats = [dict(zip(KEY, k))[cat_key] for k in keys]
    fig, ax = plt.subplots(figsize=(6.4, 4.2))
    w = 0.8 / len(IPCS)
    for j, i in enumerate(IPCS):
        ys, lo, hi = [], [], []
        for k in keys:
            vals = [r[col] for r in data.get((i,) + k, [])]
            m = statistics.median(vals) if vals else 0
            a, b = core(vals) if vals else (0, 0)
            ys.append(m)
            lo.append(m - a)
            hi.append(b - m)
        ax.bar([x + j * w for x in range(len(keys))], ys, w, yerr=[lo, hi], color=COLOR[i], label=i, capsize=3)
    ax.set_xticks([x + w for x in range(len(keys))])
    ax.set_xticklabels(cats)
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.grid(True, axis='y', alpha=0.3)
    ax.legend()
    fig.tight_layout()
    fig.savefig(os.path.join(FIG, name), dpi=150)
    plt.close(fig)
    return name


def seg_plot(data, key, title, name):
    fig, ax = plt.subplots(figsize=(6.4, 3.6))
    colors = ['#93c5fd', '#fcd34d', '#fca5a5', '#86efac', '#c4b5fd']
    for j, i in enumerate(IPCS):
        runs = data.get((i,) + key)
        if not runs:
            continue
        left = 0
        for (c, lab), color in zip(SEGS, colors):
            v = med(runs, c)
            ax.barh(j, v, left=left, color=color, label=lab if j == 0 else None)
            left += v
    ax.set_yticks(range(len(IPCS)))
    ax.set_yticklabels(IPCS)
    ax.set_xlabel('µs per request (mean)')
    ax.set_title(title)
    ax.legend(ncol=5, fontsize=8, loc='upper center', bbox_to_anchor=(0.5, -0.22))
    fig.tight_layout()
    fig.savefig(os.path.join(FIG, name), dpi=150)
    plt.close(fig)
    return name


def main():
    allx = load()
    if not allx:
        sys.exit(f'{RES}/app_*.csv 가 없다')
    os.makedirs(FIG, exist_ok=True)
    lines = ['# 2단계 측정 요약', '', f'`scripts/plot-app.py` 가 `{RES}/` 에서 만든 표다. 손으로 고치지 않는다.', '']
    env = os.path.join(RES, 'app_env.txt')
    if os.path.exists(env):
        with open(env) as fh:
            head = [ln.strip() for ln in fh if ln.startswith(('commit:', 'reps:', 'date:'))]
        lines += ['측정: ' + ', '.join(head), '']
    figs = []
    for exp in ['base', 'type', 'draw', 'clients', 'workers', 'lock', 'tile']:
        if exp in allx:
            table(lines, exp, allx[exp])
    d = allx.get('type')
    if d:
        figs.append(line_plot(d, 'snap', {'inflight': '1', 'draw_pct': '0'}, 'p50', 'SNAPSHOT side S (response S² B)',
                              'latency median (µs)', 'SNAPSHOT only, 1 client, latency', 'type_snap_latency.png', True))
        figs.append(line_plot(d, 'snap', {'inflight': '16', 'draw_pct': '0'}, 'thr', 'SNAPSHOT side S (response S² B)',
                              'requests/s', 'SNAPSHOT only, 1 client, -f 16', 'type_snap_throughput.png', True))
        k = ('1', '1', '4', 'global', '64', '0', '1024')
        if any((i,) + k in d for i in IPCS):
            figs.append(seg_plot(d, k, 'SNAPSHOT 1MB, 1 client, latency run: segments', 'type_snap1m_segments.png'))
    d = allx.get('base')
    if d:
        figs.append(seg_plot(d, ('4', '1', '4', 'global', '64', '90', '256'), 'Base mix, 4 clients, -f 1: segments', 'base_segments_lat.png'))
        figs.append(seg_plot(d, ('4', '16', '4', 'global', '64', '90', '256'), 'Base mix, 4 clients, -f 16: segments', 'base_segments_thr.png'))
    d = allx.get('draw')
    if d:
        for c in ('1', '4'):
            figs.append(line_plot(d, 'draw_pct', {'clients': c}, 'thr', 'DRAW %', 'requests/s',
                                  f'Throughput vs DRAW ratio, {c} client(s)', f'draw_c{c}.png'))
    d = allx.get('clients')
    if d:
        figs.append(line_plot(d, 'clients', {}, 'thr', 'clients', 'requests/s', 'Throughput vs clients (base mix)', 'clients.png', True))
    d = allx.get('workers')
    if d:
        figs.append(line_plot(d, 'workers', {}, 'thr', 'server workers', 'requests/s', 'Throughput vs workers (8 clients)', 'workers.png', True))
    d = allx.get('lock')
    if d:
        figs.append(bar_plot(d, 'lock', 'thr', 'requests/s', 'Throughput by lock mode (4 clients)', 'lock.png'))
    d = allx.get('tile')
    if d:
        figs.append(line_plot(d, 'tile', {}, 'thr', 'tile size', 'requests/s', 'Throughput vs tile size (-l tile)', 'tile.png', True))
    lines += ['## 그림', ''] + [f'![{n}]({n})' for n in figs] + ['']
    with open(os.path.join(FIG, 'summary.md'), 'w') as fh:
        fh.write('\n'.join(lines))
    print(f'{FIG}/summary.md, 그림 {len(figs)}개')


main()
