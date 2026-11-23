#!/usr/bin/env python3
"""대규모 버티기 실험(scale)의 요약표와 그림을 만든다. 손으로 고치지 않는다.

    python3 scripts/scale-summary.py results/exp-scale/scale.csv docs/assets/app/exp-scale

실행 하나(같은 experiment, run, clients)마다
  전체 처리량 = 클라이언트들이 잰 요청 수의 합 / 클라이언트 측정 시간 중 가장 긴 것 (모두 SIGUSR2 로 함께 시작)
  지연 p50·p99 = 클라이언트별 값의 중앙값, 서버 최대 RSS = 서버 줄의 VmHWM
을 내고, 같은 설정·클라이언트 수의 실행들의 중앙값을 쓴다. 클라이언트 줄이 N 개보다 적은 실행은
"끝나지 못함"으로 세고 값에 넣지 않는다. 그림(scale.png)은 matplotlib 이 있을 때만 만든다.
"""
import csv
import os
import statistics
import sys
from collections import defaultdict

LABEL = {
    'scale-pipe': 'pipe',
    'scale-mq1': 'mq (큐 1쌍)',
    'scale-mq64': 'mq (큐 64쌍)',
    'scale-shm': 'shm (공유 요청 링)',
    'scale-shmper': 'shm (클라이언트별 요청 링)',
}
ORDER = list(LABEL)


def main(src, out):
    runs = defaultdict(lambda: {'cli': [], 'srv': None})
    with open(src, newline='', encoding='utf-8') as f:
        for r in csv.DictReader(f):
            if not r['experiment'].startswith('scale-'):
                continue
            key = (r['experiment'], int(r['clients']), r['run'])
            if r['role'] == 'client':
                runs[key]['cli'].append(r)
            else:
                runs[key]['srv'] = r
    agg = defaultdict(lambda: defaultdict(list))   # exp -> N -> [dict]
    fails = defaultdict(int)
    for (exp, n_cli, _), v in runs.items():
        c = v['cli']
        if len(c) < n_cli:
            fails[(exp, n_cli)] += 1
            continue
        tot = sum(float(x['n']) for x in c)
        el = max(float(x['elapsed_s']) for x in c)
        agg[exp][n_cli].append({
            'thr': tot / el,
            'p50': statistics.median(float(x['lat_p50_us']) for x in c),
            'p99': statistics.median(float(x['lat_p99_us']) for x in c),
            'hwm': float(v['srv']['vmhwm_kb']) / 1024 if v['srv'] and v['srv']['vmhwm_kb'] else float('nan'),
        })
    os.makedirs(out, exist_ok=True)
    ns = sorted({n for e in agg.values() for n in e} | {n for (_, n) in fails})
    exps = [e for e in ORDER if e in agg or any(k[0] == e for k in fails)]
    md = ['# 대규모 버티기: 클라이언트 수에 따른 처리량 (scale)', '',
          f'`scripts/scale-summary.py` 가 `{src}` 에서 만든 파일이다. 손으로 고치지 않는다.',
          '클라이언트 N 개를 그림판 16개에 나눠 한꺼번에 붙이고(요청 합계 204,800개, `-f 4`, 서버 `-l tile -w 8`),',
          '모두 SIGUSR2 로 함께 시작해 잰 값이다. 값은 같은 조건 실행들의 중앙값이고 괄호는 실행 수다.', '',
          '그림: [클라이언트 수별 처리량](scale.png)', '']
    for title, k, fmt in (('전체 처리량 (요청/초)', 'thr', '{:,.0f}'), ('지연 p50 (µs, 클라이언트별 값의 중앙값)', 'p50', '{:,.1f}'),
                          ('지연 p99 (µs)', 'p99', '{:,.1f}'), ('서버 최대 RSS (MB)', 'hwm', '{:,.0f}')):
        md += [f'## {title}', '', '| 설정 | ' + ' | '.join(f'N={n}' for n in ns) + ' |',
               '| --- |' + ' ---: |' * len(ns)]
        for e in exps:
            cells = []
            for n in ns:
                v = [x[k] for x in agg[e].get(n, [])]
                f = fails.get((e, n), 0)
                if v:
                    cells.append(fmt.format(statistics.median(v)) + f' ({len(v)})' + (f', 끝나지 못함 {f}' if f else ''))
                elif f:
                    cells.append(f'끝나지 못함 ({f})')
                else:
                    cells.append('')
            md.append(f'| {LABEL.get(e, e)} | ' + ' | '.join(cells) + ' |')
        md.append('')
    with open(os.path.join(out, 'summary.md'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(md) + '\n')
    print('\n'.join(md))
    try:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
        from matplotlib import font_manager
        for name in ('NanumGothic', 'Noto Sans CJK KR', 'Noto Sans KR', 'D2Coding'):
            if any(name in fe.name for fe in font_manager.fontManager.ttflist):
                plt.rcParams['font.family'] = name
                break
    except ImportError:
        return
    fig, ax = plt.subplots(figsize=(7.5, 4.5), dpi=150)
    marks = {'scale-pipe': 'o', 'scale-mq1': 'x', 'scale-mq64': 's', 'scale-shm': '^', 'scale-shmper': 'D'}
    for e in exps:
        xs = [n for n in ns if agg[e].get(n)]
        ys = [statistics.median(x['thr'] for x in agg[e][n]) for n in xs]
        if xs:
            ax.plot(xs, ys, marker=marks.get(e, 'o'), label=LABEL.get(e, e))
        for n in ns:
            if fails.get((e, n)) and not agg[e].get(n):
                ax.annotate('끝나지 못함', (n, 0), textcoords='offset points', xytext=(0, 8), ha='center', fontsize=8, color='#b00')
    ax.set_xscale('log', base=2)
    ax.set_xticks(ns)
    ax.set_xticklabels([str(n) for n in ns])
    ax.set_xlabel('클라이언트 수 (그림판 16개)')
    ax.set_ylabel('전체 처리량 (요청/초)')
    ax.set_ylim(bottom=0)
    ax.grid(alpha=0.3)
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(os.path.join(out, 'scale.png'))


if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    main(sys.argv[1], sys.argv[2])
