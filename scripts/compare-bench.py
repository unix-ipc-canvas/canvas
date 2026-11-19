#!/usr/bin/env python3
"""1단계 실험을 기준과 비교하는 표를 만든다. 실험에서 잰 조건만 기준과 나란히 놓는다.

    python3 scripts/compare-bench.py 출력.md 기준폴더:실험폴더 [기준폴더:실험폴더 ...]
    예: python3 scripts/compare-bench.py docs/assets/bench/experiments.md results/stage1-r20:results/exp-pcore

값은 모두 실행별 값의 중앙값이다(client 줄, 처리량은 처리량 실행).
가운데 범위는 실행별 지연 p50 에서 좋은 쪽과 나쁜 쪽을 4분의 1씩 뺀 범위이고, 폭은 그 범위 / 중앙값이다.
matplotlib 없이 돈다. 손으로 고치지 않는다.
"""
import csv
import os
import statistics
import sys
from collections import defaultdict


def load(d, name, role):
    out = defaultdict(list)
    path = os.path.join(d, name)
    if not os.path.exists(path):
        return out
    with open(path, newline='', encoding='utf-8') as f:
        for r in csv.DictReader(f):
            if r['role'] == role and r['stage'] == 'bench':
                out[(r['ipc'], int(r['msg_size']), int(r['clients']))].append(r)
    return out


def med(rows, col):
    return statistics.median(float(r[col]) for r in rows)


def core(vals):
    v = sorted(vals)
    k = len(v) // 4
    if k:
        v = v[k:-k]
    return v[0], v[-1]


def env_line(d, title):
    """bench_env.txt 에서 절 제목 다음 줄"""
    path = os.path.join(d, 'bench_env.txt')
    if not os.path.exists(path):
        return '-'
    lines = open(path, encoding='utf-8').read().split('\n')
    for i, l in enumerate(lines):
        if l.startswith('## ' + title):
            return lines[i + 1].strip() if i + 1 < len(lines) else '-'
    return '-'


def env_value(d, key):
    path = os.path.join(d, 'bench_env.txt')
    for l in open(path, encoding='utf-8') if os.path.exists(path) else []:
        if l.startswith(key + ':'):
            return l.split(':', 1)[1].strip()
    return '-'


def size_label(s):
    return f'{s // 1048576}MB' if s >= 1048576 else f'{s // 1024}KB' if s >= 1024 else f'{s}B'


def section(base, exp):
    bl, bs, bt = (load(base, 'bench_latency.csv', 'client'), load(base, 'bench_latency.csv', 'server'),
                  load(base, 'bench_throughput.csv', 'client'))
    el, es, et = (load(exp, 'bench_latency.csv', 'client'), load(exp, 'bench_latency.csv', 'server'),
                  load(exp, 'bench_throughput.csv', 'client'))
    out = [f'## {os.path.basename(exp)} (기준: {os.path.basename(base)})', '']
    out.append('| | 기준 | 실험 |')
    out.append('| --- | --- | --- |')
    for title in ('CPU 배치',):
        out.append(f'| {title} | {env_line(base, title)} | {env_line(exp, title)} |')
    for key in ('commit', 'msgmax', 'msgmnb'):
        out.append(f'| {key} | {env_value(base, key)} | {env_value(exp, key)} |')
    runs = sorted({len(v) for v in el.values()})
    out.append(f'| 조건마다 실행 수 | {sorted({len(v) for v in bl.values()})} | {runs} |')
    out += ['', '| IPC | 크기 | 클라이언트 | 지연 p50 (µs) 기준 → 실험 | 배율 | 가운데 범위 폭 기준 → 실험 | 메시지당 자발적 컨텍스트 스위치 기준 → 실험 | 처리량 (MB/s) 기준 → 실험 |',
            '| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |']
    for key in sorted(el, key=lambda k: (k[1], k[2], k[0])):
        if key not in bl:
            continue
        b, e = med(bl[key], 'lat_p50_us'), med(el[key], 'lat_p50_us')
        bw = (lambda c: (c[1] - c[0]) / b * 100)(core([float(r['lat_p50_us']) for r in bl[key]]))
        ew = (lambda c: (c[1] - c[0]) / e * 100)(core([float(r['lat_p50_us']) for r in el[key]]))

        def cs(lat, srv):
            if key not in srv:
                return None
            return (med(lat[key], 'nvcsw') + med(srv[key], 'nvcsw')) / med(lat[key], 'n')
        bc, ec = cs(bl, bs), cs(el, es)
        cs_txt = f'{bc:.1f} → {ec:.1f}' if bc is not None and ec is not None else '-'
        thr_txt = (f'{med(bt[key], "thr_mb_s"):.1f} → {med(et[key], "thr_mb_s"):.1f}'
                   if key in bt and key in et else '-')
        out.append(f'| {key[0]} | {size_label(key[1])} | {key[2]} | {b:.2f} → {e:.2f} | {e / b:.2f} | '
                   f'{bw:.0f}% → {ew:.0f}% | {cs_txt} | {thr_txt} |')
    return out


def main():
    if len(sys.argv) < 3 or any(':' not in a for a in sys.argv[2:]):
        sys.exit(__doc__)
    lines = ['# 1단계 실험 비교', '',
             '`scripts/compare-bench.py`가 원자료에서 만든 표다. 손으로 고치지 않는다. '
             '실험 설계와 해석은 [1단계 측정 결과](../../measurement/stage1.md)에 있다.', '']
    for pair in sys.argv[2:]:
        base, exp = pair.split(':', 1)
        lines += section(base, exp) + ['']
    os.makedirs(os.path.dirname(os.path.abspath(sys.argv[1])), exist_ok=True)
    with open(sys.argv[1], 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines).rstrip('\n') + '\n')
    print(f'{sys.argv[1]} 를 만들었다')


if __name__ == '__main__':
    main()
