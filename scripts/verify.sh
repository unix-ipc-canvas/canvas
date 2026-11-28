#!/usr/bin/env bash
# 검증 스크립트. 항목과 판정 기준의 원본은 docs/verification/README.md 다.
# 실행 파일이 필요한 항목은 구현 전까지 SKIP 으로 남는다. SKIP 은 검증 완료가 아니다.
set -u
cd "$(dirname "$0")/.."

TEAM_NO=$(sed -n 's/^TEAM_NO ?= *//p' Makefile)
pass=0; fail=0; skip=0
ok() { echo "PASS  $1  $2"; pass=$((pass + 1)); }
ng() { echo "FAIL  $1  $2"; fail=$((fail + 1)); }
sk() { echo "SKIP  $1  $2"; skip=$((skip + 1)); }

log=$(mktemp)
trap 'rm -f "$log"' EXIT

# T-00 문서 검사: 링크, 요구사항과 테스트의 대응, 예측 선행. docs/ 가 없으면(제출용 tar) SKIP
python3 scripts/check-docs.py >"$log" 2>&1
case $? in
    0) ok T-00 "문서 검사" ;;
    2) sk T-00 "$(head -1 "$log")" ;;
    *) ng T-00 "문서 검사"; cat "$log" ;;
esac

# T-01 경고 없는 전체 빌드
if make -s clean >/dev/null 2>&1 && make -s EXTRA_CFLAGS=-Werror >"$log" 2>&1; then
    ok T-01 "make 전체 빌드 (경고 없음)"
else
    ng T-01 "make 전체 빌드"; cat "$log"
fi

# T-02 IPC 버전별 따로 빌드
ipcs_have=""
for i in pipe mq shm upipe; do [ -f "src/transport/transport_$i.c" ] && ipcs_have="$ipcs_have $i"; done
if [ -z "$ipcs_have" ]; then
    sk T-02 "버전별 빌드: 전송 계층 소스 없음"
else
    for i in $ipcs_have; do
        if make -s clean >/dev/null 2>&1 && make -s "$i" EXTRA_CFLAGS=-Werror >"$log" 2>&1; then
            ok T-02 "make $i"
        else
            ng T-02 "make $i"; cat "$log"
        fi
    done
fi

