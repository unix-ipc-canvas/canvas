#!/usr/bin/env python3
"""측정 원자료 검사. make test 의 T-40, T-41, T-42 가 실행한다.

  check-results.py T-40   1단계 원자료에 메시지 크기 5단계 이상 × 클라이언트 1·2·4·8 × IPC 3종의 모든 조합이 있는가
  check-results.py T-41   results/ 아래 1단계 CSV 머리글이 원자료 형식의 열과 같은가 (2단계 results/app/ 은 T-42)
  check-results.py T-42   2단계 원자료(results/app/)에 실험마다 세 IPC 가 같은 조건으로 30번 이상 있는가

첫 줄에 한 줄 요약을 쓰고, 그다음 줄부터 문제를 쓴다.
종료 코드: 0 = 통과, 1 = 실패, 2 = 검사할 원자료가 없다(SKIP).
"""
import csv
import glob
import os
import re
import sys

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

# 1단계 원자료: results/ 바로 아래(처음 측정)와 results/stage1-*/ (다시 잰 것). 실험(results/exp-*/)은 조합 일부만 잰다
STAGE1_DIRS = ['results'] + sorted(d for d in glob.glob('results/stage1-*') if os.path.isdir(d))
STAGE1_FILES = tuple(os.path.join(d, n) for d in STAGE1_DIRS for n in ('bench_latency.csv', 'bench_throughput.csv'))
IPCS = ('pipe', 'mq', 'shm')
CLIENTS = (1, 2, 4, 8)
ROLES = ('client', 'server')   # 1단계는 실행 하나가 client, server 두 줄이다
MIN_SIZES = 5


def read(path):
    with open(path, encoding='utf-8') as f:
        return f.read()


def finish(code, summary, problems=()):
    print(summary)
    for p in problems:
        print(f'  {p}')
    sys.exit(code)


def t40():
    files = [f for f in STAGE1_FILES if os.path.exists(f)]
    if not files:
        finish(2, '1단계 원자료 없음 (측정 전)')
    problems, notes = [], []
    for f in STAGE1_FILES:
        if f not in files:
            problems.append(f'{f} 가 없다')
            continue
        with open(f, newline='', encoding='utf-8') as fp:
            rows = [r for r in csv.DictReader(fp) if r.get('stage') == 'bench']
        count = {}
        for r in rows:
            try:
                key = (r['ipc'], int(r['msg_size']), int(r['clients']), r['role'])
            except (KeyError, ValueError):
                problems.append(f'{f}: 읽을 수 없는 줄 {r}')
                continue
            count[key] = count.get(key, 0) + 1
        sizes = sorted({k[1] for k in count if k[0] in IPCS})
        if len(sizes) < MIN_SIZES:
            problems.append(f'{f}: 메시지 크기가 {len(sizes)}단계다 ({MIN_SIZES}단계 이상이어야 한다)')
        missing = [(i, s, c, role) for i in IPCS for s in sizes for c in CLIENTS for role in ROLES
                   if (i, s, c, role) not in count]
        for i, s, c, role in missing[:10]:
            problems.append(f'{f}: {i}, {s}B, 클라이언트 {c}, {role} 줄이 없다')
        if len(missing) > 10:
            problems.append(f'{f}: 그 밖에 {len(missing) - 10}개 조합이 더 없다')
        reps = sorted({n for k, n in count.items() if k[0] in IPCS})
        rep = f'{reps[0]}번' if len(reps) == 1 else f'{reps[0]}~{reps[-1]}번' if reps else '0번'
        notes.append(f'{os.path.relpath(f, "results")} 크기 {len(sizes)}단계 × 클라이언트 {len(CLIENTS)} × IPC {len(IPCS)}, 조합마다 {rep}')
    if problems:
        finish(1, '1단계 원자료에 빠진 조합이 있다', problems)
    finish(0, ', '.join(notes))


def header_from_util():
    m = re.search(r'#define\s+CSV_HEADER\s*((?:\\\s*\n\s*"[^"]*"\s*)+)', read('include/util.h'))
    return ''.join(re.findall(r'"([^"]*)"', m.group(1))) if m else None


MEASURE_DOC = 'docs/measurement/README.md'   # 제출용 tar(make dist)에는 없다


def header_from_doc():
    doc = read(MEASURE_DOC)
    sec = re.search(r'^## 원자료 형식\s*$(.*?)^## ', doc, flags=re.S | re.M)
    block = re.search(r'```text\n(.*?)```', sec.group(1), flags=re.S) if sec else None
    return ''.join(block.group(1).split()) if block else None


def t41():
    # 측정 방법 문서가 없으면(제출용 tar) 문서와의 비교만 건너뛰고 CSV 는 CSV_HEADER 로 검사한다
    have_doc = os.path.exists(MEASURE_DOC)
    code = header_from_util()
    doc = header_from_doc() if have_doc else None
    if code is None or (have_doc and doc is None):
        finish(1, '원자료 형식을 읽지 못했다',
               [f'{name}에서 머리글을 찾지 못했다' for name, h, want in
                (('include/util.h 의 CSV_HEADER', code, True), ('측정 방법의 원자료 형식', doc, have_doc))
                if want and h is None])
    if have_doc and code != doc:
        finish(1, '측정 방법 문서의 원자료 형식과 util.h 의 CSV_HEADER 가 다르다', [f'문서: {doc}', f'코드: {code}'])
    want = code.split(',')
    files = sorted(f for f in glob.glob('results/**/*.csv', recursive=True) if not f.startswith('results/app/'))
    if not files:
        finish(2, 'results/ 에 1단계 CSV 없음 (측정 전)')
    problems, n_rows = [], 0
    for f in files:
        with open(f, newline='', encoding='utf-8') as fp:
            rows = list(csv.reader(fp))
        if not rows or rows[0] != want:
            got = rows[0] if rows else []
            lack = [c for c in want if c not in got]
            extra = [c for c in got if c not in want]
            detail = (f'빠진 열 {lack}' if lack else '') + (f' 남는 열 {extra}' if extra else '')
            problems.append(f'{f}: 머리글이 다르다. {detail or "열 순서가 다르다"}')
            continue
        for no, r in enumerate(rows[1:], start=2):
            if len(r) != len(want):
                problems.append(f'{f}:{no}: 칸이 {len(r)}개다 ({len(want)}개여야 한다)')
        n_rows += len(rows) - 1
    if problems:
        finish(1, 'CSV 열이 원자료 형식과 다르다', problems[:20])
    note = '' if have_doc else ' (측정 방법 문서가 없어 CSV_HEADER 와만 비교)'
    finish(0, f'CSV {len(files)}개, {n_rows}줄이 원자료 형식의 {len(want)}열과 같다{note}')


def t42():
    if not glob.glob('results/app/*.csv'):
        finish(2, '2단계 원자료 없음 (측정 전)')
    finish(1, '2단계 원자료 검사는 측정 스크립트와 함께 채운다')


if __name__ == '__main__':
    {'T-40': t40, 'T-41': t41, 'T-42': t42}.get(sys.argv[1] if len(sys.argv) > 1 else '', lambda: finish(64, __doc__))()
