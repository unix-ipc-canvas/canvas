#!/usr/bin/env bash
# 2단계 서버·클라이언트 시험 (T-10, T-11, T-13~T-18, T-21, T-22, T-30, T-31). verify.sh 가 IPC 버전마다 부른다.
# 항목과 판정 기준의 원본은 docs/verification/README.md 다.
#
#   scripts/test-app.sh pipe     bin/server_pipe, client_pipe, apptest_pipe, verify 로 시험한다
#
# 출력: 항목마다 "PASS  T-xx  설명" 또는 "FAIL  T-xx  설명". 마지막에 T-12 비교용 "SUM <체크섬>" 한 줄.
# 종료 코드는 FAIL 이 있으면 1.
set -u
cd "$(dirname "$0")/.."
ipc=${1:?IPC 버전 (pipe, mq, shm)}
S=bin/server_$ipc; C=bin/client_$ipc; A=bin/apptest_$ipc; V=bin/verify
for b in $S $C $A $V; do [ -x "$b" ] || { echo "FAIL  T-10  $b 가 없다"; exit 1; }; done

TEAM_NO=$(sed -n 's/^TEAM_NO ?= *//p' Makefile)
tmp=$(mktemp -d)
srv_pid=""
cleanup() {
    [ -n "$srv_pid" ] && kill -INT "$srv_pid" 2>/dev/null && wait "$srv_pid" 2>/dev/null
    pkill -f "^$C " 2>/dev/null
    rm -rf "$tmp"
}
trap cleanup EXIT
nfail=0
ok() { echo "PASS  $1  $ipc: $2"; }
ng() { echo "FAIL  $1  $ipc: $2"; nfail=$((nfail + 1)); }

# 서버를 띄우고 "준비" 줄이 나올 때까지 기다린다. 성공 0
start_server() {
    : > "$tmp/srv.err"
    $S "$@" -o "$tmp/canvas.pgm" > "$tmp/srv.out" 2> "$tmp/srv.err" &
    srv_pid=$!
    for _ in $(seq 100); do
        grep -q '준비' "$tmp/srv.err" && return 0
        kill -0 "$srv_pid" 2>/dev/null || { srv_pid=""; return 1; }
        sleep 0.05
    done
    return 1
}
# 서버에 SIGINT 를 보내고 체크섬을 변수 got 에 넣는다. 서버 종료 코드가 0 이 아니면 실패
# (서버가 이 셸의 자식이어야 wait 로 종료 코드를 받으므로 $( ) 안에서 부르지 않는다)
stop_server() {
    kill -INT "$srv_pid" 2>/dev/null
    local rc=0
    timeout 30 tail --pid="$srv_pid" -f /dev/null || rc=124
    wait "$srv_pid" || rc=$?
    srv_pid=""
    got=$(sed -n 's/^checksum //p' "$tmp/srv.out")
    return $rc
}
# 클라이언트 K 개(번호 0..K-1, 기본 시드)를 동시에 돌린다. 모두 0 으로 끝나면 성공
run_clients() {
    local k=$1; shift
    local pids="" rc=0
    for ((i = 0; i < k; i++)); do $C -i "$i" "$@" > "$tmp/cl$i.out" 2> "$tmp/cl$i.err" & pids="$pids $!"; done
    for p in $pids; do wait "$p" || rc=1; done
    return $rc
}
left_resources() {
    local left=""
    for y in 0 1 2 3 4 5 6 7 8 9; do
        key=$(printf '0x%08x' $((60000 + TEAM_NO * 10 + y)))
        ipcs -q -m -s 2>/dev/null | grep -qi "^$key" && left="$left $key"
    done
    echo "$left $(ls /tmp/osproject_"${TEAM_NO}"_* 2>/dev/null | tr '\n' ' ')" | xargs
}

N=2000
# ---- T-10 정합성: 클라이언트 2개와 8개, 락 방식 3가지 ----
sum2=""
for lk in global tile tile-rw; do
    for k in 2 8; do
        w=$([ "$k" = 8 ] && echo 8 || echo 4)
        if ! start_server -w "$w" -l "$lk"; then ng T-10 "서버를 띄우지 못함 (-l $lk)"; continue; fi
        run_clients "$k" -n $N -f 4; crc=$?
        stop_server; src=$?
        want=$($V -c "$k" -n $N 2>/dev/null | sed -n 's/^checksum //p')
        if [ $crc -ne 0 ] || [ $src -ne 0 ]; then
            ng T-10 "-l $lk 클라이언트 $k: 종료 코드 (클라이언트 $crc, 서버 $src) $(cat "$tmp"/cl*.err | head -2)"
        elif [ -n "$got" ] && [ "$got" = "$want" ]; then
            ok T-10 "-l $lk 클라이언트 $k: 체크섬 $got 이 정답과 같다"
        else
            ng T-10 "-l $lk 클라이언트 $k: 체크섬 $got, 정답 $want"
        fi
        [ "$lk" = global ] && [ "$k" = 2 ] && sum2=$got
    done
done

