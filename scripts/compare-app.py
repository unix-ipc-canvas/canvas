#!/usr/bin/env python3
"""2단계 설계 비교 실험(exp-stage2.sh 의 pipesz, shmmem)의 요약표. 원자료에서만 만든다.

    python3 scripts/compare-app.py results/exp-app-pipesz docs/assets/app/exp-app-pipesz 64k
    python3 scripts/compare-app.py results/exp-app-shmmem docs/assets/app/exp-app-shmmem def
    python3 scripts/compare-app.py results/exp-pipesz-bench docs/assets/app/exp-pipesz-bench buf65536

결과 폴더 아래의 <설계>-<배치>-<번> 폴더(예: 1m-pcore-3)를 읽는다. 셋째 인자는 기준으로 삼는 설계 이름이다.
한 실행은 plot-app.py 와 같이 모은다: 처리량은 요청 수 합 / 가장 긴 elapsed_s, 지연은 클라이언트별 중앙값의 중앙값,
메모리는 서버 줄과 클라이언트 줄의 VmHWM(최대 RSS). 같은 설계·배치·조건의 모든 실행(번갈아 잰 것을 모두 합친 것)의 중앙값을 쓴다.
기준과 비교할 때는 1단계와 같은 규칙으로, 실행별 값에서 좋은 쪽과 나쁜 쪽을 4분의 1씩 뺀 범위가 겹치면 ≈ 로 둔다.
1단계 echo 벤치 원자료(폴더에 bench_*.csv 가 있을 때)는 실험 이름 <설계>-<배치> 와 반복 번호로 실행을 나눈다.
요약표는 둘째 인자 폴더의 summary.md 에, 기준 대비 그래프는 compare.png 에 쓴다. 손으로 고치지 않는다.
그래프는 성능을 '기준 대비 배수'로 그린다(지연은 기준/설계, 처리량은 설계/기준이라 1보다 크면 좋아진 것).
막대 끝의 선은 실행별 값의 가운데 범위를 같은 방식으로 나눈 것이다. matplotlib 이 필요하다.
"""
import csv
import re
import glob
import os
import statistics
import sys
from collections import defaultdict

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

RES, FIG, BASE = sys.argv[1], sys.argv[2], sys.argv[3]
KEY = ('clients', 'inflight', 'workers', 'lock', 'tile', 'draw_pct', 'snap')
PLACE = {'free': '묶지 않음', 'pcore': 'P코어(CPU 0~3)에 묶음'}


def f(v):
    return float(v) if v not in ('', None) else None


def load():
    """(설계, 배치, 실험, ipc, 조건) -> [실행별 {'thr','p50','srv_hwm','cl_hwm'}], 폴더 목록"""
    data = defaultdict(list)
    dirs = sorted(d for d in glob.glob(os.path.join(RES, '*-*-*')) if os.path.isdir(d))
    for d in dirs:
        design, place, _ = os.path.basename(d).rsplit('-', 2)
        for path in sorted(glob.glob(os.path.join(d, 'app_*.csv'))):
            cs = []
            with open(path, newline='') as fh:
                for r in csv.DictReader(fh):
                    if r['stage'] != 'app':
                        continue
                    if r['role'] == 'client':
                        cs.append(r)
                        continue
                    keys = {tuple(c[k] for k in KEY) for c in cs}
                    if not (len(keys) == 1 and len(cs) == int(cs[0]['clients'])
                            and all(c['ipc'] == r['ipc'] and c['run'] == r['run'] for c in cs)):
                        print(f'{path}: 짝이 맞지 않는 실행을 건너뛴다 (run={r["run"]})', file=sys.stderr)
                        cs = []
                        continue
                    n = sum(int(c['n']) for c in cs)
                    el = max(f(c['elapsed_s']) for c in cs)
                    s = {'thr': n / el, 'p50': statistics.median(f(c['lat_p50_us']) for c in cs),
                         'srv_hwm': f(r['vmhwm_kb']), 'cl_hwm': statistics.median(f(c['vmhwm_kb']) for c in cs)}
                    data[(design, place, r['experiment'], r['ipc']) + keys.pop()].append(s)
                    cs = []
    return data, dirs


