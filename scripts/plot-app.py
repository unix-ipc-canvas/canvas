#!/usr/bin/env python3
"""2단계 측정 요약표와 그래프. results/app/*.csv 에서 docs/assets/app/ 에 만든다.

  python3 scripts/plot-app.py [원자료폴더 [출력폴더]]

실행 하나 = role=clients 줄 하나 + role=server 줄 하나. 같은 조건을 여러 번 잰 값은 실행마다의 값의 중앙값이다.
순위는 1단계와 같은 규칙: 실행별 값에서 좋은 쪽과 나쁜 쪽을 4분의 1씩 뺀 가운데 범위가 겹치면 ≈ 로 묶는다.
체크섬이 검증기와 다르거나(checksum_ok != 1) 실패한(errors > 0) 실행은 요약에서 빼고 그 수를 적는다.
"""
import csv
import glob
import os
import statistics
import sys

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib import font_manager

SRC = sys.argv[1] if len(sys.argv) > 1 else 'results/app'
DST = sys.argv[2] if len(sys.argv) > 2 else 'docs/assets/app'
os.makedirs(DST, exist_ok=True)
IPCS = ('pipe', 'mq', 'shm')
COLOR = {'pipe': '#d1495b', 'mq': '#edae49', 'shm': '#00798c'}
MARK = {'pipe': 'o', 'mq': 's', 'shm': '^'}

for f in ('NanumGothic', 'Noto Sans CJK KR', 'Noto Sans CJK JP', 'Noto Sans KR', 'UnDotum'):
    if any(f in x.name for x in font_manager.fontManager.ttflist):
        plt.rcParams['font.family'] = f
        break
plt.rcParams['axes.unicode_minus'] = False
plt.rcParams['figure.dpi'] = 110

KEYS = ('clients', 'rooms', 'inflight', 'rate_hz', 'workers', 'lock', 'tile', 'draw_pct', 'rect', 'snap', 'env')
TEXT = ('ipc', 'role', 'experiment', 'commit', 'lock', 'env')
# 개선한 설계 (app-bench.sh 의 IMP_ENV). 각 서버는 자기 IPC 의 변수만 읽는다
IMP_ENV = 'OSP_MQ_RESP_Q=8;OSP_MQ_ADMIT=1;OSP_SHM_REQ=per'
TARGET_HZ = 67   # 사람 한 명의 요청 속도 (워크로드 명세)
FRAME_US = 16700  # 요구사항 N-2: DRAW 상위 99% 가 한 프레임(16.7ms) 이하


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def load():
    """{실험: {조건키: {ipc: [run 딕셔너리...]}}}. run 딕셔너리는 clients 줄 + 'srv_' 가 붙은 server 줄"""
    data, dropped = {}, {}
    for path in sorted(glob.glob(os.path.join(SRC, '*.csv'))):
        with open(path, newline='', encoding='utf-8') as fp:
            rows = list(csv.DictReader(fp))
        srv = {}
        for r in rows:
            if r['role'] == 'server':
                srv[(r['experiment'], r['ipc'], r['run'], tuple(r[k] for k in KEYS))] = r   # env 까지 같아야 같은 실행 (조건이 env 만 다른 실험이 있다)
        for r in rows:
            if r['role'] != 'clients':
                continue
            key = tuple(r[k] for k in KEYS)
            s = srv.get((r['experiment'], r['ipc'], r['run'], key))
            run = {k: num(v) if k not in TEXT else v for k, v in r.items()}
            if s:
                for k, v in s.items():
                    run['srv_' + k] = num(v) if k not in TEXT else v
            good = s is not None and s.get('checksum_ok') == '1' and (run.get('errors') or 0) == 0
            d = data if good else dropped
            d.setdefault(r['experiment'], {}).setdefault(key, {}).setdefault(r['ipc'], []).append(run)
    return data, dropped


def med(runs, f):
    v = [f(r) for r in runs]
    v = [x for x in v if x is not None]
    return statistics.median(v) if v else None


def mid_range(runs, f):
    v = sorted(x for x in (f(r) for r in runs) if x is not None)
    if not v:
        return None
    q = len(v) // 4
    v = v[q:len(v) - q] or v
    return v[0], v[-1]


def rank(by_ipc, f, higher_better):
    """'shm > mq ≈ pipe' 식의 순위 문자열"""
    vals = {i: med(by_ipc[i], f) for i in IPCS if i in by_ipc}
    vals = {i: v for i, v in vals.items() if v is not None}
    if not vals:
        return '-'
    rng = {i: mid_range(by_ipc[i], f) for i in vals}
    order = sorted(vals, key=lambda i: vals[i], reverse=higher_better)
    out, prev = order[0], order[0]
    for i in order[1:]:
        a, b = rng[prev], rng[i]
        overlap = a and b and not (a[1] < b[0] or b[1] < a[0])
        out += (' ≈ ' if overlap else (' > ' if higher_better else ' < ')) + i
        prev = i
    return out


def spread(runs, f):
    v = [x for x in (f(r) for r in runs) if x is not None]
    if len(v) < 2 or statistics.median(v) == 0:
        return None
    return (max(v) - min(v)) / statistics.median(v) * 100


def fmt(v, d=1):
    if v is None:
        return '-'
    if abs(v) >= 1000:
        return f'{v:,.0f}'
    return f'{v:.{d}f}'


def ctx(r):
    """요청 하나당 자발적 컨텍스트 스위치 (모든 클라이언트 + 서버)"""
    n = r.get('n') or 0
    return ((r.get('nvcsw') or 0) + (r.get('srv_nvcsw') or 0)) / n if n else None


