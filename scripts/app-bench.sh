#!/usr/bin/env bash
# 2단계 측정. 서버와 클라이언트를 띄워 응용의 처리량과 지연을 잰다. 측정용 컴퓨터에서만 돌린다.
# 실험 목록과 기준 조건의 원본은 docs/measurement/README.md 의 "2단계 실험" 과 docs/records/prediction.md 의 3.1절이다.
#
#   scripts/app-bench.sh                                    전체 (반복 REPS 번, 기본 20. 약 26분)
#   REPS=1 EXPS="base clients" OUT=/tmp/x scripts/app-bench.sh   일부만, 다른 곳에
#
# 결과 (OUT, 기본 results/)
#   app_env.txt      측정 환경
#   app_<실험>.csv   실행마다 클라이언트 줄(role=client) 여러 개와 서버 줄(role=server) 하나. 반복 번호는 run 칸
# 기준 조건: DRAW 90%, -r 16, -S 256, 클라이언트 4, 워커 4, -l global, -t 64, 클라이언트마다 20,000개.
# 처리량은 -f 16, 지연은 -f 1. 실험마다 요인 하나만 바꾼다.
# 모든 클라이언트는 -G 로 워밍업 뒤 멈춰 있다가 SIGUSR2 로 함께 측정을 시작한다.
# 한 실행의 클라이언트가 하나라도 실패하면 그 실행의 줄은 남기지 않고 한 번 더 한다.
set -u
cd "$(dirname "$0")/.."

REPS=${REPS:-20}
IPCS=${IPCS:-"pipe mq shm"}
EXPS=${EXPS:-"base type draw clients workers lock tile"}
WORKERS=${WORKERS:-"1 2 4 8"}       # workers 실험의 워커 수 (원인 확인 실험에서 바꾼다)
OUT=${OUT:-results}
RUN0=${RUN0:-0}                     # CSV 의 반복 번호(run)를 이만큼 밀어 적는다. 반복을 나눠 부를 때 (exp-stage2.sh pipesz)

commit=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
if ! git diff --quiet HEAD -- src include Makefile scripts 2>/dev/null; then
    if [ "${ALLOW_DIRTY:-0}" != 1 ]; then
        echo "커밋하지 않은 코드 변경이 있다. 커밋한 뒤에 잰다 (무시하려면 ALLOW_DIRTY=1)" >&2
        exit 1
    fi
    commit="$commit-dirty"
fi

mkdir -p "$OUT"
make -s clean >/dev/null && make -s >/dev/null || { echo "빌드 실패" >&2; exit 1; }
for i in $IPCS; do
    for b in server client; do [ -x "bin/${b}_$i" ] || { echo "bin/${b}_$i 가 없다" >&2; exit 1; }; done
done
make -s ipcclean >/dev/null
TEAM_NO=$(sed -n 's/^TEAM_NO ?= *//p' Makefile)
tmp=$(mktemp -d)
srv=""
trap '[ -n "$srv" ] && kill -INT $srv 2>/dev/null; pkill -INT -f "^bin/client_" 2>/dev/null; rm -rf "$tmp"' EXIT

cpu_snapshot() {
    uptime
    top -b -n 2 -d 1 -w 200 | awk '/^top -/{n++} n == 2' | sed -n '1,5p;7,19p'
}