def load_bench():
    """1단계 echo 벤치 원자료. (설계, 배치, 'echo', ipc, 크기, 동시 요청) -> [실행별 요약]"""
    data = defaultdict(list)
    files = sorted(glob.glob(os.path.join(RES, 'bench_*.csv')))
    for path in files:
        with open(path, newline='') as fh:
            rows = [r for r in csv.DictReader(fh) if r['stage'] == 'bench']
        for c, sv in zip(rows[::2], rows[1::2]):
            if c['role'] != 'client' or sv['role'] != 'server' or c['run'] != sv['run'] or c['experiment'] != sv['experiment']:
                print(f'{path}: 짝이 맞지 않는 줄을 건너뛴다 (run={c["run"]})', file=sys.stderr)
                continue
            design, place = c['experiment'].rsplit('-', 1)
            n = int(c['n'])
            s = {'thr': f(c['thr_mb_s']), 'p50': f(c['lat_p50_us']), 'srv_hwm': f(sv['vmhwm_kb']), 'cl_hwm': f(c['vmhwm_kb']),
                 'csw': (int(c['nvcsw']) + int(sv['nvcsw'])) / n,
                 'cpu': (f(c['utime_s']) + f(c['stime_s']) + f(sv['utime_s']) + f(sv['stime_s'])) / n * 1e6}
            data[(design, place, 'echo', c['ipc'], c['msg_size'], c['inflight'])].append(s)
    return data, files


def core(v):
    v = sorted(v)
    k = len(v) // 4
    return (v[k], v[-1 - k]) if k else (v[0], v[-1])


def compare(a, b, low_better):
    """기준 a 와 비교한 b 의 판정: ≈ 또는 좋음·나쁨"""
    (alo, ahi), (blo, bhi) = core(a), core(b)
    if blo > ahi:
        return '나쁨' if low_better else '좋음'
    if bhi < alo:
        return '좋음' if low_better else '나쁨'
    return '≈'


def cond(key):
    if len(key) == 2:
        return f'메시지 {int(key[0]) // 1024}KB, -f {key[1]}'
    c = dict(zip(KEY, key))
    s = f"클라이언트 {c['clients']}, -f {c['inflight']}"
    if c['draw_pct'] != '':
        s += f", DRAW {c['draw_pct']}%"
    if c['snap'] != '':
        s += f", -S {c['snap']}"
    return s


def dkey(x):
    """설계 이름의 순서: 기준이 먼저, 크기(64k, 256k, 1m, buf65536)는 작은 것부터, 나머지는 이름순"""
    m = re.fullmatch(r'(?:buf)?(\d+)([km]?)', x)
    size = int(m.group(1)) * {'': 1, 'k': 1024, 'm': 1 << 20}[m.group(2)] if m else 0
    return (x != BASE, size, x)


def fmt(v, d=0):
    return '-' if v is None else f'{v:,.{d}f}'


COLORS = ['#2563eb', '#d97706', '#059669']
PLACE_EN = {'free': 'not pinned', 'pcore': 'pinned to P-cores (CPU 0-3)'}


def short(key, bench):
    """그래프의 조건 이름 (영어, 그림 글꼴에 한글이 없을 수 있어서)"""
    if bench:
        return f'echo {int(key[0]) // 1024}KB -f{key[1]}'
    c = dict(zip(KEY, key))
    if c['draw_pct'] == '100':
        what = 'DRAW only'
    elif c['draw_pct'] == '0':
        what = f'SNAPSHOT {int(c["snap"]) ** 2 // 1024}KB'
    else:
        what = f'mix c{c["clients"]}'
    return f'{what} -f{c["inflight"]}'