# ---- T-11 잘못된 요청에 오류로 응답하고 캔버스는 그대로 ----
empty=$($V 2>/dev/null | sed -n 's/^checksum //p')
if start_server -w 4; then
    out=$($A bad 0 2>&1); arc=$?
    stop_server
    if [ $arc -eq 0 ] && [ "$got" = "$empty" ]; then ok T-11 "잘못된 요청 $out, 체크섬이 빈 캔버스와 같다"
    else ng T-11 "apptest 종료 코드 $arc ($out), 체크섬 $got (빈 캔버스 $empty)"; fi
else ng T-11 "서버를 띄우지 못함"; fi

# ---- T-13 락을 끄면 어긋날 수 있다 (시연 항목: 어긋나지 않아도 FAIL 이 아니다) ----
# 새니타이저 빌드에서는 SKIP_T13=1 로 건너뛴다. 일부러 만든 경쟁이라 TSan 이 보고하고, 계측 때문에 몇 분이 걸린다
diff_n=0; runs=3
[ "${SKIP_T13:-0}" = 1 ] && runs=0
for ((r = 1; r <= runs; r++)); do
    start_server -w 8 -l none || continue
    run_clients 4 -n 400 -m 100 -r 1024 -f 8 -w 0
    stop_server
    want=$($V -c 4 -n 400 -m 100 -r 1024 2>/dev/null | sed -n 's/^checksum //p')
    [ "$got" != "$want" ] && diff_n=$((diff_n + 1))
done
if [ $runs -eq 0 ]; then echo "SKIP  T-13  $ipc: SKIP_T13=1"; else ok T-13 "-l none, 클라이언트 4·워커 8 이 캔버스 전체에 DRAW: 정답과 어긋난 실행 $diff_n/$runs (관찰 기록)"; fi

# ---- T-14 클라이언트 8, 워커 8 에서 제한 시간 안에 끝남 ----
if start_server -w 8 -l tile; then
    if timeout 120 bash -c "for i in 0 1 2 3 4 5 6 7; do $C -i \$i -n 5000 -f 16 -m 70 -S 512 >/dev/null 2>&1 & done; wait"; then
        stop_server && ok T-14 "클라이언트 8·워커 8, 요청 40000개가 120초 안에 끝났다" || ng T-14 "서버가 끝나지 않음"
    else
        ng T-14 "120초 안에 끝나지 않았다"; stop_server
    fi
else ng T-14 "서버를 띄우지 못함"; fi

# ---- T-15 큰 응답이 동시에 나가도 섞이지 않음, T-16 STATS 건수 ----
if start_server -w 8 -l tile-rw; then
    run_clients 4 -n $N -f 4 -w 100; crc=$?
    st=$($A stats 9 2>/dev/null)
    out=$($A snap 9 4 $N 90 16 256 8 40 2>&1); arc=$?
    out2=$( ( $A snap 10 4 $N 90 16 256 8 40 & $A snap 11 4 $N 90 16 256 8 40 & wait ) 2>&1 | tr '\n' ' ')
    arc2=$(echo "$out2" | grep -o 'snap 40/40' | wc -l)
    stop_server
    if [ $arc -eq 0 ] && [ "$arc2" = 2 ]; then ok T-15 "1MB SNAPSHOT 40개(동시 8) $out, 두 클라이언트 동시 $out2"
    else ng T-15 "apptest $arc: $out / $out2"; fi
    # 클라이언트 4 × (워밍업 100 + 측정 N) 이 모두 성공한 DRAW + SNAPSHOT 이다.
    # 등록된 클라이언트는 그 4개에 STATS 를 보낸 apptest(번호 9) 까지 5개다
    read -r nd ns _ ncl nw <<< "$st"
    want=$((4 * (100 + N)))
    if [ $crc -eq 0 ] && [ $((nd + ns)) -eq $want ] && [ "$ncl" = 5 ] && [ "$nw" = 8 ]; then
        ok T-16 "STATS DRAW $nd + SNAPSHOT $ns = 보낸 요청 $want, 클라이언트 $ncl, 워커 $nw. 모든 응답의 서버 시각 순서 검사 통과"
    else
        ng T-16 "STATS '$st', 기대 합 $want, 클라이언트 종료 코드 $crc"
    fi
else ng T-15 "서버를 띄우지 못함"; ng T-16 "서버를 띄우지 못함"; fi

# ---- T-17 요청 없이 끝낸 서버: 빈 캔버스 체크섬과 PGM ----
if start_server; then
    stop_server
    head=$(head -c 17 "$tmp/canvas.pgm" | tr '\n' ' ')
    size=$(stat -c %s "$tmp/canvas.pgm" 2>/dev/null)
    if [ "$got" = "$empty" ] && [ "$head" = "P5 1024 1024 255 " ] && [ "$size" = $((17 + 1024 * 1024)) ]; then
        ok T-17 "체크섬 $got 이 검증기의 빈 캔버스와 같고 1024×1024 PGM 저장"
    else ng T-17 "체크섬 $got (빈 캔버스 $empty), PGM 머리 '$head', 크기 $size"; fi
else ng T-17 "서버를 띄우지 못함"; fi