def srv_ctx(r):
    """요청 하나당 서버의 자발적 컨텍스트 스위치"""
    n = r.get('n') or 0
    return (r.get('srv_nvcsw') or 0) / n if n else None


def cpu_us(r):
    n = r.get('n') or 0
    t = sum(r.get(k) or 0 for k in ('utime_s', 'stime_s', 'srv_utime_s', 'srv_stime_s'))
    return t / n * 1e6 if n else None


SEGS = ('req_us', 'queue_us', 'lock_us', 'work_us', 'resp_us')
SEG_NAMES = ('요청 전송', '큐 대기', '락 대기', '처리', '응답 전송')
SEG_COLS = ('#8d99ae', '#ef8354', '#2b2d42', '#4f5d75', '#bfc0c0')


def env_label(env):
    """서버 환경 변수를 사람이 읽는 이름으로"""
    if not env:
        return ''
    if env == IMP_ENV:
        return '개선한 설계'
    kv = dict(x.split('=', 1) for x in env.split(';') if '=' in x)
    s = []
    if 'OSP_MQ_RESP_Q' in kv:
        s.append(f"응답 큐 {kv['OSP_MQ_RESP_Q']}개")
    if 'OSP_MQ_REQ_QBYTES' in kv:
        s.append(f"요청 큐 {int(kv['OSP_MQ_REQ_QBYTES']):,}B")
    if kv.get('OSP_MQ_ADMIT') == '1':
        s.append('입장 제한')
    if kv.get('OSP_SHM_REQ') == 'per':
        s.append('클라이언트별 요청 링')
    if 'OSP_SHM_RESP_CHUNKS' in kv:
        s.append(f"응답 링 {kv['OSP_SHM_RESP_CHUNKS']}칸")
    return ' + '.join(s) or env


def cond_label(key):
    d = dict(zip(KEYS, key))
    s = []
    if d['draw_pct'] == '100':
        s.append(f"DRAW만 {d['rect']}²")
    elif d['draw_pct'] == '0':
        s.append(f"SNAPSHOT만 {d['snap']}²")
    else:
        s.append(f"혼합 DRAW {d['draw_pct']}%")
    s.append(f"클라이언트 {d['clients']}")
    s.append(f"동시 {d['inflight']}")
    if d['rate_hz'] not in ('', '0.0', '0'):
        s.append(f"{float(d['rate_hz']):.0f}/s")
    s.append(f"그림판 {d['rooms']}")
    if d['lock'] != 'rw':
        s.append(f"락 {d['lock']}")
    if d['workers'] != '4':
        s.append(f"워커 {d['workers']}")
    if d['env']:
        s.append(env_label(d['env']))
    return ', '.join(s)


def items(exp, **want):
    """실험 exp 의 (조건 딕셔너리, {ipc: runs}) 중 want 에 맞는 것"""
    out = []
    for key, by in data.get(exp, {}).items():
        d = dict(zip(KEYS, key))
        if all(d[k] == v for k, v in want.items()):
            out.append((d, by))
    return out


def sort_key(kv):
    key = kv[0]
    d = dict(zip(KEYS, key))
    return (d['env'] != '', d['env'], num(d['clients']) or 0, num(d['inflight']) or 0, num(d['rooms']) or 0,
            d['lock'], d['draw_pct'], num(d['snap']) or 0)


data, dropped = load()
out = []
w = out.append
w('# 2단계 측정 요약\n')
w(f'`scripts/plot-app.py` 가 `{SRC}/*.csv` 에서 만든 파일이다. 손으로 고치지 않는다. 같은 조건을 여러 번 잰 값은 실행마다의 값의 중앙값이다. '
  '지연 단위는 µs, 처리량은 서버 전체의 초당 요청 수(req/s)와 응답 MB/s(1MB = 10^6B). '
  '순위는 실행별 값에서 좋은 쪽과 나쁜 쪽을 4분의 1씩 뺀 가운데 범위가 겹치면 ≈ 로 묶는다. '
  '문맥 교환은 자발적 컨텍스트 스위치(모든 클라이언트 + 서버)를 받은 응답 수로 나눈 값이다.\n')
env_path = os.path.join(SRC, 'app_env.txt')
if os.path.exists(env_path):
    with open(env_path, encoding='utf-8') as f:
        head = f.read().split('\n')[:9]
    w('측정 환경 (`app_env.txt` 의 처음)\n\n```text\n' + '\n'.join(head) + '\n```\n')
n_drop = sum(len(v) for e in dropped.values() for c in e.values() for v in c.values())
n_ok = sum(len(v) for e in data.values() for c in e.values() for v in c.values())
w(f'요약에 쓴 실행 {n_ok}번, 뺀 실행(체크섬 다름·오류) {n_drop}번.\n')
if n_drop:
    w('| 실험 | 조건 | IPC | 뺀 실행 |')
    w('| --- | --- | --- | ---: |')
    for e, conds in sorted(dropped.items()):
        for key, by in conds.items():
            for i, rs in by.items():
                w(f'| {e} | {cond_label(key)} | {i} | {len(rs)} |')
    w('')