def plot(data, designs, bench):
    others = [x for x in designs if x != BASE]
    places = [pl for pl in ('free', 'pcore') if any(k[1] == pl for k in data)]
    fig, axes = plt.subplots(1, len(places), figsize=(6.2 * len(places), 5.2), sharex=True, squeeze=False)
    for ax, place in zip(axes[0], places):
        keys = sorted({k[2:] for k in data if k[1] == place},
                      key=lambda k: (k[0], k[1]) + tuple(int(x) if x.isdigit() else -1 for x in k[2:]))
        labels, rows = [], []
        for k in keys:
            ckey = k[2:]
            lat = (ckey[1] if bench else dict(zip(KEY, ckey))['inflight']) == '1'
            col = 'p50' if lat else 'thr'
            base = [r[col] for r in data.get((BASE, place) + k, [])]
            if not base:
                continue
            bm = statistics.median(base)
            vals = []
            for o in others:
                v = [r[col] for r in data.get((o, place) + k, [])]
                if not v:
                    vals.append(None)
                    continue
                lo, hi = core(v)
                m = statistics.median(v)
                vals.append((bm / m, bm / hi, bm / lo) if lat else (m / bm, lo / bm, hi / bm))
            labels.append(short(ckey, bench) + (' (latency)' if lat else ' (throughput)'))
            rows.append(vals)
        n = len(others)
        h = 0.8 / n
        for j, o in enumerate(others):
            ys = [i + (j - (n - 1) / 2) * h for i in range(len(rows))]
            xs = [r[j][0] if r[j] else 0 for r in rows]
            err = [[r[j][0] - r[j][1] if r[j] else 0 for r in rows], [r[j][2] - r[j][0] if r[j] else 0 for r in rows]]
            ax.barh(ys, [x - 1 for x in xs], left=1, height=h * 0.9, color=COLORS[j % len(COLORS)], label=o,
                    xerr=err, error_kw={'elinewidth': 1, 'ecolor': '#555555', 'capsize': 0})
        ax.axvline(1, color='#333333', linewidth=1)
        ax.set_yticks(range(len(labels)))
        ax.set_yticklabels(labels, fontsize=8)
        ax.invert_yaxis()
        ax.set_xlabel(f'performance vs {BASE} (x, >1 is better)')
        ax.set_title(PLACE_EN.get(place, place), fontsize=10)
        ax.grid(axis='x', color='#dddddd', linewidth=0.6)
        ax.set_axisbelow(True)
        for side in ('top', 'right'):
            ax.spines[side].set_visible(False)
    axes[0][-1].legend(title='design', fontsize=8, title_fontsize=8, loc='lower right')
    fig.suptitle(f'{os.path.basename(RES)}: median of runs, whiskers = middle half of runs', fontsize=11)
    fig.tight_layout()
    fig.savefig(os.path.join(FIG, 'compare.png'), dpi=130)
    plt.close(fig)


def plot_memory(data, designs):
    """설계마다 최대 RSS 가 다르면(5% 넘게) 서버·클라이언트 VmHWM 막대 그래프를 memory.png 로 그린다 (묶지 않은 것)"""
    keys = sorted({k[2:] for k in data if k[1] == 'free'},
                  key=lambda k: (k[0], k[1]) + tuple(int(x) if x.isdigit() else -1 for x in k[2:]))
    med = {(d, k, c): statistics.median(r[c] for r in data[(d, 'free') + k])
           for d in designs for k in keys for c in ('srv_hwm', 'cl_hwm') if data.get((d, 'free') + k)}
    if all(abs(med.get((d, k, 'srv_hwm'), 1) / med[(BASE, k, 'srv_hwm')] - 1) < 0.05
           for d in designs for k in keys if (BASE, k, 'srv_hwm') in med):
        return False
    fig, axes = plt.subplots(1, 2, figsize=(12.4, 5.2), sharey=True)
    n = len(designs)
    h = 0.8 / n
    for ax, col, title in zip(axes, ('srv_hwm', 'cl_hwm'), ('server', 'client (median of clients)')):
        for j, d in enumerate(designs):
            ys = [i + (j - (n - 1) / 2) * h for i in range(len(keys))]
            ax.barh(ys, [med.get((d, k, col), 0) / 1024 for k in keys], height=h * 0.9, color=COLORS[j % len(COLORS)], label=d)
        ax.set_yticks(range(len(keys)))
        ax.set_yticklabels([short(k[2:], False) for k in keys], fontsize=8)
        ax.set_xlabel('VmHWM (MiB)')
        ax.set_title(title, fontsize=10)
        ax.grid(axis='x', color='#dddddd', linewidth=0.6)
        ax.set_axisbelow(True)
        for side in ('top', 'right'):
            ax.spines[side].set_visible(False)
    axes[0].invert_yaxis()          # y 축을 함께 쓰므로 한 번만 뒤집는다
    axes[0].legend(title='design', fontsize=8, title_fontsize=8, loc='lower right')
    fig.suptitle(f'{os.path.basename(RES)}: peak RSS, median of runs (not pinned)', fontsize=11)
    fig.tight_layout()
    fig.savefig(os.path.join(FIG, 'memory.png'), dpi=130)
    plt.close(fig)
    return True