# ---- T-18 CSV 에 한 줄, 표본 수 = 보낸 요청 수 ----
if start_server; then
    $C -i 0 -n 1234 -o "$tmp/app.csv" -e test >/dev/null 2>&1; crc=$?
    $C -i 1 -n 777 -o "$tmp/app.csv" -e test >/dev/null 2>&1 || crc=1
    stop_server
    lines=$(wc -l < "$tmp/app.csv" 2>/dev/null)
    ns=$(awk -F, 'NR > 1 { printf "%s ", $16 }' "$tmp/app.csv" 2>/dev/null)
    if [ $crc -eq 0 ] && [ "$lines" = 3 ] && [ "$ns" = "1234 777 " ]; then ok T-18 "실행마다 CSV 한 줄, n 칸 $ns= 보낸 요청 수"
    else ng T-18 "줄 수 $lines, n 칸 '$ns', 클라이언트 종료 코드 $crc"; fi
else ng T-18 "서버를 띄우지 못함"; fi

# ---- T-30 스레드 수, T-31 두 클라이언트가 동시에 처리받음 ----
if start_server -w 4; then
    $C -i 0 -n 400000 -m 100 >/dev/null 2>&1 & c0=$!
    $C -i 1 -n 400000 -m 100 >/dev/null 2>&1 & c1=$!
    sleep 1
    ts=$(ls /proc/"$srv_pid"/task 2>/dev/null | wc -l)
    tc=$(ls /proc/"$c0"/task 2>/dev/null | wc -l)
    st1=$($A stats 9 2>/dev/null); sleep 0.5; st2=$($A stats 9 2>/dev/null)
    alive=$( (kill -0 $c0 && kill -0 $c1) 2>/dev/null && echo yes)
    kill -INT $c0 $c1 2>/dev/null; wait $c0 $c1 2>/dev/null
    stop_server
    if [ "$ts" -ge 3 ] && [ "$tc" -ge 3 ]; then ok T-30 "실행 중 스레드 수: 서버 $ts, 클라이언트 $tc"
    else ng T-30 "스레드 수 서버 $ts, 클라이언트 $tc"; fi
    d1=$(echo "$st1" | cut -d' ' -f1); d2=$(echo "$st2" | cut -d' ' -f1)
    n1=$(echo "$st2" | cut -d' ' -f4)
    if [ "$alive" = yes ] && [ "$n1" -ge 2 ] && [ "$d2" -gt "$d1" ]; then
        ok T-31 "두 클라이언트가 함께 떠 있는 0.5초 동안 서버가 DRAW $((d2 - d1))개를 처리했다 (클라이언트 $n1개 등록)"
    else ng T-31 "함께 떠 있음 '$alive', STATS '$st1' → '$st2'"; fi
else ng T-30 "서버를 띄우지 못함"; ng T-31 "서버를 띄우지 못함"; fi

# ---- T-21 처리 도중 SIGINT 로 서버를 끝내도 자원이 남지 않음 ----
if start_server -w 4 -l tile; then
    for i in 0 1 2 3; do $C -i $i -n 400000 -f 8 >/dev/null 2>&1 & done
    sleep 1
    stop_server; src=$?
    timeout 30 bash -c "while pgrep -f '^$C ' >/dev/null; do sleep 0.1; done"; crc=$?
    sleep 0.3
    left=$(left_resources)
    if [ $src -eq 0 ] && [ $crc -eq 0 ] && [ -z "$left" ]; then ok T-21 "처리 중 SIGINT: 서버와 클라이언트가 끝나고 남은 자원 없음"
    else ng T-21 "서버 종료 $src, 클라이언트 종료 대기 $crc, 남은 자원 '$left'"; fi
else ng T-21 "서버를 띄우지 못함"; fi

# ---- T-22 서버 없이 띄운 클라이언트, 두 번째 서버 ----
err=$($C -i 0 -n 10 2>&1 >/dev/null); rc=$?
lines=$(printf '%s\n' "$err" | grep -c .)
if [ $rc -eq 1 ] && [ "$lines" = 1 ]; then r1="서버 없는 클라이언트: 종료 코드 1, 한 줄 오류"; else r1=""; ng T-22 "서버 없는 클라이언트: 종료 코드 $rc, 오류 $lines줄 ($err)"; fi
if start_server; then
    err2=$($S 2>&1 >/dev/null); rc2=$?
    $C -i 3 -n 100 >/dev/null 2>&1; rc3=$?
    stop_server
    lines2=$(printf '%s\n' "$err2" | grep -c .)
    if [ -n "$r1" ] && [ $rc2 -eq 1 ] && [ "$lines2" = 1 ] && [ $rc3 -eq 0 ]; then
        ok T-22 "$r1. 두 번째 서버: 종료 코드 1, '$err2'. 떠 있던 서버는 계속 동작"
    elif [ -n "$r1" ]; then
        ng T-22 "두 번째 서버: 종료 코드 $rc2, 오류 $lines2줄, 그 뒤 클라이언트 $rc3"
    fi
else ng T-22 "서버를 띄우지 못함"; fi

echo "SUM ${sum2:-none}"
[ $nfail -eq 0 ]
