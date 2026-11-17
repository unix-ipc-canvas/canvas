#!/usr/bin/env bash
# 전송 계층 시험. 세 IPC 버전에 같은 시나리오를 돌린다. make test 의 T-04 가 부른다.
# 항목과 판정 기준의 원본은 docs/verification/README.md 다.
#
#   scripts/test-transport.sh            bin/tptest_* 가 있는 IPC 모두
#   scripts/test-transport.sh shm mq     고른 IPC 만
#
# bin/tptest_<ipc> 가 있어야 한다 (make). 우리 서버가 떠 있지 않을 때 돌린다.
# 새니타이저로 돌리려면: make clean && make EXTRA_CFLAGS='-fsanitize=address,undefined' && scripts/test-transport.sh
set -u
cd "$(dirname "$0")/.."

TEAM_NO=$(sed -n 's/^TEAM_NO ?= *//p' Makefile)
KEYS=$(for y in 0 1 2 3 4 5 6 7 8 9; do printf '0x%08x|' $((60000 + TEAM_NO * 10 + y)); done)
KEYS=${KEYS%|}
MAXP=$((32 + 1024 * 1024))                   # MAX_PAYLOAD: 서버 시각 32B + 캔버스 1MB

tmp=$(mktemp -d)
bg=()                                        # 이 스크립트가 띄운 프로세스. 끝날 때 남은 것을 거둔다
cleanup() {
    for p in "${bg[@]}"; do kill -9 "$p" 2>/dev/null; done
    wait 2>/dev/null
    rm -rf "$tmp"
}
trap cleanup EXIT

pass=0; fail=0
ok() { echo "PASS  $ipc  $1"; pass=$((pass + 1)); }
ng() { echo "FAIL  $ipc  $1"; fail=$((fail + 1)); }

sysv_left() { ipcs -q -m -s 2>/dev/null | grep -iE "^($KEYS) " | awk '{print $1}' | sort -u | tr '\n' ' '; }
file_left() { ls /tmp/osproject_"${TEAM_NO}"_* 2>/dev/null | tr '\n' ' '; }
left() { echo "$(sysv_left)$(file_left)"; }

# 서버를 띄우고 "ready" 를 기다린다
start_server() {
    "$B" server "$1" > "$tmp/srv.out" 2>&1 &
    SRV=$!
    bg+=("$SRV")
    for _ in $(seq 200); do
        grep -q '^ready' "$tmp/srv.out" 2>/dev/null && return 0
        kill -0 "$SRV" 2>/dev/null || return 1
        sleep 0.05
    done
    return 1
}
stop_server() {
    kill -TERM "$SRV" 2>/dev/null
    if ! wait_gone "$SRV" 10; then ng "서버가 SIGTERM 뒤 10초 안에 끝나지 않음"; kill -9 "$SRV" 2>/dev/null; fi
    wait "$SRV" 2>/dev/null
}

# 프로세스가 끝날 때까지 최대 $2 초 기다린다. 끝나면 0
wait_gone() {
    local i
    for i in $(seq $(($2 * 20))); do
        kill -0 "$1" 2>/dev/null || return 0
        sleep 0.05
    done
    return 1
}

# 클라이언트를 뒤에서 띄운다: client_bg {client|snap} 번호 크기 요청수 동시요청수. CL 에 pid
client_bg() { "$B" "$@" > "$tmp/c$2.out" 2>&1 & CL=$!; bg+=("$CL"); }

