#!/usr/bin/env bash
# 데모용 점검: 세 버전마다 서버를 띄우고 자동 그리기 클라이언트 N개(기본 1024)를 함께 돌린 뒤,
# 서버의 그림판별 체크섬이 검증기 정답과 같은지, 서버가 끝난 뒤 남은 IPC 자원이 없는지 보여 준다.
#
#   scripts/demo-check.sh [클라이언트 수] [초] [그림판 수]     예: scripts/demo-check.sh 1024 3 8
#
# make app 을 먼저 한다. 우리 서버가 떠 있지 않을 때 돌린다.
set -u
cd "$(dirname "$0")/.."
N=${1:-1024}
DUR=${2:-3}
K=${3:-8}
TEAM_NO=$(sed -n 's/^TEAM_NO ?= *//p' Makefile)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

our_left() {
    local n=0 y key
    for y in 0 1 2 3 4 5 6 7 8 9; do
        key=$(printf '0x%08x' $((60000 + TEAM_NO * 10 + y)))
        n=$((n + $(ipcs -q -m -s 2>/dev/null | grep -ci "^$key")))
    done
    n=$((n + $(ls -d /tmp/osproject_"${TEAM_NO}"_* 2>/dev/null | wc -l)))
    echo "$n"
}

for ipc in pipe mq shm; do
    printf '\n\033[1m== %s: 서버(그림판 %d개) + 자동 그리기 클라이언트 %d개, %d초\033[0m\n' "$ipc" "$K" "$N" "$DUR"
    "bin/server_$ipc" -K "$K" -p - > "$tmp/srv.out" 2> "$tmp/srv.err" &
    sp=$!
    for _ in $(seq 200); do grep -q "시작 (pid $sp," "$tmp/srv.err" 2>/dev/null && break; sleep 0.05; done
    bin/loadgen -N "$N" -K "$K" -M "$tmp/man" -- "bin/client_$ipc" -d "$DUR" -w 50 2>&1 | sed -n '1,3p'
    kill -INT "$sp"
    wait "$sp"
    bin/verify -K "$K" -M "$tmp/man" > "$tmp/expect" 2> /dev/null
    if diff <(grep '^checksum' "$tmp/srv.out") <(grep '^checksum' "$tmp/expect") > /dev/null; then
        echo "  체크섬: 서버 = 검증기 정답 (그림판 ${K}개 모두 같다 → 경쟁 상태 없음)"
    else
        echo "  체크섬: 서버와 검증기가 다르다"
    fi
    echo "  서버가 끝난 뒤 남은 우리 IPC 자원과 FIFO: $(our_left)개"
done
