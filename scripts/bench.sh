#!/usr/bin/env bash
# 1단계 측정. IPC(pipe, 익명 pipe upipe, mq, shm) × 메시지 크기 × 클라이언트 수를 지연과 처리량으로 재서 results/ 에 남긴다.
# 측정 방법의 원본은 docs/measurement/README.md 다. 측정용 컴퓨터에서만 돌린다.
#
#   scripts/bench.sh                                       전체 (반복 5회)
#   REPS=1 SIZES="64 1048576" CLIENTS="1 8" OUT=/tmp/x scripts/bench.sh   일부만, 다른 곳에
#
# 결과 (OUT, 기본 results/)
#   bench_env.txt          측정 환경과 IPC 한도
#   bench_clock.csv        clock_gettime 비용
#   bench_latency.csv      동시 요청 1개. 지연을 볼 때 쓴다
#   bench_throughput.csv   동시 요청 INFLIGHT 개. 처리량을 볼 때 쓴다
# 이미 있는 CSV 에는 덧붙인다. 다시 처음부터 재려면 파일을 지우고 돌린다.
set -u
cd "$(dirname "$0")/.."

REPS=${REPS:-5}
IPCS=${IPCS:-"pipe upipe mq shm"}
SIZES=${SIZES:-"64 1024 4096 16384 65536 262144 1048576"}
CLIENTS=${CLIENTS:-"1 2 4 8"}
INFLIGHT=${INFLIGHT:-16}
OUT=${OUT:-results}

# 크기별 측정 메시지 수(모든 클라이언트 합)와 클라이언트마다의 워밍업 수.
# 작은 메시지는 상위 99% 가 뜻을 갖도록 표본을 많이, 큰 메시지는 실행 시간이 너무 길어지지 않게 줄인다.
count_for() { if [ "$1" -le 16384 ]; then echo 20000; elif [ "$1" -le 65536 ]; then echo 10000; elif [ "$1" -le 262144 ]; then echo 4000; else echo 1000; fi; }
warm_for()  { if [ "$1" -le 16384 ]; then echo 500;   elif [ "$1" -le 65536 ]; then echo 200;   elif [ "$1" -le 262144 ]; then echo 100;  else echo 30; fi; }

# 측정한 코드를 커밋으로 남긴다. 커밋하지 않은 변경이 있으면 멈춘다 (ALLOW_DIRTY=1 이면 -dirty 를 붙여 계속)
commit=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
if ! git diff --quiet HEAD -- src include Makefile scripts 2>/dev/null; then
    if [ "${ALLOW_DIRTY:-0}" != 1 ]; then
        echo "커밋하지 않은 코드 변경이 있다. 커밋한 뒤에 잰다 (무시하려면 ALLOW_DIRTY=1)" >&2
        exit 1
    fi
    commit="$commit-dirty"
fi

mkdir -p "$OUT"
make -s clean >/dev/null && make -s bench >/dev/null || { echo "빌드 실패" >&2; exit 1; }
for i in $IPCS; do [ -x "bin/bench_$i" ] || { echo "bin/bench_$i 가 없다" >&2; exit 1; }; done
make -s ipcclean >/dev/null

# 측정할 때 CPU 를 쓰는 프로세스 (top 의 두 번째 표본: 1초 동안의 사용률, 많이 쓰는 순)
cpu_snapshot() {
    uptime
    top -b -n 2 -d 1 -w 200 | awk '/^top -/{n++} n == 2' | sed -n '1,5p;7,19p'
}

