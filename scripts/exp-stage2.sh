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
#   pipesz     설계 개선(T6). pipe 의 요청 종류별·기준 실험을 FIFO 버퍼 기본(64KB)과 1MB(OSP_PIPE_SZ)로,
#              묶지 않은 것과 P코어에 묶은 것 각각 번갈아            -> results/exp-app-pipesz/<버퍼>-<배치>-<번>/
#   shmmem     메모리 절감(N-5, T6). shm 의 클라이언트 수·요청 종류별 실험을 링 기본(요청 4개·응답 2개 분량),
#              요청 링만 최소(가장 큰 메시지 1개, 257칸), 둘 다 최소로, 묶지 않은 것과 P코어에 묶은 것 각각 번갈아
#                                                                          -> results/exp-app-shmmem/<링>-<배치>-<번>/
#   pipesz2    pipesz 교차 확인. 1MB 응답에서 버퍼 1MB 가 오히려 느린 결과를 버퍼 256KB 를 더해 다시 본다.
#              응용은 요청 종류별 실험(3번 번갈아 조건마다 6번)    -> results/exp-app-pipesz2/<버퍼>-<배치>-<번>/
#              1단계 echo 벤치(bench_pipe, 256KB·1MB, -f 1·16, 5번 번갈아) -> results/exp-pipesz-bench/bench_pipe.csv
#   shmring    설계 개선(T7). shm 요청 링을 하나로 함께 쓰는 것(기본)과 클라이언트별 링(OSP_SHM_REQ_RING=per)을
#              클라이언트 수·DRAW 비율 실험으로, 묶지 않은 것과 P코어에 묶은 것 각각 번갈아 (3번, 조건마다 6번, 약 25분)
#                                                                          -> results/exp-app-shmring/<링>-<배치>-<번>/
# 반복은 5번(placement 는 2번). 표본을 모으는 placement 는 표본 수집이 CPU 를 써서 처리량을 비교에 쓰지 않는다.
# pipesz, shmmem 은 서버 비교(results/exp-app-servercmp/)와 같은 방식이다: 조건들을 REPS=2 씩 다른 폴더로 번갈아 부른다.
# pipesz 는 5번 번갈아 조건마다 10번(약 15~20분), shmmem 은 조건이 3개라 3번 번갈아 조건마다 6번(약 25분).
# 기본 실행 목록에는 넣지 않는다: EXPS="pipesz shmmem pipesz2 shmring" scripts/exp-stage2.sh
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
            # 시간에 따라 바뀌는 조건이 한쪽에만 몰리지 않게 전과 후를 번갈아 잰다. 64k 쪽은 바깥 환경에 값이 있어도 지운다
            for k in 1 2 3 4 5; do
                for p in free pcore; do
                    pin=""; [ "$p" = pcore ] && pin="taskset -c 0-3"
                    env -u OSP_PIPE_SZ REPS=2 IPCS=pipe EXPS="type base" OUT=results/exp-app-pipesz/64k-$p-$k \
                        $pin scripts/app-bench.sh || rc=1
                    OSP_PIPE_SZ=1048576 REPS=2 IPCS=pipe EXPS="type base" OUT=results/exp-app-pipesz/1m-$p-$k \
                        $pin scripts/app-bench.sh || rc=1
                done
            done ;;
        pipesz2)
            make -s >/dev/null || { rc=1; continue; }
            B=results/exp-pipesz-bench; mkdir -p $B; c=$(git rev-parse --short HEAD)
            for k in 1 2 3 4 5; do
                for p in free pcore; do
                    pin=""; [ "$p" = pcore ] && pin="taskset -c 0-3"
                    for sz in 65536 262144 1048576; do
                        if [ $sz = 65536 ]; then pre="env -u OSP_PIPE_SZ"; else pre="env OSP_PIPE_SZ=$sz"; fi
                        for s in 262144 1048576; do
                            for fl in 1 16; do
                                $pre $pin ./bin/bench_pipe -c 1 -s $s -n 2000 -f $fl -e buf$sz-$p -r $k -o $B/bench_pipe.csv -C $c || rc=1
                            done
                        done
                    done
                done
            done
            for k in 1 2 3; do
                for p in free pcore; do
                    pin=""; [ "$p" = pcore ] && pin="taskset -c 0-3"
                    env -u OSP_PIPE_SZ REPS=2 IPCS=pipe EXPS=type OUT=results/exp-app-pipesz2/64k-$p-$k $pin scripts/app-bench.sh || rc=1
                    OSP_PIPE_SZ=262144 REPS=2 IPCS=pipe EXPS=type OUT=results/exp-app-pipesz2/256k-$p-$k $pin scripts/app-bench.sh || rc=1
                    OSP_PIPE_SZ=1048576 REPS=2 IPCS=pipe EXPS=type OUT=results/exp-app-pipesz2/1m-$p-$k $pin scripts/app-bench.sh || rc=1
                done
            done ;;
        shmmem)
            # 257 = 칸 4096B 로 가장 큰 메시지(머리 24B + MAX_PAYLOAD)를 담는 칸 수. transport_shm.c 가 허용하는 가장 작은 링
            # 요청은 16B 를 넘지 않아 요청 링이 큰 메시지 4개 분량일 까닭이 없다. 응답 링까지 줄이면 1MB 응답을 겹쳐 보내지 못한다
            for k in 1 2 3; do
                for p in free pcore; do
                    pin=""; [ "$p" = pcore ] && pin="taskset -c 0-3"
                    env -u OSP_SHM_CHUNK -u OSP_SHM_REQ_CHUNKS -u OSP_SHM_RESP_CHUNKS REPS=2 IPCS=shm EXPS="clients type" \
                        OUT=results/exp-app-shmmem/def-$p-$k $pin scripts/app-bench.sh || rc=1
                    env -u OSP_SHM_CHUNK -u OSP_SHM_RESP_CHUNKS OSP_SHM_REQ_CHUNKS=257 REPS=2 IPCS=shm EXPS="clients type" \
                        OUT=results/exp-app-shmmem/req-$p-$k $pin scripts/app-bench.sh || rc=1
                    env -u OSP_SHM_CHUNK OSP_SHM_REQ_CHUNKS=257 OSP_SHM_RESP_CHUNKS=257 REPS=2 IPCS=shm EXPS="clients type" \
                        OUT=results/exp-app-shmmem/min-$p-$k $pin scripts/app-bench.sh || rc=1
                done
            done ;;
        shmring)
            # 링 크기는 둘 다 기본값이다(공유: 큰 메시지 4개 분량 1개, 클라이언트별: 큰 메시지 1개 분량 16개)
            for k in 1 2 3; do
                for p in free pcore; do
                    pin=""; [ "$p" = pcore ] && pin="taskset -c 0-3"
                    env -u OSP_SHM_REQ_RING -u OSP_SHM_CHUNK -u OSP_SHM_REQ_CHUNKS -u OSP_SHM_RESP_CHUNKS REPS=2 IPCS=shm \
                        EXPS="clients draw" OUT=results/exp-app-shmring/shared-$p-$k $pin scripts/app-bench.sh || rc=1
                    env -u OSP_SHM_CHUNK -u OSP_SHM_REQ_CHUNKS -u OSP_SHM_RESP_CHUNKS OSP_SHM_REQ_RING=per REPS=2 IPCS=shm \
                        EXPS="clients draw" OUT=results/exp-app-shmring/per-$p-$k $pin scripts/app-bench.sh || rc=1
                done
            done ;;
        *) echo "모르는 실험: $e" >&2; rc=1 ;;
    esac
done
exit $rc