# T-03 공통 코드에 IPC 고유 호출 없음
common=$(ls src/common/*.c src/bench/*.c src/test/*.c 2>/dev/null)
if [ -z "$common" ]; then
    sk T-03 "공통 코드의 IPC 호출 검사: 소스 없음"
elif grep -nE '\b(msgget|msgsnd|msgrcv|msgctl|shmget|shmat|shmdt|shmctl|semget|semop|semctl|mkfifo)\b' $common >"$log"; then
    ng T-03 "공통 코드에 IPC 고유 호출이 있음"; cat "$log"
else
    ok T-03 "공통 코드에 IPC 고유 호출 없음"
fi

# T-04 전송 계층 시험, T-05 1단계 벤치마크 내용 검사. 전체를 다시 빌드한 뒤 돌린다
if ! make -s clean >/dev/null 2>&1 || ! make -s >"$log" 2>&1; then
    ng T-04 "전송 계층 시험: 빌드 실패"; cat "$log"
    ng T-05 "벤치마크 내용 검사: 빌드 실패"
else
    tp_have=""
    for i in pipe mq shm; do [ -x "bin/tptest_$i" ] && tp_have="$tp_have $i"; done
    if [ -z "$tp_have" ]; then
        sk T-04 "전송 계층 시험: 시험 도구 없음"
    else
        for i in $tp_have; do
            if timeout 300 scripts/test-transport.sh "$i" >"$log" 2>&1; then
                ok T-04 "전송 계층 시험 $i ($(grep -c '^PASS' "$log")개 항목)"
            else
                ng T-04 "전송 계층 시험 $i"; grep -v '^PASS' "$log"
            fi
        done
    fi
    b_have=$(ls bin/bench_* 2>/dev/null)
    if [ -z "$b_have" ]; then
        sk T-05 "벤치마크 내용 검사: 실행 파일 없음"
    else
        maxp=$((32 + 1024 * 1024))
        for b in $b_have; do
            bad=""
            for args in "-c 1 -s 64 -n 2000 -f 1" "-c 8 -s 4097 -n 800 -f 16" "-c 4 -s $maxp -n 40 -f 4" "-c 2 -s 0 -n 500 -f 8" "-c 1024 -s 64 -n 2048 -f 1"; do
                timeout 120 "$b" $args -w 4 -V >"$log" 2>&1 || bad="$bad [$args] $(tail -1 "$log")"
            done
            if [ -z "$bad" ]; then ok T-05 "$(basename "$b") -V 내용 검사"; else ng T-05 "$(basename "$b") -V:$bad"; fi
        done
    fi
fi

# T-40 1단계 원자료의 조합, T-41 CSV 열. 측정용 컴퓨터에서 만든 results/ 의 원자료를 검사한다
for t in T-40 T-41; do
    python3 scripts/check-results.py "$t" >"$log" 2>&1
    rc=$?
    case $rc in
        0) ok "$t" "$(head -1 "$log")" ;;
        2) sk "$t" "$(head -1 "$log")" ;;
        *) ng "$t" "$(head -1 "$log")"; tail -n +2 "$log" ;;
    esac
done

# ---- 2단계 응용 (T-10 ~ T-26, T-31) ----
# 실행 파일은 T-04·T-05 앞에서 만든 일반 빌드다. 서버를 띄우고 끝내는 도우미를 쓴다.
APPS=""
for i in pipe mq shm; do [ -x "bin/server_$i" ] && [ -x "bin/client_$i" ] && APPS="$APPS $i"; done
work=$(mktemp -d)
trap 'rm -f "$log"; rm -rf "$work"' EXIT
SP=""
SENV=""          # 서버에만 줄 환경 변수 (VAR=값). 설계 개선 모드 시험(T-25, T-26)이 쓴다
# start_server ipc [옵션...] : 띄우고 "시작" 줄을 기다린다. SP 에 pid (env 는 exec 하므로 서버의 pid 다)
start_server() {
    local ipc=$1; shift
    env $SENV "bin/server_$ipc" -p "$work/c_$ipc.pgm" "$@" > "$work/srv.out" 2> "$work/srv.err" &
    SP=$!
    # srv.err 를 비우는 리다이렉션은 자식이 한다. 자식이 늦게 돌면 직전 서버의 '시작' 줄이 아직 남아 있으므로
    # 이 서버의 pid 가 찍힌 줄만 인정한다 (CI 에서 T-22 가 이 경쟁으로 실패했다)
    for _ in $(seq 100); do grep -q "시작 (pid $SP," "$work/srv.err" 2>/dev/null && return 0; kill -0 "$SP" 2>/dev/null || return 1; sleep 0.05; done
    return 1
}
# stop_server : SIGINT 뒤 10초 안에 끝나야 한다. 종료 코드를 돌려준다
stop_server() {
    kill -INT "$SP" 2>/dev/null
    for _ in $(seq 200); do kill -0 "$SP" 2>/dev/null || break; sleep 0.05; done
    if kill -0 "$SP" 2>/dev/null; then kill -9 "$SP"; wait "$SP" 2>/dev/null; return 124; fi
    wait "$SP"
}
# 서버 출력의 체크섬 줄과 검증기 출력이 같은가
same_sum() { diff <(grep '^checksum' "$work/srv.out") <(bin/verify "$@") > "$work/diff" 2>&1; }
our_left() {
    local l="" y key
    for y in 0 1 2 3 4 5 6 7 8 9; do
        key=$(printf '0x%08x' $((60000 + TEAM_NO * 10 + y)))
        ipcs -q -m -s 2>/dev/null | grep -qi "^$key" && l="$l $key"
    done
    l="$l $(ls -d /tmp/osproject_"${TEAM_NO}"_* 2>/dev/null | tr '\n' ' ')"
    echo "$l" | xargs
}

if [ -z "$APPS" ] || [ ! -x bin/verify ]; then
    for t in T-10 T-11 T-12 T-13 T-14 T-15 T-16 T-17 T-18 T-19 T-21 T-22 T-23 T-24 T-25 T-26 T-31; do sk "$t" "2단계 실행 파일 없음"; done
else
    for ipc in $APPS; do
        # T-10 기본 동작: 그림판 2개, 클라이언트 4개(번호 i → 그림판 i mod 2), 체크섬 = 검증기
        if start_server "$ipc" -K 2; then
            pids=""; for i in 0 1 2 3; do timeout 60 "bin/client_$ipc" -i "$i" -k $((i % 2)) -n 2000 -f 4 -q > "$work/c$i.out" 2>&1 & pids="$pids $!"; done
            bad=0; for p in $pids; do wait "$p" || bad=$((bad + 1)); done
            stop_server; rc=$?
            if [ $bad -eq 0 ] && [ $rc -eq 0 ] && same_sum -K 2 -s 1 -s 2 -s 3 -s 4 -n 2000; then ok T-10 "$ipc 클라이언트 4개 × 그림판 2개, 체크섬 = 검증기"
            else ng T-10 "$ipc 클라이언트 실패 $bad, 서버 $rc"; cat "$work/diff" "$work"/c*.out | head -5; fi
        else ng T-10 "$ipc 서버를 띄우지 못함: $(head -c 200 "$work/srv.err")"; fi

        # T-11 락 방식별 동시 요청: 클라이언트 8 × 동시 요청 16 × DRAW 100%, 그림판 1, 워커 8
        for lk in rw mutex room; do
            if start_server "$ipc" -K 1 -w 8 -l "$lk"; then
                pids=""; for i in 0 1 2 3 4 5 6 7; do timeout 60 "bin/client_$ipc" -i "$i" -k 0 -n 1000 -f 16 -m 100 -q > /dev/null 2>&1 & pids="$pids $!"; done
                bad=0; for p in $pids; do wait "$p" || bad=$((bad + 1)); done
                stop_server; rc=$?
                if [ $bad -eq 0 ] && [ $rc -eq 0 ] && same_sum -K 1 -c 8 -n 1000 -m 100; then ok T-11 "$ipc -l $lk 클라이언트 8 × 동시 16, 체크섬 = 검증기"
                else ng T-11 "$ipc -l $lk 실패 $bad, 서버 $rc"; cat "$work/diff" | head -3; fi
            else ng T-11 "$ipc -l $lk 서버를 띄우지 못함"; fi
        done

        # T-12 SNAPSHOT 내용, T-13 잘못된 요청, T-14 STATS: 그리기 클라이언트 명령 줄 모드 하나로
        if start_server "$ipc" -K 2; then
            printf 'draw 10 10 20 20 5\nsnap 0 0 64 64\nroom 1\nsnap 0 0 64 64\nroom 0\ndraw 1000 1000 100 100 1\ndraw 0 0 0 4 1\nsnap 0 0 257 10\nroom 5\ndraw 0 0 4 4 1\nroom 0\nbadtype\nsnap 0 0 64 64\nstats\nquit\n' \
                | timeout 30 "bin/draw_$ipc" > "$work/d.out" 2>&1
            stop_server
            mapfile -t L < "$work/d.out"
            [ "${L[1]}" = "snap 0 0 64 64 sum=2000 nz=400 fnv=a4111a18411fc7b5" ] && [ "${L[3]%% fnv*}" = "snap 0 0 64 64 sum=0 nz=0" ] \
                && ok T-12 "$ipc 그린 영역의 SNAPSHOT 값이 맞고(합 2000, 0 아닌 칸 400) 다른 그림판과 섞이지 않음" \
                || ng T-12 "$ipc SNAPSHOT 내용: ${L[1]} / ${L[3]}"
            [ "${L[5]}" = "error 3" ] && [ "${L[6]}" = "error 3" ] && [ "${L[7]}" = "error 4" ] && [ "${L[9]}" = "error 2" ] \
                && [ "${L[11]}" = "error 1" ] && [ "${L[12]}" = "${L[1]}" ] \
                && ok T-13 "$ipc 잘못된 요청 5종에 오류 응답(영역 밖, 크기 0, SNAPSHOT 257, 그림판 번호, 종류), 그림판 그대로" \
                || ng T-13 "$ipc 잘못된 요청: $(tr '\n' '|' < "$work/d.out")"
            [ "${L[13]}" = "stats draw=1 snapshot=3 error=5 clients=1 rooms=2 workers=4" ] \
                && ok T-14 "$ipc STATS 건수·접속 수·그림판·워커 수" || ng T-14 "$ipc STATS: ${L[13]}"
        else ng T-12 "$ipc 서버를 띄우지 못함"; ng T-13 "$ipc 서버 없음"; ng T-14 "$ipc 서버 없음"; fi

        # T-15 서버 시각 순서: 클라이언트가 모든 응답에서 t0 ≤ t_recv ≤ t_deq ≤ t_lock ≤ t_send ≤ t5 를 검사한다
        if start_server "$ipc"; then
            timeout 60 "bin/client_$ipc" -i 0 -n 5000 -f 8 > "$work/c.out" 2>&1; rc=$?
            stop_server
            [ $rc -eq 0 ] && ! grep -q '순서 오류' "$work/c.out" && ok T-15 "$ipc 응답 5000개의 서버 시각 순서" || ng T-15 "$ipc $(tail -2 "$work/c.out" | tr '\n' ' ')"
        else ng T-15 "$ipc 서버를 띄우지 못함"; fi

        # T-16 서버 종료: 체크섬 출력, PGM(1,048,593B = 머리 17B + 1MB), 종료 코드 0 / 멈춘 클라이언트가 있을 때 두 번째 신호
        if start_server "$ipc" -K 3; then
            timeout 30 "bin/client_$ipc" -i 1 -n 500 -q > /dev/null 2>&1
            stop_server; rc=$?
            sz=$(stat -c %s "$work/c_$ipc.pgm" 2>/dev/null)
            [ $rc -eq 0 ] && [ "$(grep -c '^checksum room=' "$work/srv.out")" = 3 ] && [ "$sz" = 1048593 ] \
                && ok T-16 "$ipc SIGINT: 그림판 3개 체크섬, PGM ${sz}B, 종료 코드 0" || ng T-16 "$ipc SIGINT rc=$rc pgm=$sz"
        else ng T-16 "$ipc 서버를 띄우지 못함"; fi
        if start_server "$ipc" -w 2; then
            "bin/client_$ipc" -i 2 -m 0 -f 64 -n 1000000 -q > /dev/null 2>&1 & CP=$!
            sleep 0.5; kill -STOP "$CP"; sleep 0.5
            kill -INT "$SP"; sleep 0.5; t0=$(date +%s%N); kill -INT "$SP"
            for _ in $(seq 100); do kill -0 "$SP" 2>/dev/null || break; sleep 0.05; done
            if kill -0 "$SP" 2>/dev/null; then kill -9 "$SP"; wait "$SP" 2>/dev/null; rc=124; else wait "$SP"; rc=$?; fi
            ms=$(( ($(date +%s%N) - t0) / 1000000 ))
            sysv=""
            for y in 0 1 2 3 4 5 6 7 8 9; do
                key=$(printf '0x%08x' $((60000 + TEAM_NO * 10 + y)))
                ipcs -q -m -s 2>/dev/null | grep -qi "^$key" && sysv="$sysv $key"
            done
            [ -e /tmp/osproject_${TEAM_NO}_req ] && sysv="$sysv req-fifo"
            kill -9 "$CP" 2>/dev/null; wait "$CP" 2>/dev/null
            make -s ipcclean > /dev/null 2>&1          # 멈춘 채 죽인 클라이언트가 남긴 자기 응답 FIFO
            [ $rc -eq 1 ] && [ $ms -lt 2000 ] && [ -z "$sysv" ] && ok T-16 "$ipc 멈춘 클라이언트 + 두 번째 SIGINT: ${ms}ms 안에 종료 코드 1, 서버 자원 남지 않음" \
                || ng T-16 "$ipc 두 번째 SIGINT rc=$rc ${ms}ms 남은 것:$sysv"
        else ng T-16 "$ipc 서버를 띄우지 못함"; fi

        # T-17 접속 거부: 서버 없음, 같은 번호
        timeout 10 "bin/client_$ipc" -i 0 -n 10 > "$work/c.out" 2>&1; rc=$?
        [ $rc -eq 1 ] && grep -q '서버가 없다' "$work/c.out" && ok T-17 "$ipc 서버 없음 → 종료 코드 1" || ng T-17 "$ipc 서버 없음: rc=$rc $(cat "$work/c.out")"
        if start_server "$ipc"; then
            "bin/client_$ipc" -i 7 -n 100000000 -q > /dev/null 2>&1 & CP=$!
            sleep 0.5
            timeout 10 "bin/client_$ipc" -i 7 -n 10 > "$work/c.out" 2>&1; rc=$?
            kill -INT "$CP"; wait "$CP" 2>/dev/null
            stop_server
            [ $rc -eq 1 ] && grep -q '같은 번호' "$work/c.out" && ok T-17 "$ipc 같은 번호 → 거부" || ng T-17 "$ipc 같은 번호: rc=$rc $(cat "$work/c.out")"
        else ng T-17 "$ipc 서버를 띄우지 못함"; fi

        # T-18 스레드 수 (서버, 부하 클라이언트, 그리기 클라이언트, 실행기)
        if start_server "$ipc"; then
            "bin/client_$ipc" -i 3 -n 100000000 -q > /dev/null 2>&1 & CP=$!
            (echo "sleep 1500"; echo quit) | "bin/draw_$ipc" > /dev/null 2>&1 & DP=$!
            bin/loadgen -N 2 -q -- "bin/client_$ipc" -n 200000 -w 10 > /dev/null 2>&1 & LP=$!
            sleep 0.8
            ts=$(ls /proc/"$SP"/task | wc -l); tc=$(ls /proc/"$CP"/task | wc -l); td=$(ls /proc/"$DP"/task 2>/dev/null | wc -l)
            tl=$(ls /proc/"$LP"/task 2>/dev/null | wc -l)
            kill -INT "$CP"; wait "$CP" 2>/dev/null; wait "$DP" 2>/dev/null; kill -TERM "$LP" 2>/dev/null; wait "$LP" 2>/dev/null
            stop_server
            [ "$ts" -ge 3 ] && [ "$tc" -ge 3 ] && [ "$td" -ge 3 ] && [ "$tl" -ge 3 ] \
                && ok T-18 "$ipc 스레드 수: 서버 $ts, 부하 클라이언트 $tc, 그리기 $td, 실행기 $tl" \
                || ng T-18 "$ipc 스레드 수: 서버 $ts, 부하 $tc, 그리기 $td, 실행기 $tl"
        else ng T-18 "$ipc 서버를 띄우지 못함"; fi

        # T-19 그리기 클라이언트 둘이 같은 그림판에서 서로 그린 것을 본다. 다른 그림판은 보지 못한다
        if start_server "$ipc" -K 2; then
            (printf 'draw 100 100 10 10 7\n'; sleep 0.6; printf 'snap 200 200 8 8\nquit\n') | timeout 20 "bin/draw_$ipc" -k 0 > "$work/a.out" 2>&1 &
            AP=$!
            sleep 0.3
            (printf 'draw 200 200 8 8 3\nsnap 100 100 10 10\nquit\n') | timeout 20 "bin/draw_$ipc" -k 0 > "$work/b.out" 2>&1
            (printf 'snap 100 100 10 10\nquit\n') | timeout 20 "bin/draw_$ipc" -k 1 > "$work/o.out" 2>&1
            wait "$AP"
            stop_server
            grep -q 'snap 100 100 10 10 sum=700 nz=100' "$work/b.out" && grep -q 'snap 200 200 8 8 sum=192 nz=64' "$work/a.out" \
                && grep -q 'snap 100 100 10 10 sum=0 nz=0' "$work/o.out" \
                && ok T-19 "$ipc 그리기 클라이언트 A·B가 서로의 그림을 보고, 다른 그림판 C는 보지 못함" \
                || ng T-19 "$ipc A:$(tr '\n' '|' < "$work/a.out") B:$(tr '\n' '|' < "$work/b.out") C:$(tr '\n' '|' < "$work/o.out")"
        else ng T-19 "$ipc 서버를 띄우지 못함"; fi

        # T-22 서버 중복 실행
        if start_server "$ipc"; then
            timeout 10 "bin/server_$ipc" -p - > /dev/null 2> "$work/s2.err"; rc=$?
            timeout 30 "bin/client_$ipc" -i 0 -n 300 -q > /dev/null 2>&1; rc2=$?
            stop_server
            [ $rc -eq 1 ] && grep -q '이미 떠 있다' "$work/s2.err" && [ $rc2 -eq 0 ] && ok T-22 "$ipc 두 번째 서버 거부, 첫 서버는 계속 동작" \
                || ng T-22 "$ipc 두 번째 서버 rc=$rc, 첫 서버 클라이언트 rc=$rc2"
        else ng T-22 "$ipc 서버를 띄우지 못함"; fi

        # T-21 kill -9 뒤 다음 서버가 정상으로 뜨고, make ipcclean 뒤 남은 자원이 없다
        if start_server "$ipc"; then
            "bin/client_$ipc" -i 4 -n 100000000 -f 4 -q > /dev/null 2>&1 & CP=$!
            sleep 0.4; kill -9 "$CP"; wait "$CP" 2>/dev/null; kill -9 "$SP"; wait "$SP" 2>/dev/null
            if start_server "$ipc" && timeout 30 "bin/client_$ipc" -i 4 -n 300 -q > /dev/null 2>&1; then stop_server; r1=0; else stop_server; r1=1; fi
            "bin/client_$ipc" -i 5 -n 100000000 -q > /dev/null 2>&1 & CP=$!
            start_server "$ipc" > /dev/null 2>&1; sleep 0.3; kill -9 "$SP" "$CP" 2>/dev/null; wait "$SP" "$CP" 2>/dev/null
            make -s ipcclean > /dev/null 2>&1
            l=$(our_left)
            [ $r1 -eq 0 ] && [ -z "$l" ] && ok T-21 "$ipc kill -9 뒤 다음 서버 정상, make ipcclean 뒤 남은 것 없음" || ng T-21 "$ipc 다음 서버 $r1, 남은 것: $l"
        else ng T-21 "$ipc 서버를 띄우지 못함"; fi

        # T-23 규모: 클라이언트 1024개 × 그림판 8개
        if [ -x bin/loadgen ] && start_server "$ipc" -K 8; then
            timeout 170 bin/loadgen -N 1024 -K 8 -M "$work/man.txt" -T 150 -q -- "bin/client_$ipc" -n 60 -w 5 > "$work/l.out" 2>&1; rc=$?
            stop_server; rc2=$?
            [ $rc -eq 0 ] && [ $rc2 -eq 0 ] && [ "$(wc -l < "$work/man.txt")" = 1024 ] && same_sum -K 8 -M "$work/man.txt" \
                && ok T-23 "$ipc 클라이언트 1024개 × 그림판 8개, 모두 종료 코드 0, 체크섬 = 검증기" \
                || ng T-23 "$ipc 실행기 rc=$rc 서버 rc=$rc2 $(head -c 300 "$work/l.out")"
        else ng T-23 "$ipc 실행기 또는 서버 없음"; fi
    done

    # T-24 경쟁 시연: 락을 끄면(-l none) 그림판 전체를 겹쳐 그린 체크섬이 정답과 어긋난다 (IPC 와 무관한 서버 내부라 하나로)
    ipc=$(echo $APPS | awk '{print $NF}')
    if start_server "$ipc" -K 1 -w 8 -l none; then
        pids=""; for i in 0 1 2 3; do timeout 60 "bin/client_$ipc" -i "$i" -k 0 -n 300 -m 100 -r 1024 -f 8 -q > /dev/null 2>&1 & pids="$pids $!"; done
        for p in $pids; do wait "$p"; done
        stop_server
        if same_sum -K 1 -c 4 -n 300 -m 100 -r 1024; then ng T-24 "$ipc -l none 인데 체크섬이 같다 (경쟁이 드러나지 않음)"
        else ok T-24 "$ipc -l none: 같은 칸 갱신이 사라져 체크섬이 검증기와 다름 (락이 경쟁을 막고 있다는 대조)"; fi
    else ng T-24 "$ipc 서버를 띄우지 못함"; fi

    # T-26 설계 개선 모드 (N-6): mq 응답 큐 8개 + 요청 큐 입장 제한(OSP_MQ_RESP_Q=8 OSP_MQ_ADMIT=1), shm 클라이언트별 요청 링(OSP_SHM_REQ=per),
    # pipe 응답 FIFO 키우기 + 송신 스레드(OSP_PIPE_GROW=1 OSP_PIPE_ASYNC=1)와 송신 스레드만(FIFO 가 작아 나머지가 실제로 생기는 경로),
    # 그리고 pipe 원인 확인 실험의 두 요인(받기 버퍼 OSP_PIPE_RBUF=1, 응답 FIFO 128KB OSP_PIPE_RESP_SZ=131072. 크기를 주면
    # 클라이언트 1024개에서 사용자별 한도로 거부되는 경로도 함께 지난다).
    # 전송 계층 시험 23개, T-10 과 같은 판정, 클라이언트 1024개 × 그림판 8개, kill -9 뒤 같은 번호 재접속
    for m in "mq OSP_MQ_RESP_Q=8 OSP_MQ_ADMIT=1" "shm OSP_SHM_REQ=per" "pipe OSP_PIPE_GROW=1 OSP_PIPE_ASYNC=1" "pipe OSP_PIPE_ASYNC=1" "pipe OSP_PIPE_RBUF=1 OSP_PIPE_RESP_SZ=131072"; do
        read -r ipc SENV <<< "$m"
        case " $APPS " in *" $ipc "*) ;; *) sk T-26 "$ipc 실행 파일 없음"; SENV=""; continue ;; esac
        r=""
        if [ -x "bin/tptest_$ipc" ]; then
            env $SENV timeout 300 scripts/test-transport.sh "$ipc" > "$log" 2>&1 || r="$r 전송계층($(grep -c '^FAIL' "$log")개 실패)"
        fi
        if start_server "$ipc" -K 2; then
            pids=""; for i in 0 1 2 3; do timeout 60 "bin/client_$ipc" -i "$i" -k $((i % 2)) -n 2000 -f 4 -q > /dev/null 2>&1 & pids="$pids $!"; done
            for p in $pids; do wait "$p" || r="$r 클라이언트"; done
            stop_server || r="$r 서버"
            same_sum -K 2 -s 1 -s 2 -s 3 -s 4 -n 2000 || r="$r 체크섬"
        else r="$r 서버시작"; fi
        if start_server "$ipc" -K 8; then
            timeout 170 bin/loadgen -N 1024 -K 8 -M "$work/man.txt" -T 150 -q -- "bin/client_$ipc" -n 60 -w 5 > /dev/null 2>&1 || r="$r 1024개"
            stop_server || r="$r 1024개서버"
            same_sum -K 8 -M "$work/man.txt" || r="$r 1024개체크섬"
        else r="$r 서버시작"; fi
        if start_server "$ipc"; then             # 그림판 8개 (클라이언트 4 → 그림판 4)
            "bin/client_$ipc" -i 4 -n 100000000 -f 4 -q > /dev/null 2>&1 & CP=$!
            sleep 0.4; kill -9 "$CP"; wait "$CP" 2>/dev/null
            timeout 30 "bin/client_$ipc" -i 4 -n 2000 -f 4 -q > /dev/null 2>&1 || r="$r 재접속"
            stop_server || r="$r 재접속서버"
        else r="$r 서버시작"; fi
        [ -z "$r" ] && ok T-26 "$ipc $SENV: 전송 계층 시험, 클라이언트 4개·1024개 체크섬 = 검증기, kill -9 뒤 같은 번호 재접속" \
                    || ng T-26 "$ipc $SENV:$r"
        SENV=""
        make -s ipcclean > /dev/null 2>&1
    done

    # T-25 ThreadSanitizer: 따로 빌드한 tsan/ 에서 T-10 을 줄여 돌린다. 설계 개선 모드와 pipe 원인 확인 모드도 함께
    if echo 'int main(void){return 0;}' | gcc -fsanitize=thread -x c -o "$work/tsan_probe" - 2>/dev/null && "$work/tsan_probe" 2>/dev/null; then
        if make -s clean > /dev/null 2>&1 && make -s app EXTRA_CFLAGS='-fsanitize=thread -O1' > "$work/tsan_build" 2>&1; then
            tbad=""
            modes=""; for ipc in $APPS; do modes="$modes $ipc"; done
            case " $APPS " in *" mq "*) modes="$modes mq:OSP_MQ_RESP_Q=8,OSP_MQ_ADMIT=1" ;; esac
            case " $APPS " in *" shm "*) modes="$modes shm:OSP_SHM_REQ=per" ;; esac
            case " $APPS " in *" pipe "*) modes="$modes pipe:OSP_PIPE_GROW=1,OSP_PIPE_ASYNC=1 pipe:OSP_PIPE_ASYNC=1 pipe:OSP_PIPE_RBUF=1,OSP_PIPE_RESP_SZ=131072" ;; esac
            for m in $modes; do
                ipc=${m%%:*}; SENV=""; [ "$m" != "$ipc" ] && SENV=${m#*:} && SENV=${SENV//,/ }
                if start_server "$ipc" -K 2; then
                    pids=""; for i in 0 1 2 3; do TSAN_OPTIONS=halt_on_error=0 timeout 120 "bin/client_$ipc" -i "$i" -k $((i % 2)) -n 400 -f 4 -q > "$work/tc$i.out" 2>&1 & pids="$pids $!"; done
                    for p in $pids; do wait "$p" || tbad="$tbad $m:client"; done
                    stop_server || tbad="$tbad $m:server"
                    same_sum -K 2 -s 1 -s 2 -s 3 -s 4 -n 400 || tbad="$tbad $m:checksum"
                    grep -l 'ThreadSanitizer' "$work/srv.err" "$work"/tc*.out > /dev/null 2>&1 && tbad="$tbad $m:report"
                else tbad="$tbad $m:start"; fi
            done
            SENV=""
            [ -z "$tbad" ] && ok T-25 "ThreadSanitizer 빌드로 세 버전과 개선·원인 확인 모드 5개, 클라이언트 4개, 보고 0건, 체크섬 일치" || ng T-25 "ThreadSanitizer:$tbad $(grep -h -m1 -A3 'WARNING: ThreadSanitizer' "$work/srv.err" "$work"/tc*.out | head -8)"
        else ng T-25 "ThreadSanitizer 빌드 실패"; head -5 "$work/tsan_build"; fi
        make -s clean > /dev/null 2>&1; make -s > /dev/null 2>&1          # 일반 빌드로 되돌린다
    else
        sk T-25 "이 환경에서 ThreadSanitizer 를 쓸 수 없음"
    fi

    # T-31 2단계 CSV 형식: 클라이언트(-o), 서버(-O), 실행기(-o) 줄
    ipc=$(echo $APPS | awk '{print $1}')
    rm -f "$work/app.csv"
    if start_server "$ipc" -O "$work/app.csv" -e t31; then
        timeout 30 "bin/client_$ipc" -i 0 -n 200 -q -o "$work/app.csv" > /dev/null 2>&1
        OSP_IPC=$ipc timeout 60 bin/loadgen -N 3 -P "$SP" -o "$work/app.csv" -q -- "bin/client_$ipc" -n 100 -w 5 > /dev/null 2>&1
        stop_server
        python3 scripts/check-results.py T-31 "$work/app.csv" > "$log" 2>&1
        [ $? -eq 0 ] && ok T-31 "$(head -1 "$log")" || { ng T-31 "$(head -1 "$log")"; tail -n +2 "$log"; }
    else ng T-31 "서버를 띄우지 못함"; fi
fi

# T-30 측정 구간에 출력·malloc 없음 (MEASURE_BEGIN ~ MEASURE_END)
bad30=$(awk '/MEASURE_BEGIN/{m=1} m && /(printf|fprintf|puts|malloc|calloc|realloc)[[:space:]]*\(/{print FILENAME":"FNR": "$0} /MEASURE_END/{m=0}' src/*/*.c 2>/dev/null)
if ! grep -q MEASURE_BEGIN src/*/*.c 2>/dev/null; then
    sk T-30 "측정 구간 표시 없음 (구현 전)"
elif [ -z "$bad30" ]; then
    ok T-30 "측정 구간에 출력·malloc 없음"
else
    ng T-30 "측정 구간에 출력·malloc"; echo "$bad30"
fi

# T-42 2단계 원자료 (측정 전에는 SKIP)
python3 scripts/check-results.py T-42 >"$log" 2>&1
case $? in
    0) ok T-42 "$(head -1 "$log")" ;;
    2) sk T-42 "$(head -1 "$log")" ;;
    *) ng T-42 "$(head -1 "$log")"; tail -n +2 "$log" ;;
esac

# T-20 남은 IPC 자원 없음 (우리 서버가 떠 있지 않을 때 실행한다)
left=""
for y in 0 1 2 3 4 5 6 7 8 9; do
    key=$(printf '0x%08x' $((60000 + TEAM_NO * 10 + y)))
    ipcs -q -m -s 2>/dev/null | grep -qi "^$key" && left="$left $key"
done
fifos=$(ls /tmp/osproject_"${TEAM_NO}"_* 2>/dev/null)
if [ -z "$left" ] && [ -z "$fifos" ]; then
    ok T-20 "남은 IPC 자원과 FIFO 없음"
else
    ng T-20 "남은 자원:$left $fifos"
fi

# 끝난 뒤에는 T-04·T-05 앞에서 만든 일반 빌드(make)를 bin/ 에 남긴다. readme.txt 의 순서(make → make test → 실행)를 따라도 실행 파일이 있다
echo "----"
echo "PASS $pass / FAIL $fail / SKIP $skip"
[ "$fail" -eq 0 ]
