#!/usr/bin/env bash
# 요청 하나당 시스템 콜 수: 서버와 클라이언트 하나를 각각 strace -f -c 로 감싸 요청 N개를 보낸 뒤 호출 수를 N 으로 나눈다.
#
#   scripts/syscall-count.sh [요청 수] [클라이언트 옵션...]     예: scripts/syscall-count.sh 20000 -m 100 -f 16
#
# strace 가 느리게 만들므로 시간은 보지 않고 호출 수만 본다. make app 을 먼저 하고, 우리 서버가 떠 있지 않을 때 돌린다.
set -u
cd "$(dirname "$0")/.."
N=${1:-20000}
shift || true
OPTS=${*:--m 100 -f 16}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# strace -c 요약에서 "호출 이름 호출 수" 만 뽑는다 (errors 칸이 비어 있을 수 있어 4번째 칸이 호출 수)
calls() { awk '/^-/ {sep++; next} sep == 1 && NF >= 5 && $4 ~ /^[0-9]+$/ {print $NF, $4}' "$1"; }

echo "요청 $N개, 클라이언트 옵션: $OPTS (그림판 1)"
for ipc in pipe mq shm; do
    strace -f -c -o "$tmp/srv" "bin/server_$ipc" -K 1 > /dev/null 2> "$tmp/srv.err" &
    sp=$!
    for _ in $(seq 400); do grep -q "시작 (pid" "$tmp/srv.err" 2>/dev/null && break; sleep 0.05; done
    strace -f -c -o "$tmp/cli" "bin/client_$ipc" -n "$N" -w 0 $OPTS > /dev/null 2>&1
    kill -INT "$(pgrep -P "$sp" | head -1)" 2>/dev/null
    wait "$sp"
    for side in srv cli; do
        name=서버; [ $side = cli ] && name=클라이언트
        printf '%-4s %-9s' "$ipc" "$name"
        calls "$tmp/$side" | sort -k2,2nr | awk -v n="$N" '{ r = $2 / n; if (r >= 0.05) { printf " %s %.2f", $1, r; t += r } } END { printf "  | 합 %.2f\n", t }'
    done
done