def table_basic(exp, title):
    if exp not in data:
        return
    w(f'\n## {title}\n')
    w('| 조건 | IPC | 실행 | 처리량 req/s | MB/s | 전체 p50 | 전체 p99 | DRAW p50 | DRAW p99 | SNAPSHOT p50 | SNAPSHOT p99 | 문맥 교환 /요청 | 서버 문맥 교환 /요청 | CPU µs /요청 | 실행 사이 편차(처리량) |')
    w('| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |')
    for key, by in sorted(data[exp].items(), key=sort_key):
        for i in IPCS:
            if i not in by:
                continue
            rs = by[i]
            w(f"| {cond_label(key)} | {i} | {len(rs)} | {fmt(med(rs, lambda r: r['thr_req_s']), 0)} | {fmt(med(rs, lambda r: r['thr_mb_s']))} | "
              f"{fmt(med(rs, lambda r: r['lat_p50_us']))} | {fmt(med(rs, lambda r: r['lat_p99_us']))} | "
              f"{fmt(med(rs, lambda r: r['draw_p50_us'] if r.get('draw_n') else None))} | {fmt(med(rs, lambda r: r['draw_p99_us'] if r.get('draw_n') else None))} | "
              f"{fmt(med(rs, lambda r: r['snap_p50_us'] if r.get('snap_n') else None))} | {fmt(med(rs, lambda r: r['snap_p99_us'] if r.get('snap_n') else None))} | "
              f"{fmt(med(rs, ctx), 2)} | {fmt(med(rs, srv_ctx), 2)} | {fmt(med(rs, cpu_us), 2)} | {fmt(spread(rs, lambda r: r['thr_req_s']), 0)}% |")
        if len(by) > 1:
            d = dict(zip(KEYS, key))
            snap = d['draw_pct'] != '100'
            draw = d['draw_pct'] != '0'
            w(f"| {cond_label(key)} | 순위 | | {rank(by, lambda r: r['thr_req_s'], True)} | | {rank(by, lambda r: r['lat_p50_us'], False)} | "
              f"{rank(by, lambda r: r['lat_p99_us'], False)} | "
              f"{rank(by, lambda r: r['draw_p50_us'], False) if draw else ''} | {rank(by, lambda r: r['draw_p99_us'], False) if draw else ''} | "
              f"{rank(by, lambda r: r['snap_p50_us'], False) if snap else ''} | {rank(by, lambda r: r['snap_p99_us'], False) if snap else ''} | | | | |")


def table_segments(exp, title):
    if exp not in data:
        return
    w(f'\n## {title}\n')
    w('왕복 지연을 요청 전송(t_recv − t0), 큐 대기(t_deq − t_recv), 락 대기(t_lock − t_deq), 처리(t_send − t_lock), 응답 전송(t5 − t_send)으로 나눈 평균(µs).\n')
    w('| 조건 | IPC | 요청 전송 | 큐 대기 | 락 대기 | 처리 | 응답 전송 | 합 | 락 대기 비율 |')
    w('| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |')
    for key, by in sorted(data[exp].items(), key=sort_key):
        for i in IPCS:
            if i not in by:
                continue
            rs = by[i]
            seg = [med(rs, lambda r, k=k: r[k]) for k in SEGS]
            tot = sum(x or 0 for x in seg)
            share = (seg[2] or 0) / tot * 100 if tot else None
            w(f"| {cond_label(key)} | {i} | " + ' | '.join(fmt(x, 2) for x in seg) + f" | {fmt(tot, 1)} | {fmt(share, 1)}% |")


def table_memory(exp, title):
    if exp not in data:
        return
    w(f'\n## {title}\n')
    w('서버 줄의 값. IPC 버퍼 용량은 pipe: 연 FIFO 버퍼 합, mq: 큐 용량 합, shm: 공유 메모리 크기. 실제 사용은 shm 만 안다(`/proc/sysvipc/shm` rss). '
      '가장 작은 응답 채널은 pipe 의 F_GETPIPE_SZ 최솟값, mq 응답 큐 용량, shm 응답 링 크기.\n')
    w('| 조건 | IPC | 서버 VmHWM kB | IPC 버퍼 용량 kB | shm 실제 kB | 가장 작은 응답 채널 B | 클라이언트 VmHWM kB(가장 큰 것) |')
    w('| --- | --- | ---: | ---: | ---: | ---: | ---: |')
    for key, by in sorted(data[exp].items(), key=sort_key):
        for i in IPCS:
            if i not in by:
                continue
            rs = by[i]
            w(f"| {cond_label(key)} | {i} | {fmt(med(rs, lambda r: r.get('srv_vmhwm_kb')), 0)} | {fmt(med(rs, lambda r: r.get('srv_ipc_cap_kb')), 0)} | "
              f"{fmt(med(rs, lambda r: r.get('srv_ipc_rss_kb')), 0)} | {fmt(med(rs, lambda r: r.get('srv_ipc_min_chan')), 0)} | {fmt(med(rs, lambda r: r.get('vmhwm_kb')), 0)} |")


