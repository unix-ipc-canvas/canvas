#!/bin/bash
# race-start.sh - 시작 구간 경쟁 재현 실험 (docs/verification/README.md#시작-구간-경쟁)
#
#   사용: scripts/race-start.sh [-n 반복] [-l 부하] [IPC ...]      기본: -n 100 -l 0 pipe mq shm
#
#   실험 A  서버 둘을 같은 순간에 띄운다. 0.3초 뒤 살아 있는 서버가 정확히 하나여야 한다.
#           둘 다 살아 있으면 요청 채널 하나를 서버 둘이 나눠 읽는다. 둘 다 없으면 서로의 채널을 지운 것이다.
#   실험 B  서버 하나에 같은 번호(7)의 클라이언트 둘을 같은 순간에 띄운다. 하나만 거부되어야 한다.
#           둘 다 실패(타임아웃)면 응답 채널 하나를 둘이 나눠 읽어 응답이 엇갈린 것이다.
#   부하    -l K 면 바쁜 루프 K개를 함께 돌려 CPU 를 빼앗는다. 시작 구간 중간에 선점될 확률이 커진다.
#
# 결과 한 줄: "A ipc=.. 부하=.. 반복=.. 하나=.. 둘다=.. 없음=.." / "B ipc=.. 하나만거부=.. 둘다성공=.. 둘다실패=.."
cd "$(dirname "$0")/.." || exit 1
n=100; load=0
while getopts "n:l:" o; do
    case $o in
        n) n=$OPTARG ;;
        l) load=$OPTARG ;;
        *) echo "사용: $0 [-n 반복] [-l 부하] [IPC ...]" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
ipcs_list=${*:-pipe mq shm}
work=$(mktemp -d)
lp=""
cleanup() { [ -n "$lp" ] && kill $lp 2>/dev/null; rm -rf "$work"; }
trap cleanup EXIT

make -s app > /dev/null || exit 1
make -s ipcclean > /dev/null 2>&1
if [ "$load" -gt 0 ]; then
    for _ in $(seq "$load"); do ( while :; do :; done ) & lp="$lp $!"; done
fi

for ipc in $ipcs_list; do
    # 실험 A
    one=0; two=0; zero=0
    for r in $(seq "$n"); do
        "bin/server_$ipc" -p - > /dev/null 2> "$work/a.err" & A=$!
        "bin/server_$ipc" -p - > /dev/null 2> "$work/b.err" & B=$!
        sleep 0.3
        na=0
        kill -0 $A 2>/dev/null && na=$((na + 1))
        kill -0 $B 2>/dev/null && na=$((na + 1))
        case $na in 1) one=$((one + 1)) ;; 2) two=$((two + 1)) ;; 0) zero=$((zero + 1)) ;; esac
        kill -INT $A $B 2>/dev/null; wait $A $B 2>/dev/null
        make -s ipcclean > /dev/null 2>&1
    done
    echo "A ipc=$ipc 부하=$load 반복=$n 하나=$one 둘다=$two 없음=$zero"

    # 실험 B
    "bin/server_$ipc" -p - > /dev/null 2> "$work/s.err" & SP=$!
    sleep 0.3
    okbad=0; okok=0; badbad=0
    for r in $(seq "$n"); do
        timeout -k 2 20 "bin/client_$ipc" -i 7 -n 100000 -q > /dev/null 2>&1 & C1=$!
        timeout -k 2 20 "bin/client_$ipc" -i 7 -n 100000 -q > /dev/null 2>&1 & C2=$!
        wait $C1; r1=$?; wait $C2; r2=$?
        if [ $r1 -eq 0 ] && [ $r2 -eq 0 ]; then okok=$((okok + 1))
        elif [ $r1 -ne 0 ] && [ $r2 -ne 0 ]; then badbad=$((badbad + 1))
        else okbad=$((okbad + 1)); fi
    done
    kill -INT $SP; wait $SP
    make -s ipcclean > /dev/null 2>&1
    echo "B ipc=$ipc 부하=$load 반복=$n 하나만거부=$okbad 둘다성공=$okok 둘다실패=$badbad"
done
