#!/usr/bin/env bash
# 2단계 측정. 실험과 조건의 원본은 docs/measurement/README.md#2단계-실험 이다.
#
#   scripts/app-bench.sh                                   전체 (EXPS="type thr scale lock target"), 반복 REPS=30
#   REPS=2 EXPS="scale" OUT=/tmp/x scripts/app-bench.sh     일부만, 다른 곳에
#   EXPS="imp-mq" ... 원인 확인·설계 개선 실험은 COND_<실험> 환경 변수로 조건을 준다 (아래 conds_for)
#
# 실행 하나: 서버를 새로 띄우고 → 실행기가 클라이언트 N개를 워밍업시킨 뒤 함께 시작 → 끝나면 검증기로 정답을 만들어
# 두고 서버를 SIGINT 로 끝낸다. 서버는 정답과 그림판 체크섬을 비교해 CSV 서버 줄의 checksum_ok 에 남긴다.
# 결과: $OUT/<실험>.csv (실행마다 role=clients 줄과 role=server 줄), $OUT/app_env.txt
set -u
cd "$(dirname "$0")/.."
REPS=${REPS:-30}
EXPS=${EXPS:-"type thr scale lock target"}
IPCS=${IPCS:-"pipe mq shm"}
OUT=${OUT:-results/app}
TEAM_NO=$(sed -n 's/^TEAM_NO ?= *//p' Makefile)

if [ -n "$(git status --porcelain -- src include Makefile 2>/dev/null)" ] && [ -z "${ALLOW_DIRTY:-}" ]; then
    echo "커밋하지 않은 코드 변경이 있다. 커밋한 코드로만 잰다 (시험 삼아 돌릴 때는 ALLOW_DIRTY=1)" >&2
    exit 1
fi
export BENCH_COMMIT=${BENCH_COMMIT:-$(git rev-parse --short HEAD)}
make -s app > /dev/null || { echo "빌드 실패" >&2; exit 1; }
mkdir -p "$OUT"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# 실험의 조건 목록. 한 줄 = "서버옵션|클라이언트 수|클라이언트옵션|측정초|환경(서버에 줄 VAR=값, 공백으로 여럿)"
conds_for() {
    case $1 in
    type)   for spec in "-m 100 -r 16" "-m 0 -S 16" "-m 0 -S 64" "-m 0 -S 128" "-m 0 -S 256"; do echo "-K 1|1|$spec -f 1|1|"; done ;;
    thr)    echo "-K 1|1|-f 16|1|" ;;
    scale)  for m in 90 100; do for n in 1 4 16 64 256 1024; do d=2; [ "$n" -ge 256 ] && d=3; echo "-K 8|$n|-m $m -f 1|$d|"; done; done ;;
    lock)   for l in rw room; do for n in 16 64 256; do echo "-K 1 -l $l|$n|-f 1|2|"; done; done ;;
    target) echo "-K 8|1024|-H 67 -f 4|3|" ;;
    *)      v="COND_${1//-/_}"; [ -n "${!v:-}" ] && printf '%s\n' "${!v}" | tr ';' '\n' ;;
    esac
}