table_basic('type', '요청 종류별 (클라이언트 1, 동시 요청 1, 그림판 1) — P1, P2, P7')
table_segments('type', '요청 종류별 구간별 시간')
table_basic('thr', '처리량 (클라이언트 1, 동시 요청 16, 기준 혼합) — P3')
table_segments('thr', '처리량 실험의 구간별 시간')
table_basic('scale', '클라이언트 수 (동시 요청 1, 그림판 8) — P4, P5')
table_segments('scale', '클라이언트 수별 구간별 시간')
table_memory('scale', '클라이언트 수별 메모리 — P7, N-5')
table_basic('lock', '락 방식 (그림판 1, 기준 혼합) — P6')
table_segments('lock', '락 방식별 구간별 시간')
table_basic('target', f'목표 부하 (클라이언트 1024개가 초당 {TARGET_HZ}개씩, 동시 4) — P8, N-2')
table_segments('target', '목표 부하의 구간별 시간')
table_basic('cause-procs', '원인 확인: 띄운 요청 수(1024)는 같고 클라이언트 프로세스 수만 다름 (기준 혼합, 그림판 8)')
table_segments('cause-procs', '원인 확인(프로세스 수)의 구간별 시간')
table_basic('imp-mq-q', '원인 확인·개선 1: mq 응답 큐 수 Q (클라이언트 1024, 기준 혼합, 동시 1)')
table_segments('imp-mq-q', 'mq 응답 큐 수별 구간별 시간')
table_memory('imp-mq-q', 'mq 응답 큐 수별 메모리')
table_basic('imp-mq-req', '원인 확인·개선 2: mq 요청 큐 용량과 입장 제한 (응답 큐 8개, 기준 혼합, 동시 1)')
table_segments('imp-mq-req', 'mq 요청 큐 실험의 구간별 시간')
table_basic('imp-scale', '개선 전후: 처음 설계(클라이언트 1024) / 개선한 설계(클라이언트 1~1024), 기준 혼합, 동시 1, 그림판 8')
table_segments('imp-scale', '개선 전후의 구간별 시간')
table_memory('imp-scale', '개선 전후의 메모리')
table_basic('imp-target', f'개선 뒤 목표 부하 (클라이언트 1024개가 초당 {TARGET_HZ}개씩, 동시 4) — N-2')
table_segments('imp-target', '개선 뒤 목표 부하의 구간별 시간')
table_basic('mem-shm', '메모리 절감: shm 응답 링 칸 수 34 / 17 × 동시 요청 1·4 (클라이언트 1024, 클라이언트별 요청 링)')
table_memory('mem-shm', 'shm 응답 링 칸 수별 메모리 — N-5')
table_basic('cause-workers', '원인 확인: 서버 워커 수 (클라이언트 1, 동시 요청 16, 기준 혼합, 그림판 1) — P3')
table_segments('cause-workers', '워커 수별 구간별 시간')
table_basic('cause-lockw', '원인 확인: 그림판 전체 mutex에서 서버 워커 수 (그림판 1, 클라이언트 64, 동시 요청 1) — P6')
table_segments('cause-lockw', '그림판 전체 mutex, 워커 수별 구간별 시간')
table_basic('cause-mix', '원인 확인: 요청 종류만 바꿈 — DRAW만 / SNAPSHOT만 (클라이언트 1, 동시 요청 16, 그림판 1) — P3')
table_segments('cause-mix', '요청 종류별 구간별 시간')
table_basic('cause-ring', '원인 확인: shm 응답 링 칸 수만 바꿈 34 / 68 / 136 (클라이언트 1, 동시 요청 16, 기준 혼합) — P3')
table_memory('cause-ring', 'shm 응답 링 칸 수별 메모리')
KNOWN = ('type', 'thr', 'scale', 'lock', 'target', 'cause-procs', 'imp-mq-q', 'imp-mq-req', 'imp-scale', 'imp-target', 'mem-shm', 'cause-workers', 'cause-lockw',
         'cause-mix', 'cause-ring')
for exp in sorted(e for e in data if e not in KNOWN):
    table_basic(exp, f'실험 {exp}')
    table_segments(exp, f'실험 {exp} 구간별 시간')
    table_memory(exp, f'실험 {exp} 메모리')

# ---- 그림 ----


def client_axis(a, xs=(1, 4, 16, 64, 256, 1024)):
    a.set_xscale('log', base=2)
    a.set_xticks(list(xs))
    a.set_xticklabels([str(x) for x in xs])
    a.minorticks_off()


def save(fig, name):
    fig.tight_layout()
    fig.savefig(os.path.join(DST, name))
    plt.close(fig)
    w(f'\n![{name}]({name})\n')


def errbar(runs, f):
    """가운데 범위를 막대 오차로 (중앙값 기준 아래, 위)"""
    m, r = med(runs, f), mid_range(runs, f)
    if m is None or r is None:
        return (0, 0)
    return (m - r[0], r[1] - m)


def bars(ax, groups, f, ylabel, title, log=False, hline=None):
    """groups = [(x 라벨, {ipc: runs})]. IPC 마다 색을 정해 묶음 막대로"""
    width = 0.8 / 3
    for k, i in enumerate(IPCS):
        xs, ys, es = [], [], ([], [])
        for g, (_, by) in enumerate(groups):
            if i not in by:
                continue
            v = med(by[i], f)
            if v is None:
                continue
            e = errbar(by[i], f)
            xs.append(g + (k - 1) * width); ys.append(v); es[0].append(e[0]); es[1].append(e[1])
        if xs:
            ax.bar(xs, ys, width, yerr=es, color=COLOR[i], label=i, capsize=2, ecolor='#555')
    ax.set_xticks(range(len(groups))); ax.set_xticklabels([g for g, _ in groups], fontsize=8)
    ax.set_ylabel(ylabel); ax.set_title(title, fontsize=10); ax.grid(alpha=.3, axis='y')
    if log:
        ax.set_yscale('log')
    if hline:
        ax.axhline(hline[0], color='gray', ls='--', lw=1)
        ax.text(ax.get_xlim()[0], hline[0] * (1.08 if log else 1.02), hline[1], color='gray', fontsize=8, va='bottom')
    ax.legend(fontsize=8)


w('\n## 그림\n')
w('막대의 가는 선은 실행별 값의 가운데 범위(좋은 쪽과 나쁜 쪽 4분의 1을 뺀 범위)다.\n')
if 'type' in data:
    fig, ax = plt.subplots(1, 2, figsize=(11, 4))
    for i in IPCS:
        xs, p50, p99 = [], [], []
        for key, by in data['type'].items():
            if i not in by:
                continue
            d = dict(zip(KEYS, key))
            size = 56 if d['draw_pct'] == '100' else 56 + int(d['snap']) ** 2
            xs.append(size)
            p50.append(med(by[i], lambda r: r['lat_p50_us']))
            p99.append(med(by[i], lambda r: r['lat_p99_us']))
        o = sorted(range(len(xs)), key=lambda k: xs[k])
        ax[0].plot([xs[k] for k in o], [p50[k] for k in o], marker=MARK[i], color=COLOR[i], label=i)
        ax[1].plot([xs[k] for k in o], [p99[k] for k in o], marker=MARK[i], color=COLOR[i], label=i)
    for a, t in zip(ax, ('중앙값', '상위 99%')):
        a.set_xscale('log'); a.set_xlabel('응답 크기 (B, 머리 포함)'); a.set_ylabel('왕복 지연 (µs)')
        a.set_title(f'요청 종류별 왕복 지연 {t} (클라이언트 1)'); a.grid(alpha=.3); a.legend()
    save(fig, 'type_latency.png')

