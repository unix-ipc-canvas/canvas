#!/usr/bin/env bash
# 동기화 시연: 동기화 하나를 뺀 서버로 같은 일을 시켜, 그 동기화가 무엇을 막고 있었는지 보인다.
#
#   scripts/race-demo.sh            네 가지를 차례로 (약 2분). 결과 표를 화면에 쓴다
#   OUT=표.md scripts/race-demo.sh  표를 파일로도
#
# 뺀 것은 시연용 빌드(build/demo-*/)에만 있다. 평소 빌드(bin/)는 그대로다.
#   1. 캔버스 락       서버 -l none (실행 옵션)                   같은 칸을 동시에 더해 값이 사라진다(갱신 유실)
#   2. 타일 락 순서    DEMO_UNORDERED_LOCKS: 읽기만 타일을 내림차순으로 잡는다     쓰기(오름차순)와 서로 기다린다(교착)
#   3. shm 링 MUTEX    DEMO_NO_SHM_MUTEX: 요청 링에 쓸 때 MUTEX 없이 빈 칸만 얻는다  두 클라이언트가 같은 칸에 쓴다
#   4. pipe 응답 mutex DEMO_NO_RESP_MUTEX: 워커들이 같은 응답 FIFO 에 잠금 없이 쓴다 큰 응답의 바이트가 섞인다
# 같은 조건을 평소 빌드로도 돌려 나란히 놓는다. 끝나지 않는 실행은 LIMIT 초 뒤에 강제로 끝내고 자원을 지운다.
set -u
cd "$(dirname "$0")/.."
LIMIT=${LIMIT:-20}
OUT=${OUT:-}
W=$(mktemp -d)
KILL_PIDS=""
if ls /tmp/osproject_2_* >/dev/null 2>&1; then
    echo "우리 서버나 클라이언트가 떠 있거나 남은 파일이 있다(/tmp/osproject_2_*). 끝낸 뒤 다시 실행한다." >&2
    exit 1
fi
trap 'cleanup; rm -rf "$W"' EXIT

# 우리 팀 key(0xea74~0xea7d)의 자원과 파일만 지운다
cleanup() {
    [ -n "${KILL_PIDS:-}" ] && kill -9 $KILL_PIDS 2>/dev/null
    KILL_PIDS=""
    sleep 0.2
    for k in 4 5 6 7 8 9 a b c d; do
        ipcrm -M 0x0000ea7$k 2>/dev/null; ipcrm -S 0x0000ea7$k 2>/dev/null; ipcrm -Q 0x0000ea7$k 2>/dev/null
    done
    rm -f /tmp/osproject_2_*
}

build() {   # build <이름> <-D 매크로>
    make -s BIN=build/demo-$1/bin OBJ=build/demo-$1/obj EXTRA_CFLAGS="-D$2" >"$W/make.log" 2>&1 || { cat "$W/make.log"; exit 1; }
}

