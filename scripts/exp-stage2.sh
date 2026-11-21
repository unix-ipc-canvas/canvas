#!/usr/bin/env bash
# 2단계 원인 확인 실험. 측정용 컴퓨터에서만 돌린다. scripts/app-bench.sh 를 조건만 바꿔 부른다.
# 무엇을 왜 하는지는 docs/measurement/stage2.md 의 "원인 확인 실험" 이 원본이다.
#
#   scripts/exp-stage2.sh                     아래 실험을 모두 (약 10분)
#   EXPS="wsteps pcore" scripts/exp-stage2.sh 일부만
#
#   wsteps     워커 1~8 (3, 5, 6 포함), 배치는 운영체제에 맡김            -> results/exp-app-wsteps/
#   ecore      워커 실험을 E코어(CPU 4~11)에 묶어서                        -> results/exp-app-ecore/
#   pcore      워커 실험을 P코어(CPU 0~3)에 묶어서                         -> results/exp-app-pcore/
#   placement  워커 실험을 돌리며 스레드의 CPU 와 주파수를 표본으로 모음     -> results/exp-app-placement/
#   ecore-mix  기준·클라이언트 수·DRAW 비율 실험을 E코어에 묶어서            -> results/exp-app-ecore-mix/
#   pcore-mix  같은 실험을 P코어에 묶어서                                  -> results/exp-app-pcore-mix/
#   pipesz     설계 개선(T6). pipe 의 요청 종류별·기준 실험을 FIFO 버퍼 기본(64KB)과 1MB(OSP_PIPE_SZ)로
#              반복마다 번갈아                                             -> results/exp-app-pipesz/{64k,1m}/
# 반복은 5번(placement 는 2번). 표본을 모으는 placement 는 표본 수집이 CPU 를 써서 처리량을 비교에 쓰지 않는다.
# pipesz 는 기본 실행 목록에 넣지 않는다: EXPS=pipesz scripts/exp-stage2.sh
set -u
cd "$(dirname "$0")/.."
EXPS=${EXPS:-"wsteps ecore pcore placement ecore-mix pcore-mix"}
rc=0
for e in $EXPS; do
    echo "== $e"
    case $e in
        wsteps)    REPS=5 EXPS=workers WORKERS="1 2 3 4 5 6 8" OUT=results/exp-app-wsteps scripts/app-bench.sh || rc=1 ;;
        ecore)     REPS=5 EXPS=workers OUT=results/exp-app-ecore taskset -c 4-11 scripts/app-bench.sh || rc=1 ;;
        pcore)     REPS=5 EXPS=workers OUT=results/exp-app-pcore taskset -c 0-3 scripts/app-bench.sh || rc=1 ;;
        placement)
            mkdir -p results/exp-app-placement
            python3 scripts/sample-placement.py results/exp-app-placement/placement.jsonl & sp=$!
            REPS=2 EXPS=workers OUT=results/exp-app-placement scripts/app-bench.sh || rc=1
            kill $sp
            python3 scripts/sample-placement.py --summary results/exp-app-placement/placement.jsonl ;;
        ecore-mix) REPS=5 EXPS="base clients draw" OUT=results/exp-app-ecore-mix taskset -c 4-11 scripts/app-bench.sh || rc=1 ;;
        pcore-mix) REPS=5 EXPS="base clients draw" OUT=results/exp-app-pcore-mix taskset -c 0-3 scripts/app-bench.sh || rc=1 ;;
        pipesz)
            # 시간에 따라 바뀌는 조건이 한쪽에만 몰리지 않게 전과 후를 반복마다 번갈아 잰다. -R 이 반복 번호다
            # DECISION: app-bench.sh 를 반복마다 REPS=1, RUN0 = 반복 - 1 로 부른다. app_env.txt 에는 반복마다의 기록이 이어 붙는다
            for r in 1 2 3 4 5 6 7 8 9 10; do
                env -u OSP_PIPE_SZ REPS=1 RUN0=$((r - 1)) IPCS=pipe EXPS="type base" OUT=results/exp-app-pipesz/64k scripts/app-bench.sh || rc=1
                OSP_PIPE_SZ=1048576 REPS=1 RUN0=$((r - 1)) IPCS=pipe EXPS="type base" OUT=results/exp-app-pipesz/1m scripts/app-bench.sh || rc=1
            done ;;
        *) echo "모르는 실험: $e" >&2; rc=1 ;;
    esac
done
exit $rc