if 'thr' in data:
    fig, ax = plt.subplots(1, 3, figsize=(12, 3.8))
    g = [('기준 혼합', by) for _, by in items('thr')]
    bars(ax[0], g, lambda r: r['thr_req_s'], '처리량 (req/s)', '처리량 (클라이언트 1, 동시 16)')
    bars(ax[1], g, cpu_us, 'CPU 시간 / 요청 (µs)', '요청 하나의 CPU 시간 (클라이언트 + 서버)')
    bars(ax[2], g, ctx, '자발적 문맥 교환 / 요청', '요청 하나의 자발적 문맥 교환')
    save(fig, 'thr.png')

if 'scale' in data:
    for m, tag in (('90', 'mix'), ('100', 'draw')):
        fig, ax = plt.subplots(1, 2, figsize=(11, 4))
        for i in IPCS:
            pts = []
            for key, by in data['scale'].items():
                d = dict(zip(KEYS, key))
                if d['draw_pct'] != m or i not in by:
                    continue
                pts.append((int(d['clients']), med(by[i], lambda r: r['thr_req_s']), med(by[i], lambda r: r['draw_p99_us']),
                            mid_range(by[i], lambda r: r['thr_req_s'])))
            pts.sort()
            if not pts:
                continue
            x = [p[0] for p in pts]
            ax[0].plot(x, [p[1] for p in pts], marker=MARK[i], color=COLOR[i], label=i)
            ax[0].fill_between(x, [p[3][0] for p in pts], [p[3][1] for p in pts], color=COLOR[i], alpha=.15)
            ax[1].plot(x, [p[2] for p in pts], marker=MARK[i], color=COLOR[i], label=i)
        name = '기준 혼합(DRAW 90%)' if m == '90' else 'DRAW만'
        ax[0].set_title(f'클라이언트 수별 처리량, {name}'); ax[0].set_ylabel('처리량 (req/s)')
        ax[1].set_title(f'클라이언트 수별 DRAW 지연 상위 99%, {name}'); ax[1].set_ylabel('왕복 지연 (µs)'); ax[1].set_yscale('log')
        for a in ax:
            client_axis(a); a.set_xlabel('동시 클라이언트 수 (동시 요청 1)'); a.grid(alpha=.3); a.legend()
        save(fig, f'scale_{tag}.png')
    # 구간별 시간 (기준 혼합, 클라이언트 64·1024)
    fig, ax = plt.subplots(1, 2, figsize=(11, 4))
    for a, n in zip(ax, ('64', '1024')):
        for k, i in enumerate(IPCS):
            for d, by in items('scale', draw_pct='90', clients=n):
                if i not in by:
                    continue
                bottom = 0
                for s, nm, c in zip(SEGS, SEG_NAMES, SEG_COLS):
                    v = med(by[i], lambda r, s=s: r[s]) or 0
                    a.bar(k, v, bottom=bottom, color=c, label=nm if k == 0 else None)
                    bottom += v
        a.set_xticks(range(3)); a.set_xticklabels(IPCS); a.set_ylabel('평균 (µs)')
        a.set_title(f'구간별 시간, 기준 혼합, 클라이언트 {n}'); a.grid(alpha=.3, axis='y'); a.legend(fontsize=8)
    save(fig, 'scale_segments.png')
    # 메모리: 서버 VmHWM(shm 은 서버가 붙인 공유 메모리 중 건드린 쪽이 들어 있다)과 IPC 버퍼 용량(pipe·mq 는 커널 메모리라 VmHWM 밖)
    fig, ax = plt.subplots(1, 2, figsize=(11, 4))
    for i in IPCS:
        pts = []
        for d, by in items('scale', draw_pct='90'):
            if i not in by:
                continue
            pts.append((int(d['clients']), (med(by[i], lambda r: r.get('srv_vmhwm_kb')) or 0) / 1024,
                        (med(by[i], lambda r: r.get('srv_ipc_cap_kb')) or 0) / 1024))
        pts.sort()
        if pts:
            ax[0].plot([p[0] for p in pts], [p[1] for p in pts], marker=MARK[i], color=COLOR[i], label=i)
            ax[1].plot([p[0] for p in pts], [p[2] for p in pts], marker=MARK[i], color=COLOR[i], label=i)
    ax[0].set_ylabel('서버 VmHWM (MB)'); ax[0].set_title('서버 프로세스 메모리 (shm 은 공유 메모리 포함)')
    ax[1].set_ylabel('IPC 버퍼 용량 (MB)'); ax[1].set_title('IPC 버퍼 용량 (pipe·mq 는 커널 메모리)')
    for a in ax:
        client_axis(a); a.set_xlabel('동시 클라이언트 수'); a.grid(alpha=.3); a.legend()
    save(fig, 'scale_memory.png')