# run <서버 실행 파일> <서버 옵션> <클라이언트 실행 파일> <클라이언트 수> <클라이언트 옵션...>
# 결과: R_DONE(제한 안에 끝난 클라이언트 수) R_SEC(걸린 초) R_SRV(서버 마지막 줄들) R_STUCK(futex 에서 기다리던 서버 스레드 수)
run() {
    local srv=$1 sopt=$2 cli=$3 n=$4; shift 4
    cleanup
    # shellcheck disable=SC2086
    $srv $sopt -p "$W/c.pgm" >"$W/srv.log" 2>&1 &
    local sp=$! t0 pids="" i
    KILL_PIDS=$sp
    sleep 0.3
    t0=$(date +%s.%N)
    for i in $(seq 0 $((n - 1))); do
        timeout -s KILL "$LIMIT" "$cli" -i "$i" "$@" >"$W/c$i.log" 2>&1 &
        pids="$pids $!"; KILL_PIDS="$KILL_PIDS $!"
    done
    R_DONE=0
    for i in $pids; do wait "$i" && R_DONE=$((R_DONE + 1)); done
    R_SEC=$(awk -v a="$t0" -v b="$(date +%s.%N)" 'BEGIN{printf "%.1f", b - a}')
    R_STUCK=$(for t in /proc/$sp/task/*; do cat "$t/wchan" 2>/dev/null; echo; done | grep -c futex)
    kill -INT "$sp" 2>/dev/null
    for _ in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$sp" 2>/dev/null || break; sleep 0.3; done
    if kill -0 "$sp" 2>/dev/null; then kill -INT "$sp"; sleep 1; fi
    if kill -0 "$sp" 2>/dev/null; then kill -9 "$sp"; R_SRV="(서버가 SIGINT 두 번에도 끝나지 않아 SIGKILL)"; else R_SRV=""; fi
    wait "$sp" 2>/dev/null
    R_SUM=$(grep -o 'checksum [0-9a-f]*' "$W/srv.log" | tail -1)
    R_CNT=$(grep -oE 'DRAW [0-9]+, SNAPSHOT [0-9]+, 오류 응답 [0-9]+' "$W/srv.log" | tail -1)
    cleanup
}

rows=()
row() { rows+=("$1"); printf '%s\n' "$1"; }

echo "빌드: 평소 빌드와 시연용 빌드 3개"
make -s >/dev/null || exit 1
build unordered-locks DEMO_UNORDERED_LOCKS
build no-shm-mutex DEMO_NO_SHM_MUTEX
build no-resp-mutex DEMO_NO_RESP_MUTEX
echo

row "| 동기화 | 조건 | 동기화 있음 | 뺀 것 |"
row "| --- | --- | --- | --- |"

# 1. 캔버스 락: 클라이언트 8개가 캔버스 전체 사각형만 300개씩. 3번
want=$(bin/verify -c 8 -n 300 -m 100 -r 1024 2>/dev/null)
ok_n=0; bad_n=0; sums=""
for _ in 1 2 3; do
    run bin/server_shm "-l tile -w 8" bin/client_shm 8 -n 300 -w 0 -m 100 -r 1024 -f 16
    [ "$R_SUM" = "$want" ] && ok_n=$((ok_n + 1))
    run bin/server_shm "-l none -w 8" bin/client_shm 8 -n 300 -w 0 -m 100 -r 1024 -f 16
    [ "$R_SUM" != "$want" ] && { bad_n=$((bad_n + 1)); sums="$sums ${R_SUM#checksum }"; }
done
ndiff=$(echo $sums | tr ' ' '\n' | sort -u | grep -c .)
row "| 캔버스 타일 락 (\`-l tile\` → \`-l none\`) | shm, 클라이언트 8, 워커 8, 캔버스 전체 DRAW 2,400개, 3번 | 3번 중 $ok_n번 정답 체크섬 | 3번 중 $bad_n번 어긋남(서로 다른 값 ${ndiff}개). 같은 칸을 두 워커가 동시에 읽고 더해 한쪽이 사라졌다 |"

# 2. 타일 락 순서: 큰 DRAW(오름차순)와 캔버스 전체 SNAPSHOT(시연 빌드는 내림차순)이 섞임
run bin/server_shm "-l tile -t 16 -w 8" bin/client_shm 4 -n 2000 -m 50 -r 512 -S 1024 -f 8
a="${R_DONE}/4 클라이언트가 ${R_SEC}초에 끝남"
run build/demo-unordered-locks/bin/server_shm "-l tile -t 16 -w 8" build/demo-unordered-locks/bin/client_shm 4 -n 2000 -m 50 -r 512 -S 1024 -f 8
row "| 타일 락을 잡는 순서 (모두 오름차순 → 읽기만 내림차순) | shm, \`-l tile -t 16\`, 클라이언트 4, DRAW 512×512와 SNAPSHOT 1024×1024 반반 | $a | ${R_DONE}/4 클라이언트가 ${LIMIT}초 안에 끝나지 않음. 서버 스레드 ${R_STUCK}개가 락(futex)에서 서로를 기다림(교착) $R_SRV |"

# 3. shm 요청 링 MUTEX
want=$(bin/verify -c 8 -n 20000 2>/dev/null)
run bin/server_shm "-w 8" bin/client_shm 8 -n 20000 -f 16
s="${R_DONE}/8 클라이언트가 ${R_SEC}초에 끝남, $([ "$R_SUM" = "$want" ] && echo 정답 체크섬 || echo "체크섬 다름")"
run build/demo-no-shm-mutex/bin/server_shm "-w 8" build/demo-no-shm-mutex/bin/client_shm 8 -n 20000 -f 16
row "| shm 요청 링의 MUTEX 세마포어 | 클라이언트 8, 요청 20,000개씩(DRAW 90%), \`-f 16\` | $s | ${R_DONE}/8 클라이언트가 ${LIMIT}초 안에 끝나지 않음. 서버가 처리한 것 ${R_CNT:-없음}. 두 클라이언트가 같은 칸에 덮어써 요청이 사라지거나 깨졌다 |"

# 4. pipe 응답 mutex
run bin/server_pipe "-w 8" bin/client_pipe 8 -n 2000 -m 0 -S 1024 -f 16
s="${R_DONE}/8 클라이언트가 ${R_SEC}초에 끝남 (${R_CNT})"
run build/demo-no-resp-mutex/bin/server_pipe "-w 8" build/demo-no-resp-mutex/bin/client_pipe 8 -n 2000 -m 0 -S 1024 -f 16
row "| pipe 응답 FIFO의 송신 mutex | 클라이언트 8, 캔버스 전체 SNAPSHOT(1MB 응답) 2,000개씩, \`-f 16\` | $s | ${R_DONE}/8 클라이언트가 ${LIMIT}초 안에 끝나지 않음. 서버가 처리한 것 ${R_CNT:-없음}. 워커 둘이 한 클라이언트에 1MB 응답을 동시에 써 바이트가 섞였고, 받는 쪽이 메시지 경계를 잃었다 |"

if [ -n "$OUT" ]; then
    { echo "# 동기화 시연 결과"; echo; echo "\`scripts/race-demo.sh\` 가 $(date '+%F %T')에 $(hostname)에서 만든 표 (커밋 $(git rev-parse --short HEAD 2>/dev/null))."; echo; printf '%s\n' "${rows[@]}"; } > "$OUT"
fi