def main():
    data, dirs = load()
    bench = not data
    if bench:
        data, dirs = load_bench()
    designs = sorted({k[0] for k in data}, key=dkey)
    others = [x for x in designs if x != BASE]
    what = ('1단계 echo 벤치(클라이언트 1). 처리량은 MB/s, 문맥 교환과 CPU 시간은 클라이언트와 서버를 더해 메시지 수로 나눈 것이다.'
            if bench else '')
    lines = [f'# 설계 비교: {os.path.basename(RES)}', '',
             f'`scripts/compare-app.py` 가 `{RES}/` 에서 만든 파일이다. 손으로 고치지 않는다.',
             f'{"파일" if bench else "폴더"} {len(dirs)}개({", ".join(os.path.basename(d) for d in dirs[:3])}{" ..." if len(dirs) > 3 else ""})를 읽었다. 기준 설계는 `{BASE}` 이다. {what}',
             '처리량(응용은 msg/s)은 동시 요청이 1보다 큰 조건, 지연(p50, µs)은 동시 요청 1인 조건의 값이다. '
             + ('' if bench else '메모리는 VmHWM(최대 RSS, KB)이고 클라이언트는 클라이언트별 값의 중앙값이다. ')
             + '값은 같은 조건의 모든 실행의 중앙값이고 괄호는 실행 수다.',
             '판정은 기준과 비교한 것이며, 실행별 값의 가운데 범위(좋은 쪽과 나쁜 쪽 4분의 1씩 뺀 것)가 겹치면 ≈ 다.', '']
    for place in ('free', 'pcore'):
        keys = sorted({k[2:] for k in data if k[1] == place},
                      key=lambda k: (k[0], k[1]) + tuple(int(x) if x.isdigit() else -1 for x in k[2:]))
        if not keys:
            continue
        lines += [f'## {PLACE.get(place, place)}', '']
        head = '| 실험 | IPC | 조건 | 지표 | ' + ' | '.join([BASE] + others) + ' | ' + \
               ' | '.join(f'{o}/{BASE}' for o in others) + ' | ' + ' | '.join(f'{o} 판정' for o in others) + ' |'
        lines += [head, '|' + ' --- |' * (4 + len(designs) + 2 * len(others))]
        for k in keys:
            exp, ipc, ckey = k[0], k[1], k[2:]
            runs = {d: data.get((d, place) + k, []) for d in designs}
            if not runs[BASE]:
                continue
            lat = (ckey[1] if bench else dict(zip(KEY, ckey))['inflight']) == '1'
            mets = [('p50', '지연 p50', True, 2) if lat else ('thr', '처리량 MB/s' if bench else '처리량', False, 0)]
            if bench:
                mets += [('csw', '자발적 문맥 교환/메시지', True, 1), ('cpu', 'CPU µs/메시지', True, 0)]
            else:
                mets += [('srv_hwm', '서버 VmHWM', True, 0), ('cl_hwm', '클라이언트 VmHWM', True, 0)]
            for col, name, low, dg in mets:
                vals = {d: [r[col] for r in runs[d] if r[col] is not None] for d in designs}
                med = {d: statistics.median(v) if v else None for d, v in vals.items()}
                row = [exp, ipc, cond(ckey), name] + [f'{fmt(med[d], dg)} ({len(vals[d])})' for d in designs]
                row += [f'{med[o] / med[BASE]:.2f}' if med[o] and med[BASE] else '-' for o in others]
                row += [compare(vals[BASE], vals[o], low) if vals[o] and vals[BASE] else '-' for o in others]
                lines.append('| ' + ' | '.join(row) + ' |')
        lines.append('')
    os.makedirs(FIG, exist_ok=True)
    plot(data, designs, bench)
    mem = not bench and plot_memory(data, designs)
    lines.insert(6, '그래프: [기준 대비 성능](compare.png) (지연은 기준/설계, 처리량은 설계/기준. 1보다 크면 좋아진 것)'
                 + (', [최대 RSS](memory.png)' if mem else '') + '\n')
    with open(os.path.join(FIG, 'summary.md'), 'w') as fh:
        fh.write('\n'.join(lines))
    print(os.path.join(FIG, 'summary.md'))


main()