run_ipc() {
    B=bin/tptest_$ipc
    [ -x "$B" ] || { ng "$B 가 없다 (make 를 먼저)"; return; }
    local l
    l=$(left)
    if [ -n "$l" ]; then
        ng "시작 전에 우리 자원이 남아 있다: $l (서버가 떠 있으면 끄고, 아니면 make ipcclean)"
        return
    fi

    # 한 서버가 떠 있는 동안 PIPE_BUF 이하 요청과 넘는 요청을 섞지 않는다 (transport.h 의 약속).
    # 그래서 [A] 는 경우마다 서버를 새로 띄우고, [B]~[D] 는 작은 요청(64B·1KB echo, 8B snap)만 쓴다.

    # [A] 클라이언트 8개, 워커 8개. 같은 클라이언트로 가는 응답이 여러 워커에서 동시에 나간다.
    #     echo 는 요청과 응답이 같은 크기, snap 은 응용의 SNAPSHOT 처럼 요청은 작고 응답만 크다.
    for spec in "client 64 300" "client 65536 100" "client $MAXP 15" "snap $MAXP 15"; do
        set -- $spec
        if ! start_server 8; then ng "A 서버를 띄우지 못함: $(head -c 200 "$tmp/srv.out")"; return; fi
        local pids=() bad=0 id
        for id in 0 1 2 3 4 5 6 7; do client_bg "$1" "$id" "$2" "$3" 8; pids+=("$CL"); done
        for p in "${pids[@]}"; do
            if wait_gone "$p" 60; then wait "$p" || bad=$((bad + 1)); else kill -9 "$p"; bad=$((bad + 1)); fi
        done
        stop_server
        if [ "$bad" -eq 0 ]; then ok "A $1 응답 $2B 클라이언트 8개: 내용 섞임 없음"
        else ng "A $1 응답 $2B: 클라이언트 $bad 개 실패. $(cat "$tmp"/c*.out | grep -v 'bad=0 ' | head -3 | tr '\n' ' ')"; fi
        l=$(left); [ -z "$l" ] && ok "A $1 응답 $2B 뒤 자원 정리" || ng "A $1 응답 $2B 뒤 남은 자원: $l"
    done

    # [B] 서버 없이 클라이언트, 서버 중복
    local r
    r=$("$B" open 0)
    [ "$r" = "open ECONNREFUSED" ] && ok "B 서버 없음 → ECONNREFUSED" || ng "B 서버 없음 → $r"
    if ! start_server 2; then ng "B 서버를 띄우지 못함"; return; fi
    r=$(timeout 10 "$B" server 2 2>&1 | head -1)
    case "$r" in server_open\ EADDRINUSE*) ok "B 두 번째 서버 → EADDRINUSE" ;; *) ng "B 두 번째 서버 → $r" ;; esac
    timeout 30 "$B" client 0 1024 200 4 > /dev/null 2>&1 && ok "B 첫 서버는 계속 동작" || ng "B 두 번째 서버 뒤 첫 서버가 응답하지 않음"

    # [C] 같은 번호의 클라이언트가 실행 중
    "$B" open 3 2 > "$tmp/hold.out" & local H=$!; bg+=("$H")
    sleep 0.5
    r=$("$B" open 3)
    [ "$r" = "open EADDRINUSE" ] && ok "C 같은 번호 → EADDRINUSE" || ng "C 같은 번호 → $r"
    wait "$H"

    # [D] 1MB 응답을 받던 클라이언트를 kill -9. 다른 클라이언트와 같은 번호의 새 클라이언트는 영향을 받지 않는다.
    #     요청은 작게(snap) 보낸다. pipe 버전은 큰 요청을 쓰던 클라이언트가 죽으면 요청 FIFO 가 깨지는 한계가 있다(설계 명세).
    client_bg snap 5 1048576 100000 4; local K=$CL
    client_bg snap 6 65536 3000 4; local O=$CL
    sleep 0.7
    kill -9 "$K"; wait "$K" 2>/dev/null
    if wait_gone "$O" 60 && wait "$O"; then ok "D kill -9 뒤 다른 클라이언트 완료"
    else kill -9 "$O" 2>/dev/null; ng "D 다른 클라이언트 실패: $(cat "$tmp/c6.out" | tr '\n' ' ')"; fi
    timeout 60 "$B" snap 5 1048576 30 2 > "$tmp/c5.out" 2>&1 && ok "D 같은 번호로 다시 접속 ($(grep -o 'stale=[0-9]*' "$tmp/c5.out"))" \
        || ng "D 같은 번호 재접속 실패: $(tr '\n' ' ' < "$tmp/c5.out")"
    timeout 60 "$B" client 1 64 2000 8 > /dev/null 2>&1 && ok "D 그 뒤 작은 메시지" || ng "D 그 뒤 작은 메시지 실패"
    stop_server
    l=$(left); [ -z "$l" ] && ok "D 뒤 자원 정리" || ng "D 뒤 남은 자원: $l"

    # [E] 처리 중에 서버가 SIGINT 로 끝난다. 클라이언트는 멈추지 않고 끝나며 자원이 남지 않는다
    if ! start_server 4; then ng "E 서버를 띄우지 못함"; return; fi
    local pids=() alive=0
    for id in 0 1 2; do client_bg client "$id" 1048576 100000 4; pids+=("$CL"); done
    sleep 0.7
    kill -INT "$SRV"
    if wait_gone "$SRV" 10; then wait "$SRV" 2>/dev/null; ok "E 큰 요청을 받는 중에도 서버가 SIGINT 로 끝남"
    else kill -9 "$SRV"; wait "$SRV" 2>/dev/null; ng "E 서버가 SIGINT 뒤 10초 안에 끝나지 않음"; fi
    for p in "${pids[@]}"; do wait_gone "$p" 10 || { alive=$((alive + 1)); kill -9 "$p"; }; done
    [ "$alive" -eq 0 ] && ok "E 서버 SIGINT 뒤 클라이언트 모두 끝남" || ng "E 서버 SIGINT 뒤 클라이언트 $alive 개가 멈춤"
    l=$(left); [ -z "$l" ] && ok "E SIGINT 뒤 자원 정리" || ng "E SIGINT 뒤 남은 자원: $l"

    # [F] 처리 중에 서버가 kill -9 로 죽는다. 클라이언트가 알아채고 끝나며, mq·shm 은 남은 System V 자원을 지운다
    if ! start_server 4; then ng "F 서버를 띄우지 못함"; return; fi
    pids=(); alive=0
    for id in 0 1 2; do client_bg client "$id" 65536 1000000 4; pids+=("$CL"); done
    sleep 0.7
    kill -9 "$SRV"; wait "$SRV" 2>/dev/null
    for p in "${pids[@]}"; do wait_gone "$p" 10 || { alive=$((alive + 1)); kill -9 "$p"; }; done
    [ "$alive" -eq 0 ] && ok "F 서버 kill -9 뒤 클라이언트 모두 끝남" || ng "F 서버 kill -9 뒤 클라이언트 $alive 개가 멈춤"
    sleep 0.3
    l=$(sysv_left); [ -z "$l" ] && ok "F System V 자원은 클라이언트가 정리" || ng "F System V 자원 남음: $l"
    if start_server 2 && timeout 30 "$B" client 0 1024 500 4 > /dev/null 2>&1; then ok "F 새 서버가 남은 것을 치우고 동작"
    else ng "F 새 서버 실패: $(head -c 200 "$tmp/srv.out")"; fi
    stop_server
    l=$(left); [ -z "$l" ] && ok "F 뒤 자원 정리" || ng "F 뒤 남은 자원: $l"

    # 실패로 남은 것이 있으면 다음 IPC 를 위해 치운다 (우리가 방금 만든 것만 남아 있다)
    [ -n "$(left)" ] && make -s ipcclean > /dev/null
}

if [ $# -gt 0 ]; then ipc_list=("$@")
else ipc_list=(); for i in pipe mq shm; do [ -x "bin/tptest_$i" ] && ipc_list+=("$i"); done; fi
[ ${#ipc_list[@]} -gt 0 ] || { echo "bin/tptest_* 가 없다. make 를 먼저 실행한다" >&2; exit 1; }

for ipc in "${ipc_list[@]}"; do run_ipc; done
echo "---- 전송 계층 시험: PASS $pass / FAIL $fail"
[ "$fail" -eq 0 ]