# ---- 측정 환경 ----
# 명령의 출력은 영어 기준으로 남긴다. 한국어 설정이면 lscpu 항목 이름이 바뀌어 plot-bench.py 가 읽지 못한다
{
    export LC_ALL=C
    echo "# scripts/bench.sh 가 측정을 시작할 때 기록한 환경"
    echo "date: $(date -Iseconds)"
    echo "commit: $commit"
    echo "reps: $REPS, ipcs: $IPCS, sizes: $SIZES, clients: $CLIENTS, inflight: $INFLIGHT"
    echo
    echo "## lscpu"; lscpu | grep -E 'Model name|^CPU\(s\)|Thread|Core|Socket|MHz'
    echo; echo "## free -h"; free -h | head -2
    echo; echo "## os"; (. /etc/os-release && echo "$PRETTY_NAME"); uname -r
    echo; echo "## gcc"; gcc --version | head -1
    echo; echo "## virtualization"; systemd-detect-virt 2>/dev/null || echo "(systemd-detect-virt 없음)"
    echo; echo "## cpufreq governor"; cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort | uniq -c || echo "(없음)"
    echo; echo "## IPC 한도"
    echo "msgmax: $(cat /proc/sys/kernel/msgmax)"
    echo "msgmnb: $(cat /proc/sys/kernel/msgmnb)"
    echo "PIPE_BUF: $(getconf PIPE_BUF /)"
    echo "pipe-max-size: $(cat /proc/sys/fs/pipe-max-size)"
    echo "sem (SEMMSL SEMMNS SEMOPM SEMMNI): $(cat /proc/sys/kernel/sem)"
    echo "shmmax: $(cat /proc/sys/kernel/shmmax)"
    echo; echo "## ipcs -l"; ipcs -l
    echo; echo "## 측정 시작 때 CPU 를 쓰는 프로세스 (끌 수 없는 프로그램 확인용)"; cpu_snapshot
} > "$OUT/bench_env.txt"

# ---- 측정 ----
n_runs=0
for _ in $(seq "$REPS"); do for _ in $SIZES; do for _ in $CLIENTS; do for _ in $IPCS; do n_runs=$((n_runs + 2)); done; done; done; done
echo "측정 시작: 커밋 $commit, 실행 $n_runs 번 (+ clock $REPS 번), 결과는 $OUT/"
start=$(date +%s)
done_runs=0
failed=0
read -r -a ipc_list <<< "$IPCS"

run_one() {   # ipc size clients inflight experiment rep csv
    local args=(-c "$3" -s "$2" -n "$(count_for "$2")" -w "$(warm_for "$2")" -f "$4" -e "$5" -r "$6" -C "$commit" -o "$7")
    if ! "bin/bench_$1" "${args[@]}"; then
        echo "실패: $1 size=$2 clients=$3 inflight=$4 rep=$6. 자원을 치우고 한 번 더 한다" >&2
        make -s ipcclean >/dev/null
        if ! "bin/bench_$1" "${args[@]}"; then
            echo "다시 실패: 건너뛴다" >&2
            failed=$((failed + 1))
            make -s ipcclean >/dev/null
        fi
    fi
    done_runs=$((done_runs + 1))
}

for rep in $(seq "$REPS"); do
    bin/bench_pipe -K -n 1000000 -r "$rep" -C "$commit" -o "$OUT/bench_clock.csv"
    # 반복마다 IPC 순서를 돌려 시간에 따른 변화가 한 IPC 에만 몰리지 않게 한다
    k=$(( (rep - 1) % ${#ipc_list[@]} ))
    order=("${ipc_list[@]:k}" "${ipc_list[@]:0:k}")
    for size in $SIZES; do
        for c in $CLIENTS; do
            for ipc in "${order[@]}"; do
                run_one "$ipc" "$size" "$c" 1 latency "$rep" "$OUT/bench_latency.csv"
                run_one "$ipc" "$size" "$c" "$INFLIGHT" throughput "$rep" "$OUT/bench_throughput.csv"
            done
        done
    done
    echo "반복 $rep/$REPS 끝: $done_runs/$n_runs, $(( $(date +%s) - start ))초"
done

elapsed=$(( $(date +%s) - start ))
echo "측정 끝: ${elapsed}초, 실패 $failed 번"
{
    export LC_ALL=C
    echo; echo "## 측정 끝: $(date -Iseconds), ${elapsed}초, 실행 $done_runs 번, 실패 $failed 번"
    echo; echo "## 측정 끝 때 CPU 를 쓰는 프로세스"; cpu_snapshot
} >> "$OUT/bench_env.txt"
team=$(sed -n 's/^TEAM_NO ?= *//p' Makefile)
left=""
for y in 0 1 2 3 4 5 6 7 8 9; do
    key=$(printf '0x%08x' $((60000 + team * 10 + y)))
    ipcs -q -m -s 2>/dev/null | grep -qi "^$key" && left="$left $key"
done
left="$left $(ls /tmp/osproject_"${team}"_* 2>/dev/null | tr '\n' ' ')"
left=$(echo $left)                          # 공백 정리
echo "남은 IPC 자원과 파일: ${left:-없음}"
[ "$failed" -eq 0 ]