if 'lock' in data:
    fig, ax = plt.subplots(1, 2, figsize=(11, 4))
    for i in IPCS:
        for lk, ls in (('rw', '-'), ('room', '--')):
            pts = []
            for d, by in items('lock', lock=lk):
                if i not in by:
                    continue
                lock = med(by[i], lambda r: r['lock_us']) or 0
                tot = sum(med(by[i], lambda r, s=s: r[s]) or 0 for s in SEGS)
                pts.append((int(d['clients']), lock / tot * 100 if tot else 0, med(by[i], lambda r: r['draw_p99_us'])))
            pts.sort()
            if pts:
                ax[0].plot([p[0] for p in pts], [p[1] for p in pts], ls=ls, marker=MARK[i], color=COLOR[i], label=f'{i} {lk}')
                ax[1].plot([p[0] for p in pts], [p[2] for p in pts], ls=ls, marker=MARK[i], color=COLOR[i], label=f'{i} {lk}')
    ax[0].set_ylabel('락 대기 / 왕복 지연 (%)'); ax[0].set_title('락 대기 비율 (그림판 1)')
    ax[1].set_ylabel('DRAW 왕복 지연 상위 99% (µs)'); ax[1].set_title('DRAW 상위 99% (그림판 1)'); ax[1].set_yscale('log')
    for a in ax:
        client_axis(a, (16, 64, 256)); a.set_xlabel('동시 클라이언트 수'); a.grid(alpha=.3); a.legend(fontsize=8)
    save(fig, 'lock.png')

if 'target' in data or 'imp-target' in data:
    groups = []
    for exp, name in (('target', '처음 설계'), ('imp-target', '개선한 설계')):
        for _, by in items(exp):
            groups.append((name, by))
    if groups:
        fig, ax = plt.subplots(1, 2, figsize=(11, 4))
        bars(ax[0], groups, lambda r: r['draw_p99_us'], 'DRAW 왕복 지연 상위 99% (µs)',
             f'목표 부하에서 DRAW 상위 99% (클라이언트 1024 × {TARGET_HZ}/s)', log=True, hline=(FRAME_US, '16.7ms (N-2)'))
        bars(ax[1], groups, lambda r: r['thr_req_s'], '처리량 (req/s)', '목표 부하에서 처리한 요청',
             hline=(1024 * TARGET_HZ, f'보낸 속도 {1024 * TARGET_HZ:,}/s'))
        save(fig, 'target.png')

if 'cause-procs' in data:
    groups = []
    for d, by in sorted(items('cause-procs'), key=lambda x: int(x[0]['clients'])):
        groups.append((f"{d['clients']} × {d['inflight']}", by))
    fig, ax = plt.subplots(1, 3, figsize=(13, 3.8))
    bars(ax[0], groups, lambda r: r['thr_req_s'], '처리량 (req/s)', '처리량 (띄운 요청 1024)')
    bars(ax[1], groups, ctx, '자발적 문맥 교환 / 요청', '요청 하나의 자발적 문맥 교환')
    bars(ax[2], groups, lambda r: r['queue_us'], '큐 대기 평균 (µs)', '서버 작업 큐 대기', log=True)
    for a in ax:
        a.set_xlabel('클라이언트 수 × 동시 요청')
    save(fig, 'cause_procs.png')

if 'imp-mq-q' in data:
    pts = []
    for d, by in items('imp-mq-q'):
        if 'mq' not in by:
            continue
        q = int(dict(x.split('=') for x in d['env'].split(';'))['OSP_MQ_RESP_Q'])
        rs = by['mq']
        pts.append((q, med(rs, lambda r: r['thr_req_s']), mid_range(rs, lambda r: r['thr_req_s']),
                    med(rs, lambda r: r['draw_p99_us']), med(rs, lambda r: r['resp_us']), med(rs, lambda r: r['queue_us']),
                    med(rs, lambda r: r['req_us'])))
    pts.sort()
    if pts:
        fig, ax = plt.subplots(1, 3, figsize=(13, 3.8))
        x = [p[0] for p in pts]
        ax[0].plot(x, [p[1] for p in pts], marker='s', color=COLOR['mq'], label='mq')
        ax[0].fill_between(x, [p[2][0] for p in pts], [p[2][1] for p in pts], color=COLOR['mq'], alpha=.2)
        ax[0].set_ylabel('처리량 (req/s)'); ax[0].set_title('mq 응답 큐 수별 처리량 (클라이언트 1024)')
        ax[1].plot(x, [p[3] for p in pts], marker='s', color=COLOR['mq'], label='DRAW p99')
        ax[1].set_ylabel('DRAW 왕복 지연 상위 99% (µs)'); ax[1].set_yscale('log'); ax[1].set_title('DRAW 상위 99%')
        for k, (lbl, c) in enumerate((('요청 전송', '#8d99ae'), ('큐 대기', '#ef8354'), ('응답 전송', '#bfc0c0'))):
            ax[2].plot(x, [p[6 if k == 0 else 5 if k == 1 else 4] for p in pts], marker='os^'[k], color=c, label=lbl)
        ax[2].set_ylabel('평균 (µs)'); ax[2].set_yscale('log'); ax[2].set_title('구간별 시간')
        for a in ax:
            a.set_xscale('log', base=2); a.set_xticks(x); a.set_xticklabels([str(v) for v in x])
            a.set_xlabel('응답 큐 수 Q (OSP_MQ_RESP_Q)'); a.grid(alpha=.3); a.legend(fontsize=8)
        save(fig, 'imp_mq_q.png')

