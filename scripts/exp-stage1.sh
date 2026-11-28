#!/usr/bin/env bash
# 1단계 결과를 설명하는 가설을 확인하는 실험. 측정용 컴퓨터에서만 돌린다.
# CPU 배치 하나만 바꾸고 나머지는 scripts/bench.sh 와 같다. 실험 설계와 결과는 docs/measurement/stage1.md 에 있다.
#
#   scripts/exp-stage1.sh                 네 실험 모두 (반복 20회)
#   EXPS="cpu1" REPS=5 scripts/exp-stage1.sh
#
# 실험 (결과 디렉터리)
#   cpu1   results/exp-cpu1/   모든 프로세스를 논리 CPU 하나(2번, P코어)에 묶는다. 64KB·1MB, 클라이언트 1
#   pbig   results/exp-pbig/   P코어(CPU 0~3)에만 둔다. 64KB·1MB, 클라이언트 1. cpu1 의 대조군(P코어 효과와 CPU 하나 효과를 나눈다)
#   pcore  results/exp-pcore/  P코어(CPU 0~3)에만 둔다. 64B·4KB, 클라이언트 1
#   ecore  results/exp-ecore/  E코어(CPU 4~11)에만 둔다. 64B·4KB, 클라이언트 1
# 묶지 않은 기준은 같은 반복 수로 잰 results/stage1-r20/ 의 같은 조건이다.
#
# 1단계 보강 (11/28, 2단계 예측 뒤에 추가. 기본 목록 밖이라 EXPS="clients" 로 돌린다)
#   clients results/exp-clients/  클라이언트 수만 1·2·4·8·16·64·256·1024 로 바꾼다. 64B·64KB, 동시 요청 1,
#           pipe·mq·shm, 반복 30. 응용(클라이언트 1024개)의 규모를 1단계 echo 로 재 본다(CPU 는 묶지 않는다)
set -u
cd "$(dirname "$0")/.."

EXPS=${EXPS:-"cpu1 pbig pcore ecore"}

for e in $EXPS; do
    if [ "$e" = clients ]; then
        reps=${REPS:-30}
        echo "== 실험 clients: 클라이언트 1~1024, 크기 64B·64KB, 동시 요청 1, 반복 $reps"
        env REPS="$reps" IPCS="pipe mq shm" SIZES="64 65536" CLIENTS="1 2 4 8 16 64 256 1024" MODES=latency \
            OUT="results/exp-clients" scripts/bench.sh || exit
        continue
    fi
    REPS=${REPS:-20}
    case $e in
        cpu1)  cpus=2;    sizes="65536 1048576" ;;
        pbig)  cpus=0-3;  sizes="65536 1048576" ;;
        pcore) cpus=0-3;  sizes="64 4096" ;;
        ecore) cpus=4-11; sizes="64 4096" ;;
        *) echo "모르는 실험: $e" >&2; exit 64 ;;
    esac
    echo "== 실험 $e: taskset -c $cpus, 크기 $sizes, 클라이언트 1, 반복 $REPS"
    taskset -c "$cpus" env REPS="$REPS" IPCS="pipe mq shm" SIZES="$sizes" CLIENTS=1 OUT="results/exp-$e" \
        scripts/bench.sh || exit
done