{
    export LC_ALL=C
    echo "# scripts/app-bench.sh 가 측정을 시작할 때 기록한 환경"
    echo "date: $(date -Iseconds)"
    echo "commit: $commit"
    echo "reps: $REPS (run 번호 $((RUN0 + 1))부터), ipcs: $IPCS, experiments: $EXPS, workers: $WORKERS"
    echo
    echo "## lscpu"; lscpu | grep -E 'Model name|^CPU\(s\)|Thread|Core|Socket|MHz'
    echo; echo "## free -h"; free -h | head -2
    echo; echo "## os"; (. /etc/os-release && echo "$PRETTY_NAME"); uname -r
    echo; echo "## gcc"; gcc --version | head -1
    echo; echo "## virtualization"
    if command -v systemd-detect-virt >/dev/null; then systemd-detect-virt || true; else echo "(systemd-detect-virt 없음)"; fi
    echo; echo "## cpufreq governor"; cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort | uniq -c || echo "(없음)"
    echo; echo "## CPU 배치 (taskset 으로 묶었는지)"; taskset -cp $$ 2>/dev/null || echo "(taskset 없음)"
    echo; echo "## IPC 한도"
    echo "msgmax: $(cat /proc/sys/kernel/msgmax)"
    echo "msgmnb: $(cat /proc/sys/kernel/msgmnb)"
    echo "PIPE_BUF: $(getconf PIPE_BUF /)"
    echo "pipe-max-size: $(cat /proc/sys/fs/pipe-max-size)"
    echo "ulimit -n: $(ulimit -n)"
    echo; echo "## 전송 계층 환경 변수 (OSP_*, 설계 개선 실험)"
    osp=$(env | grep '^OSP_' | sort); echo "${osp:-(없음)}"
    echo; echo "## 측정 시작 때 CPU 를 쓰는 프로세스"; cpu_snapshot
} > "$OUT/app_env.new"
# 반복을 나눠 부르면(RUN0 > 0) 앞 기록 뒤에 덧붙인다
if [ "$RUN0" -gt 0 ]; then cat "$OUT/app_env.new" >> "$OUT/app_env.txt"; rm -f "$OUT/app_env.new"
else mv "$OUT/app_env.new" "$OUT/app_env.txt"; fi

# SNAPSHOT 만 보내는 실행은 응답이 커서 요청 수와 워밍업을 줄인다 (1단계와 같은 표)
count_for() { if [ "$1" != 0 ] || [ "$2" -le 128 ]; then echo 20000; elif [ "$2" -le 256 ]; then echo 10000; elif [ "$2" -le 512 ]; then echo 4000; else echo 2000; fi; }
warm_for()  { if [ "$1" != 0 ] || [ "$2" -le 128 ]; then echo 500;   elif [ "$2" -le 256 ]; then echo 200;   elif [ "$2" -le 512 ]; then echo 100;  else echo 30; fi; }

# 실험마다 실행할 조건: "클라이언트 동시요청 워커 락 타일 DRAW% 변" 한 줄에 하나
cases_for() {
    case $1 in
        base)    echo "4 16 4 global 64 90 256"; echo "4 1 4 global 64 90 256" ;;
        type)    for f in 1 16; do
                     echo "1 $f 4 global 64 100 256"
                     for s in 128 256 512 1024; do echo "1 $f 4 global 64 0 $s"; done
                 done ;;
        draw)    for c in 1 4; do for m in 70 80 85 90 95 100; do echo "$c 16 4 global 64 $m 256"; done; done ;;
        clients) for c in 1 2 4 8; do echo "$c 16 4 global 64 90 256"; done ;;
        workers) for w in $WORKERS; do echo "8 16 $w global 64 90 256"; done ;;
        lock)    for l in global tile tile-rw; do echo "4 16 4 $l 64 90 256"; done ;;
        tile)    for t in 16 64 256 1024; do echo "4 16 4 tile $t 90 256"; done ;;
        *) echo "모르는 실험: $1" >&2; exit 1 ;;
    esac
}