# 옵션 문자열에서 값 하나 (없으면 기본값)
optval() { local o=$1 d=$2; shift 2; local a=($*) i; for ((i = 0; i < ${#a[@]}; i++)); do [ "${a[i]}" = "$o" ] && { echo "${a[i+1]}"; return; }; done; echo "$d"; }

our_left() {
    local l="" y key
    for y in 0 1 2 3 4 5 6 7 8 9; do
        key=$(printf '0x%08x' $((60000 + TEAM_NO * 10 + y)))
        ipcs -q -m -s 2>/dev/null | grep -qi "^$key" && l="$l $key"
    done
    ls -d /tmp/osproject_"${TEAM_NO}"_* > /dev/null 2>&1 && l="$l /tmp/osproject_${TEAM_NO}_*"
    echo "$l"
}

record_env() {
    {
        echo "=== $(date -Iseconds) commit $BENCH_COMMIT, EXPS=\"$EXPS\", REPS=$REPS, IPCS=\"$IPCS\""
        LC_ALL=C lscpu | grep -E 'Model name|^CPU\(s\)|Thread|Core|Socket|MHz'
        LC_ALL=C free -h | head -2
        . /etc/os-release && echo "$PRETTY_NAME"; uname -r
        gcc --version | head -1
        echo "virt: $(systemd-detect-virt 2>/dev/null || true)"
        echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)"
        echo "msgmax $(cat /proc/sys/kernel/msgmax) msgmnb $(cat /proc/sys/kernel/msgmnb) msgmni $(cat /proc/sys/kernel/msgmni) sem $(cat /proc/sys/kernel/sem)"
        echo "pipe-max-size $(cat /proc/sys/fs/pipe-max-size) pipe-user-pages-soft $(cat /proc/sys/fs/pipe-user-pages-soft) ulimit-n $(ulimit -n)/$(ulimit -Hn)"
        echo "load $(cat /proc/loadavg)"
        LC_ALL=C top -bn1 -o %CPU | sed -n '7,14p'
    } >> "$OUT/app_env.txt"
}

fails=0; runs=0
run_one() {
    local exp=$1 ipc=$2 rep=$3 sopts=$4 n=$5 copts=$6 dur=$7 senv=$8
    local rooms lock workers tile warm
    rooms=$(optval -K 8 $sopts); lock=$(optval -l rw $sopts); workers=$(optval -w 4 $sopts); tile=$(optval -t 64 $sopts)
    warm=200; [ "$n" -ge 256 ] && warm=50
    export OSP_IPC=$ipc OSP_CLIENTS=$n OSP_WORKERS=$workers OSP_LOCK=$lock OSP_TILE=$tile
    export OSP_INFLIGHT=$(optval -f 1 $copts) OSP_RATE=$(optval -H 0 $copts) OSP_DRAW_PCT=$(optval -m 90 $copts)
    export OSP_RECT=$(optval -r 16 $copts) OSP_SNAP=$(optval -S 256 $copts) OSP_ENV_NOTE=${senv// /;}
    rm -f "$tmp/expect" "$tmp/man"
    env $senv "bin/server_$ipc" $sopts -p - -O "$OUT/$exp.csv" -e "$exp" -R "$rep" -X "$tmp/expect" > "$tmp/srv.out" 2> "$tmp/srv.err" &
    local sp=$! ok=0
    for _ in $(seq 200); do grep -q '시작' "$tmp/srv.err" 2>/dev/null && { ok=1; break; }; kill -0 $sp 2>/dev/null || break; sleep 0.05; done
    runs=$((runs + 1))
    if [ $ok -eq 0 ]; then
        echo "  실패: 서버가 뜨지 않음 $(head -c 200 "$tmp/srv.err")"; kill -9 $sp 2>/dev/null; wait $sp 2>/dev/null
        make -s ipcclean > /dev/null 2>&1; fails=$((fails + 1)); return
    fi
    timeout 400 bin/loadgen -N "$n" -K "$rooms" -P $sp -o "$OUT/$exp.csv" -e "$exp" -R "$rep" -M "$tmp/man" -T 300 \
        -- "bin/client_$ipc" $copts -d "$dur" -w $warm > "$tmp/lg.out" 2>&1
    local lrc=$?
    bin/verify -K "$rooms" -M "$tmp/man" > "$tmp/expect" 2> /dev/null
    kill -INT $sp
    for _ in $(seq 400); do kill -0 $sp 2>/dev/null || break; sleep 0.05; done
    if kill -0 $sp 2>/dev/null; then kill -INT $sp; sleep 1; kill -9 $sp 2>/dev/null; fi
    wait $sp; local src=$?
    local same; grep -q '검증기와 비교: 같음' "$tmp/srv.out" && same=같음 || same=다름
    local line; line=$(sed -n '2,3p' "$tmp/lg.out" | tr -s ' ' | tr '\n' ' ' | cut -c1-150)
    if [ $lrc -ne 0 ] || [ $src -ne 0 ] || [ "$same" != 같음 ]; then
        fails=$((fails + 1))
        echo "  실패: 실행기 $lrc 서버 $src 체크섬 $same | $(head -c 300 "$tmp/lg.out")"
        make -s ipcclean > /dev/null 2>&1
    fi
    printf '  [%s %d/%d] %-4s N=%-4s %-22s %s| 체크섬 %s\n' "$exp" "$rep" "$REPS" "$ipc" "$n" "$copts${senv:+ $senv}" "$line" "$same"
}

record_env
start=$(date +%s)
echo "측정 시작: 커밋 $BENCH_COMMIT, 실험 [$EXPS], 반복 $REPS, 결과 $OUT/"
for exp in $EXPS; do
    mapfile -t CONDS < <(conds_for "$exp")
    [ ${#CONDS[@]} -gt 0 ] || { echo "실험 $exp 의 조건이 없다" >&2; continue; }
    echo "== $exp: 조건 ${#CONDS[@]}개 × IPC 3개 × 반복 $REPS"
    for ((rep = 1; rep <= REPS; rep++)); do
        # 반복마다 IPC 순서를 돌린다 (pipe mq shm → mq shm pipe → shm pipe mq)
        read -r -a order <<< "$IPCS"
        k=$(( (rep - 1) % ${#order[@]} )); order=("${order[@]:k}" "${order[@]:0:k}")
        for c in "${CONDS[@]}"; do
            IFS='|' read -r sopts n copts dur senv <<< "$c"
            for ipc in "${order[@]}"; do run_one "$exp" "$ipc" "$rep" "$sopts" "$n" "$copts" "$dur" "$senv"; done
        done
        echo "반복 $rep/$REPS 끝 ($exp): 실행 $runs번, 실패 $fails번, $(( $(date +%s) - start ))초"
    done
done
record_env
left=$(our_left)
echo "측정 끝: $(( $(date +%s) - start ))초, 실행 $runs번, 실패 $fails번"
echo "남은 IPC 자원과 파일: ${left:-없음}"
[ $fails -eq 0 ]