if 'imp-mq-req' in data:
    groups = []
    for d, by in sorted(items('imp-mq-req'), key=lambda x: (int(x[0]['clients']), x[0]['env'])):
        kv = dict(x.split('=') for x in d['env'].split(';'))
        lbl = f"N={d['clients']}\n요청 큐 {int(kv.get('OSP_MQ_REQ_QBYTES', 16384)):,}B\n" + ('입장 제한' if kv.get('OSP_MQ_ADMIT') == '1' else '제한 없음')
        groups.append((lbl, by))
    fig, ax = plt.subplots(1, 3, figsize=(13, 4.2))
    bars(ax[0], groups, lambda r: r['thr_req_s'], '처리량 (req/s)', 'mq 처리량 (응답 큐 8개)')
    bars(ax[1], groups, srv_ctx, '서버 자발적 문맥 교환 / 요청', '서버 문맥 교환')
    bars(ax[2], groups, ctx, '자발적 문맥 교환 / 요청', '전체 문맥 교환 (클라이언트 + 서버)', log=True)
    save(fig, 'imp_mq_req.png')

if 'imp-mq-q' in data:
    # 실행 하나하나: 처리량이 낮은 실행일수록 요청 하나의 문맥 교환이 많다 (요청 큐의 한꺼번에 깨우기)
    fig, ax = plt.subplots(figsize=(7.5, 4.6))
    sets = []
    for d, by in items('imp-mq-q'):
        if 'mq' in by:
            q = int(dict(x.split('=') for x in d['env'].split(';'))['OSP_MQ_RESP_Q'])
            sets.append((q, f'응답 큐 {q}개, 입장 제한 없음', by['mq']))
    for d, by in items('imp-mq-req'):
        kv = dict(x.split('=') for x in d['env'].split(';'))
        if d['clients'] == '1024' and kv.get('OSP_MQ_ADMIT') == '1' and 'mq' in by:
            sets.append((99, '응답 큐 8개 + 입장 제한 (개선 1)', by['mq']))
    cmap = {1: '#c9d6df', 2: '#8d99ae', 4: '#ef8354', 8: '#d1495b', 99: '#2a9d8f'}
    for q, lbl, rs in sorted(sets, key=lambda x: x[0]):
        ax.scatter([ctx(r) for r in rs], [r['thr_req_s'] for r in rs], s=22, color=cmap.get(q, 'k'),
                   marker='D' if q == 99 else 'o', label=f'{lbl} ({len(rs)}번)', alpha=.85, edgecolors='none')
    ax.set_xscale('log'); ax.set_xlabel('요청 하나의 자발적 문맥 교환 (클라이언트 + 서버, 로그)')
    ax.set_ylabel('처리량 (req/s)'); ax.set_title('mq, 클라이언트 1024: 실행 하나 = 점 하나')
    ax.grid(alpha=.3); ax.legend(fontsize=8, loc='upper right')
    save(fig, 'imp_mq_runs.png')

if 'imp-scale' in data:
    fig, ax = plt.subplots(1, 2, figsize=(11, 4))
    for i in IPCS:
        before = sorted((int(d['clients']), med(by[i], lambda r: r['thr_req_s']), med(by[i], lambda r: r['draw_p99_us']))
                        for d, by in items('scale', draw_pct='90') if i in by and d['clients'] in ('1', '64', '256', '1024'))
        if before:
            lab = f'{i} 처음 설계' if i != 'pipe' else 'pipe (개선 없음)'
            ax[0].plot([p[0] for p in before], [p[1] for p in before], ls='--' if i != 'pipe' else '-', marker=MARK[i], color=COLOR[i], alpha=.6 if i != 'pipe' else 1, label=lab)
            ax[1].plot([p[0] for p in before], [p[2] for p in before], ls='--' if i != 'pipe' else '-', marker=MARK[i], color=COLOR[i], alpha=.6 if i != 'pipe' else 1, label=lab)
        after = sorted((int(d['clients']), med(by[i], lambda r: r['thr_req_s']), med(by[i], lambda r: r['draw_p99_us']))
                       for d, by in items('imp-scale', env=IMP_ENV) if i in by)
        if after:
            ax[0].plot([p[0] for p in after], [p[1] for p in after], marker=MARK[i], color=COLOR[i], lw=2, label=f'{i} 개선')
            ax[1].plot([p[0] for p in after], [p[2] for p in after], marker=MARK[i], color=COLOR[i], lw=2, label=f'{i} 개선')
    ax[0].set_ylabel('처리량 (req/s)'); ax[0].set_title('개선 전후 처리량 (기준 혼합, 동시 1, 그림판 8)')
    ax[1].set_ylabel('DRAW 왕복 지연 상위 99% (µs)'); ax[1].set_yscale('log'); ax[1].set_title('개선 전후 DRAW 상위 99%')
    for a in ax:
        client_axis(a, (1, 64, 256, 1024)); a.set_xlabel('동시 클라이언트 수'); a.grid(alpha=.3); a.legend(fontsize=7)
    save(fig, 'imp_scale.png')

if 'mem-shm' in data:
    groups = []
    for d, by in sorted(items('mem-shm'), key=lambda x: (int(x[0]['inflight']), -int(dict(y.split('=') for y in x[0]['env'].split(';'))['OSP_SHM_RESP_CHUNKS']))):
        c = dict(y.split('=') for y in d['env'].split(';'))['OSP_SHM_RESP_CHUNKS']
        groups.append((f"동시 {d['inflight']}\n응답 링 {c}칸", by))
    fig, ax = plt.subplots(1, 3, figsize=(13, 4.2))
    bars(ax[0], groups, lambda r: (r.get('srv_ipc_cap_kb') or 0) / 1024, '공유 메모리 크기 (MB)', 'shm 공유 메모리 (클라이언트 1024)')
    bars(ax[1], groups, lambda r: r['thr_req_s'], '처리량 (req/s)', '처리량')
    bars(ax[2], groups, lambda r: r['draw_p99_us'], 'DRAW 왕복 지연 상위 99% (µs)', 'DRAW 상위 99%', log=True)
    save(fig, 'mem_shm.png')