# 한 실행. 성공하면 0 이고 그 실행의 줄을 결과 CSV 에 덧붙인다
run_once() {   # ipc exp rep clients inflight workers lock tile m s
    local ipc=$1 exp=$2 rep=$3 c=$4 f=$5 w=$6 l=$7 t=$8 m=$9 s=${10}
    local n warm csv="$tmp/run.csv" pids="" k
    n=$(count_for "$m" "$s"); warm=$(warm_for "$m" "$s")
    rm -f "$csv" "$tmp"/c*.err "$tmp/srv.err"   # 앞 실행의 "시작" 을 새 서버의 것으로 읽지 않게
    "bin/server_$ipc" -w "$w" -l "$l" -t "$t" -p "$tmp/canvas.pgm" -O "$csv" -e "$exp" -R "$rep" -C "$commit" \
        > "$tmp/srv.out" 2> "$tmp/srv.err" &
    srv=$!
    # 서버는 채널을 열고 스레드를 모두 만든 뒤 stderr 에 "시작" 을 쓴다
    for _ in $(seq 200); do grep -q '시작' "$tmp/srv.err" && break; kill -0 $srv 2>/dev/null || break; sleep 0.02; done
    grep -q '시작' "$tmp/srv.err" || { echo "  서버가 뜨지 않음: $(head -1 "$tmp/srv.err")" >&2; srv=""; return 1; }
    for ((k = 0; k < c; k++)); do
        "bin/client_$ipc" -i "$k" -n "$n" -w "$warm" -m "$m" -r 16 -S "$s" -f "$f" -G \
            -c "$c" -L "$l" -T "$t" -o "$csv" -e "$exp" -R "$rep" -C "$commit" > /dev/null 2> "$tmp/c$k.err" &
        pids="$pids $!"
    done
    # 모든 클라이언트가 워밍업을 마치면 한꺼번에 시작시킨다
    local ready=0
    for _ in $(seq 1500); do
        ready=$(cat "$tmp"/c*.err 2>/dev/null | grep -c '준비')
        [ "$ready" -eq "$c" ] && break
        sleep 0.02
    done
    local rc=0
    if [ "$ready" -eq "$c" ]; then
        kill -USR2 $pids 2>/dev/null
    else
        echo "  클라이언트 준비 $ready/$c" >&2; kill -INT $pids 2>/dev/null; rc=1
    fi
    for p in $pids; do wait "$p" || rc=1; done
    kill -INT $srv 2>/dev/null; wait $srv || rc=1; srv=""
    if [ $rc -ne 0 ]; then grep -hvE '준비|시작|끝내는 중|로 끝남' "$tmp"/c*.err "$tmp/srv.err" | head -3 >&2; return 1; fi
    local want=$((c + 1)) got
    got=$(($(wc -l < "$csv") - 1))
    [ "$got" -eq "$want" ] || { echo "  CSV 줄 $got, 기대 $want" >&2; return 1; }
    local dst="$OUT/app_$exp.csv"
    if [ -s "$dst" ]; then tail -n +2 "$csv" >> "$dst"; else cp "$csv" "$dst"; fi
}

n_runs=0
for _ in $(seq "$REPS"); do for e in $EXPS; do n_runs=$((n_runs + $(cases_for "$e" | wc -l) * $(echo $IPCS | wc -w))); done; done
echo "측정 시작: 커밋 $commit, 실행 $n_runs 번, 결과는 $OUT/"
start=$(date +%s)
done_runs=0; failed=0
read -r -a ipc_list <<< "$IPCS"
for rep in $(seq "$REPS"); do
    k=$(( (rep - 1) % ${#ipc_list[@]} ))
    order=("${ipc_list[@]:k}" "${ipc_list[@]:0:k}")
    for exp in $EXPS; do
        while read -r c f w l t m s; do
            for ipc in "${order[@]}"; do
                if ! run_once "$ipc" "$exp" "$((RUN0 + rep))" "$c" "$f" "$w" "$l" "$t" "$m" "$s"; then
                    echo "실패: $ipc $exp rep=$((RUN0 + rep)) c=$c f=$f w=$w l=$l t=$t m=$m S=$s. 자원을 치우고 한 번 더 한다" >&2
                    make -s ipcclean >/dev/null
                    if ! run_once "$ipc" "$exp" "$((RUN0 + rep))" "$c" "$f" "$w" "$l" "$t" "$m" "$s"; then
                        echo "다시 실패: 건너뛴다" >&2; failed=$((failed + 1)); make -s ipcclean >/dev/null
                    fi
                fi
                done_runs=$((done_runs + 1))
            done
        done < <(cases_for "$exp")
    done
    echo "반복 $rep/$REPS 끝: $done_runs/$n_runs, $(( $(date +%s) - start ))초"
done

elapsed=$(( $(date +%s) - start ))
echo "측정 끝: ${elapsed}초, 실패 $failed 번"
{
    export LC_ALL=C
    echo; echo "## 측정 끝: $(date -Iseconds), ${elapsed}초, 실행 $done_runs 번, 실패 $failed 번"
    echo; echo "## 측정 끝 때 CPU 를 쓰는 프로세스"; cpu_snapshot
} >> "$OUT/app_env.txt"
left=""
for y in 0 1 2 3 4 5 6 7 8 9; do
    key=$(printf '0x%08x' $((60000 + TEAM_NO * 10 + y)))
    ipcs -q -m -s 2>/dev/null | grep -qi "^$key" && left="$left $key"
done
left=$(echo $left $(ls /tmp/osproject_"${TEAM_NO}"_* 2>/dev/null))
echo "남은 IPC 자원과 파일: ${left:-없음}"
[ "$failed" -eq 0 ]