if 'cause-workers' in data:
    fig, ax = plt.subplots(1, 3, figsize=(13, 3.8))
    for i in IPCS:
        pts = sorted((int(d['workers']), med(by[i], lambda r: r['thr_req_s']), mid_range(by[i], lambda r: r['thr_req_s']),
                      med(by[i], srv_ctx), med(by[i], lambda r: r['resp_us']))
                     for d, by in items('cause-workers') if i in by)
        if not pts:
            continue
        x = [p[0] for p in pts]
        ax[0].plot(x, [p[1] for p in pts], marker=MARK[i], color=COLOR[i], label=i)
        ax[0].fill_between(x, [p[2][0] for p in pts], [p[2][1] for p in pts], color=COLOR[i], alpha=.15)
        ax[1].plot(x, [p[3] for p in pts], marker=MARK[i], color=COLOR[i], label=i)
        ax[2].plot(x, [p[4] for p in pts], marker=MARK[i], color=COLOR[i], label=i)
    ax[0].set_ylabel('처리량 (req/s)'); ax[0].set_title('서버 워커 수별 처리량 (클라이언트 1, 동시 16)')
    ax[1].set_ylabel('서버 자발적 문맥 교환 / 요청'); ax[1].set_title('서버가 잠드는 횟수')
    ax[2].set_ylabel('응답 전송 평균 (µs)'); ax[2].set_title('응답 전송 구간 (t5 − t_send)')
    for a in ax:
        a.set_xscale('log', base=2); a.set_xticks([1, 2, 4, 8]); a.set_xticklabels(['1', '2', '4', '8'])
        a.set_xlabel('서버 워커 수 (-w)'); a.grid(alpha=.3); a.legend(fontsize=8)
    save(fig, 'cause_workers.png')

if 'cause-mix' in data or 'cause-ring' in data:
    # P3 원인 확인: 요청 종류만 바꾸기(DRAW만 / SNAPSHOT만), shm 응답 링 칸 수만 바꾸기
    fig, ax = plt.subplots(1, 2, figsize=(11, 3.8))
    groups = []
    for d, by in sorted(items('cause-mix'), key=lambda x: -int(x[0]['draw_pct'])):
        groups.append(('DRAW만 (응답 56B)' if d['draw_pct'] == '100' else 'SNAPSHOT만 (응답 64KB)', by))
    bars(ax[0], groups, lambda r: r['thr_req_s'], '처리량 (req/s)', '요청 종류별 처리량 (클라이언트 1, 동시 16)', log=True)
    pts = []
    for d, by in items('cause-ring'):
        if 'shm' in by:
            c = int(dict(x.split('=') for x in d['env'].split(';'))['OSP_SHM_RESP_CHUNKS'])
            pts.append((c, med(by['shm'], lambda r: r['thr_req_s']), mid_range(by['shm'], lambda r: r['thr_req_s'])))
    pts.sort()
    if pts:
        x = [p[0] for p in pts]
        ax[1].plot(x, [p[1] for p in pts], marker=MARK['shm'], color=COLOR['shm'], label='shm')
        ax[1].fill_between(x, [p[2][0] for p in pts], [p[2][1] for p in pts], color=COLOR['shm'], alpha=.2)
        ax[1].set_xscale('log', base=2); ax[1].set_xticks(x); ax[1].set_xticklabels([f'{v}칸\n(64KB 응답 {v // 17}개)' for v in x])
        ax[1].set_ylim(0, max(p[2][1] for p in pts) * 1.3)
        ax[1].set_ylabel('처리량 (req/s)'); ax[1].set_title('shm 응답 링 크기만 바꿈 (기준 혼합, 동시 16)')
        ax[1].set_xlabel('응답 링 칸 수 (OSP_SHM_RESP_CHUNKS)'); ax[1].grid(alpha=.3); ax[1].legend(fontsize=8)
    save(fig, 'cause_mix.png')

if 'cause-lockw' in data:
    fig, ax = plt.subplots(1, 2, figsize=(11, 3.8))
    for i in IPCS:
        pts = []
        for d, by in items('cause-lockw'):
            if i not in by:
                continue
            lock = med(by[i], lambda r: r['lock_us']) or 0
            tot = sum(med(by[i], lambda r, s=s: r[s]) or 0 for s in SEGS)
            pts.append((int(d['workers']), lock, lock / tot * 100 if tot else 0))
        pts.sort()
        if pts:
            ax[0].plot([p[0] for p in pts], [p[1] for p in pts], marker=MARK[i], color=COLOR[i], label=i)
            ax[1].plot([p[0] for p in pts], [p[2] for p in pts], marker=MARK[i], color=COLOR[i], label=i)
    ax[0].set_ylabel('락 대기 평균 (µs)'); ax[0].set_title('그림판 전체 mutex의 락 대기 (클라이언트 64)')
    ax[1].set_ylabel('락 대기 / 왕복 지연 (%)'); ax[1].set_title('락 대기 비율')
    for a in ax:
        a.set_xscale('log', base=2); a.set_xticks([1, 4, 16]); a.set_xticklabels(['1', '4', '16'])
        a.set_xlabel('서버 워커 수 (-w)'); a.grid(alpha=.3); a.legend(fontsize=8)
    save(fig, 'cause_lockw.png')

with open(os.path.join(DST, 'summary.md'), 'w', encoding='utf-8') as f:
    f.write('\n'.join(out) + '\n')
print(f'{DST}/summary.md 와 그림을 만들었다 (요약에 쓴 실행 {n_ok}번, 뺀 실행 {n_drop}번)')
