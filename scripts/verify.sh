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

# 제한 시간을 걸어 실행한다: tmo 초 명령... 제한 시간에 SIGTERM 을 보내고, 그래도 남으면 5초 뒤 SIGKILL 로 끝낸다.
# 클라이언트는 첫 SIGTERM 에 남은 응답을 기다리므로, 서버가 멈추면 SIGTERM 만으로는 끝나지 않아 검사 전체가 멈춘다.
# 제한 시간에 걸리면 종료 코드는 124(SIGTERM 으로 끝남) 또는 137(SIGKILL 로 끝남)이다. timed_out 으로 센다
tmo() { timeout -k 5 "$@"; }
timed_out() { [ "$1" -eq 124 ] || [ "$1" -eq 137 ]; }

log=$(mktemp)
W=$(mktemp -d)          # 실행 검사(T-10~T-31)의 출력, CSV, PGM
SP=""                   # 실행 검사에서 띄운 서버의 pid
trap 'rm -f "$log"; rm -rf "$W"; [ -n "$SP" ] && kill -9 "$SP" 2>/dev/null' EXIT

# 우리 key(600x0~600x9)의 System V 자원과 /tmp/osproject_<팀>_* 파일 중 남은 것을 출력한다
leftover() {
    local y key
    for y in 0 1 2 3 4 5 6 7 8 9; do
        key=$(printf '0x%08x' $((60000 + TEAM_NO * 10 + y)))
        ipcs -q -m -s 2>/dev/null | grep -qi "^$key" && printf ' %s' "$key"
    done
    ls /tmp/osproject_"${TEAM_NO}"_* 2>/dev/null | tr '\n' ' '
}

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
            if tmo 300 scripts/test-transport.sh "$i" >"$log" 2>&1; then
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
            for args in "-c 1 -s 64 -n 2000 -f 1" "-c 8 -s 4097 -n 800 -f 16" "-c 4 -s $maxp -n 40 -f 4" "-c 2 -s 0 -n 500 -f 8"; do
                tmo 120 "$b" $args -w 4 -V >"$log" 2>&1 || bad="$bad [$args] $(tail -1 "$log")"
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

# ---- T-10 ~ T-31 실행 검사: 세 버전 각각 서버를 띄워 클라이언트, 검증기, 시험 도구(apptest)로 확인한다 ----
# CI 제한 시간(5분) 안에 끝나도록 요청 수를 작게 둔다. 클라이언트는 tmo 60 으로 감싼다(맨 위의 tmo 설명).

# 서버를 띄우고 "시작" 줄이 나올 때까지 기다린다. $1 = IPC, 나머지는 서버 옵션. 성공 0
srv_start() {
    local ipc=$1 _
    shift
    rm -f "$W/srv.err"      # 앞 서버의 "시작" 을 새 서버의 것으로 읽지 않게. 2> 가 파일을 비우기 전에 grep 이 읽을 수 있다
    "bin/server_$ipc" -p "$W/c.pgm" "$@" >"$W/srv.out" 2>"$W/srv.err" &
    SP=$!
    for _ in $(seq 50); do
        grep -q '시작' "$W/srv.err" 2>/dev/null && return 0
        kill -0 "$SP" 2>/dev/null || break
        sleep 0.1
    done
    return 1
}

# SIGINT 로 끝내고 종료 코드를 돌려준다. 10초 안에 끝나지 않으면 kill -9 하고 99
srv_stop() {
    local rc _
    [ -n "$SP" ] || return 0
    kill -INT "$SP" 2>/dev/null
    for _ in $(seq 100); do
        kill -0 "$SP" 2>/dev/null || break
        sleep 0.1
    done
    if kill -0 "$SP" 2>/dev/null; then
        kill -9 "$SP" 2>/dev/null; wait "$SP" 2>/dev/null; SP=""
        return 99
    fi
    wait "$SP"; rc=$?; SP=""
    return $rc
}

# 클라이언트 k 개(번호 0..k-1, 기본 시드 = 번호 + 1)를 동시에 돌린다. $1 = IPC, $2 = k, $3 = 요청 수, 나머지는 클라이언트 옵션.
# 실패한 클라이언트 수를 돌려주고, 제한 시간에 걸린 수를 TIMEOUTS 에 더한다. 출력은 $W/c<번호>.out
TIMEOUTS=0
run_clients() {
    local ipc=$1 k=$2 n=$3 pids="" bad=0 c p rc
    shift 3
    for ((c = 0; c < k; c++)); do
        tmo 60 "bin/client_$ipc" -i "$c" -n "$n" "$@" >"$W/c$c.out" 2>&1 &
        pids="$pids $!"
    done
    for p in $pids; do
        wait "$p"; rc=$?
        [ "$rc" -eq 0 ] || bad=$((bad + 1))
        timed_out "$rc" && TIMEOUTS=$((TIMEOUTS + 1))
    done
    return $bad
}

srv_sum() { grep '^checksum' "$W/srv.out"; }

RUN_T="T-10 T-11 T-12 T-13 T-14 T-15 T-16 T-17 T-18 T-19 T-21 T-22 T-30 T-31"
app_have=""
for i in pipe mq shm; do
    [ -x "bin/server_$i" ] && [ -x "bin/client_$i" ] && [ -x "bin/apptest_$i" ] && app_have="$app_have $i"
done
if [ -z "$app_have" ] || [ ! -x bin/verify ]; then
    for t in $RUN_T; do sk "$t" "실행 파일 없음"; done
else
    empty=$(bin/verify 2>/dev/null)
    sums12=""
    for i in $app_have; do
        # T-17 요청 없이 끝낸 서버: 빈 캔버스 체크섬, 1024 × 1024 PGM
        if srv_start "$i"; then srv_stop; rc=$?; else srv_stop; rc=98; fi
        hdr_ok=0
        [ -f "$W/c.pgm" ] && [ "$(head -c 17 "$W/c.pgm" | od -An -c | tr -d ' \n')" = 'P5\n10241024\n255\n' ] && hdr_ok=1
        if [ "$rc" -eq 0 ] && [ "$(srv_sum)" = "$empty" ] && [ "$hdr_ok" = 1 ] && [ "$(stat -c %s "$W/c.pgm")" = 1048593 ]; then
            ok T-17 "$i: 빈 서버 체크섬 = 검증기 빈 캔버스, PGM 1024 × 1024"
        else
            ng T-17 "$i: 종료 코드 $rc, $(srv_sum) / $empty, PGM 머리 $hdr_ok"; cat "$W/srv.err"
        fi
        rm -f "$W/c.pgm"

        # T-11 잘못된 요청에 오류 응답, 체크섬 변하지 않음
        srv_start "$i"
        tmo 30 "bin/apptest_$i" bad 5 >"$log" 2>&1; ra=$?
        srv_stop; rc=$?
        if [ "$ra" -eq 0 ] && [ "$rc" -eq 0 ] && [ "$(srv_sum)" = "$empty" ]; then
            ok T-11 "$i: $(sed 's/^ok *//' "$log"), 체크섬 그대로"
        else
            ng T-11 "$i: apptest $ra, 서버 $rc, $(srv_sum)"; cat "$log"
        fi

        # T-16 STATS 건수 = 보낸 DRAW + SNAPSHOT(워밍업 포함). 서버 시각 순서는 클라이언트가 응답마다 검사한다(어긋나면 종료 코드 1)
        srv_start "$i"
        run_clients "$i" 1 700 -w 37; cf=$?
        srv_stop; rc=$?
        st=$(sed -n 's/^  서버: DRAW \([0-9]*\), SNAPSHOT \([0-9]*\),.*/\1 \2/p' "$W/c0.out")
        # shellcheck disable=SC2086
        set -- $st
        if [ "$cf" -eq 0 ] && [ "$rc" -eq 0 ] && [ $# -eq 2 ] && [ $(($1 + $2)) -eq 737 ]; then
            ok T-16 "$i: STATS DRAW $1 + SNAPSHOT $2 = 보낸 737개, 서버 시각 순서 맞음"
        else
            ng T-16 "$i: 클라이언트 $cf, 서버 $rc, STATS [$st] (737 이어야 함)"; cat "$W/c0.out"
        fi

        # T-18 클라이언트가 끝날 때마다 CSV 에 한 줄, 표본 수 n = 보낸 측정 요청 수
        csv="$W/app_$i.csv"
        srv_start "$i"
        run_clients "$i" 1 300 -w 10 -o "$csv"; c1=$?; l1=$(wc -l <"$csv" 2>/dev/null || echo 0)
        run_clients "$i" 1 300 -w 10 -o "$csv"; c2=$?; l2=$(wc -l <"$csv" 2>/dev/null || echo 0)
        srv_stop; rc=$?
        ns=$(awk -F, 'NR == 1 { for (k = 1; k <= NF; k++) if ($k == "n") col = k; next } { printf "%s ", $col }' "$csv" 2>/dev/null)
        if [ "$c1$c2$rc" = 000 ] && [ "$l1" = 2 ] && [ "$l2" = 3 ] && [ "$ns" = "300 300 " ]; then
            ok T-18 "$i: 실행마다 CSV 한 줄 추가(머리글 + 1 → + 2), n = 300"
        else
            ng T-18 "$i: 클라이언트 $c1 $c2, 서버 $rc, 줄 수 $l1 → $l2, n = [$ns]"; cat "$W/c0.out"
        fi

        # T-22 서버 없이 띄운 클라이언트, 두 번째 서버: 한 줄 오류와 종료 코드 1. 떠 있던 서버는 계속 동작
        "bin/client_$i" -i 3 -n 10 >"$W/o1" 2>"$W/e1"; r1=$?
        srv_start "$i"
        tmo 10 "bin/server_$i" -p "$W/c2.pgm" >"$W/o2" 2>"$W/e2"; r2=$?   # 잘못 떠도 멈추지 않게
        run_clients "$i" 1 200 -w 10; cf=$?
        srv_stop; rc=$?
        if [ "$r1" = 1 ] && [ "$(wc -l <"$W/e1")" = 1 ] && [ "$r2" = 1 ] && [ "$(wc -l <"$W/e2")" = 1 ] &&
           [ "$cf" = 0 ] && [ "$rc" = 0 ] && [ ! -e "$W/c2.pgm" ]; then
            ok T-22 "$i: 서버 없는 클라이언트·두 번째 서버 모두 한 줄 오류와 종료 코드 1, 떠 있던 서버 정상"
        else
            ng T-22 "$i: 클라이언트 $r1($(wc -l <"$W/e1")줄), 두 번째 서버 $r2($(wc -l <"$W/e2")줄), 뒤 클라이언트 $cf, 서버 $rc"
            cat "$W/e1" "$W/e2"
        fi

        # T-30 실행 중 스레드 수: 서버, 클라이언트 각각 3개 이상
        srv_start "$i"
        "bin/client_$i" -i 0 -n 10000000 -f 4 -w 0 >"$W/c0.out" 2>&1 &
        cp=$!
        sleep 0.5
        ts=$(ls "/proc/$SP/task" 2>/dev/null | wc -l); tc=$(ls "/proc/$cp/task" 2>/dev/null | wc -l)
        kill -INT "$cp" 2>/dev/null; wait "$cp" 2>/dev/null
        srv_stop; rc=$?
        if [ "$ts" -ge 3 ] && [ "$tc" -ge 3 ] && [ "$rc" = 0 ]; then
            ok T-30 "$i: 서버 스레드 $ts개, 클라이언트 스레드 $tc개"
        else
            ng T-30 "$i: 서버 스레드 $ts개, 클라이언트 스레드 $tc개, 서버 종료 코드 $rc"
        fi

        # T-10 락 3종 × 클라이언트 2·8개의 체크섬 = 검증기. T-14 클라이언트 8·워커 8 이 제한 시간 안에 끝남.
        # T-31 클라이언트 2개 실행에서 STATS 의 접속 클라이언트 수가 2 인 클라이언트가 있음. T-12 용 체크섬도 모은다
        bad10=""; to0=$TIMEOUTS; hung=0; seen2=0
        for lock in global tile tile-rw; do
            for k in 2 8; do
                if [ "$k" = 2 ]; then n=1000; so="-w 4"; co="-w 20"; else n=400; so="-w 8 -t 16"; co="-w 20 -f 8"; fi
                # shellcheck disable=SC2086
                srv_start "$i" -l "$lock" $so
                # shellcheck disable=SC2086
                run_clients "$i" "$k" "$n" $co; cf=$?
                srv_stop; rc=$?
                [ "$rc" = 99 ] && hung=$((hung + 1))
                got=$(srv_sum); want=$(bin/verify -c "$k" -n "$n" 2>/dev/null)
                [ "$cf" = 0 ] && [ "$rc" = 0 ] && [ "$got" = "$want" ] ||
                    bad10="$bad10 [-l $lock 클라이언트 $k: 실패 $cf, 서버 $rc, ${got#checksum } / ${want#checksum }]"
                if [ "$k" = 2 ]; then
                    grep -h '^  서버:' "$W/c0.out" "$W/c1.out" | grep -q '클라이언트 2,' && seen2=$((seen2 + 1))
                    [ "$lock" = global ] && sums12="$sums12 $i=${got#checksum }"
                fi
            done
        done
        if [ -z "$bad10" ]; then ok T-10 "$i: 락 global·tile·tile-rw × 클라이언트 2·8개 체크섬 = 검증기"
        else ng T-10 "$i:$bad10"; fi
        if [ "$TIMEOUTS" -eq "$to0" ] && [ "$hung" -eq 0 ]; then
            ok T-14 "$i: 클라이언트 8개·워커 8개 실행 3번이 제한 시간 안에 끝남"
        else
            ng T-14 "$i: 클라이언트 제한 시간 초과 $((TIMEOUTS - to0))개, 서버가 끝나지 않음 $hung번"
        fi
        if [ "$seen2" -ge 1 ]; then ok T-31 "$i: 클라이언트 2개 실행 3번 중 $seen2번 STATS 시점에 둘 다 접속해 처리받는 중"
        else ng T-31 "$i: 클라이언트 2개가 동시에 처리받은 증거가 없음"; fi

        # T-13 -l none 으로 같은 영역(캔버스 전체)에 몰아 그리기. 어긋나지 않아도 FAIL 이 아니다(관찰)
        srv_start "$i" -l none -w 8
        run_clients "$i" 8 300 -w 0 -m 100 -r 1024 -f 16; cf=$?
        srv_stop; rc=$?
        got=$(srv_sum); want=$(bin/verify -c 8 -n 300 -m 100 -r 1024 2>/dev/null)
        if [ "$cf" != 0 ] || [ "$rc" != 0 ]; then ng T-13 "$i: 클라이언트 실패 $cf, 서버 $rc"
        elif [ "$got" != "$want" ]; then ok T-13 "$i: 관찰 - 락 없이 체크섬이 어긋남 (${got#checksum } / 정답 ${want#checksum })"
        else ok T-13 "$i: 관찰 - 이번 실행에서는 어긋나지 않음 (FAIL 아님)"; fi

        # T-15 그린 뒤 apptest 4개가 동시 8개씩 큰 SNAPSHOT 을 받아 정답 캔버스와 비교.
        # 설계 개선 실험의 서버 환경으로도 한 번 더 본다: pipe 는 FIFO 버퍼 1MB(OSP_PIPE_SZ),
        # shm 은 요청·응답 링을 가장 큰 메시지 1개(칸 4096B 로 257칸)로 줄인 것(OSP_SHM_*_CHUNKS). 쉼표로 여러 변수
        envs="-"
        [ "$i" = pipe ] && envs="- OSP_PIPE_SZ=1048576"
        [ "$i" = shm ] && envs="- OSP_SHM_REQ_CHUNKS=257,OSP_SHM_RESP_CHUNKS=257"
        for ev in $envs; do
            tag=""
            if [ "$ev" != - ]; then
                for kv in ${ev//,/ }; do export "$kv"; done
                tag=" $ev"
            fi
            srv_start "$i" -l tile -w 8
            if [ "$ev" != - ]; then for kv in ${ev//,/ }; do unset "${kv%%=*}"; done; fi
            run_clients "$i" 4 500 -w 10; cf=$?
            pids=""
            for a in 10 11 12 13; do
                tmo 60 "bin/apptest_$i" snap "$a" 24 8 4 500 >"$W/a$a.out" 2>&1 &
                pids="$pids $!"
            done
            abad=0
            for p in $pids; do wait "$p" || abad=$((abad + 1)); done
            srv_stop; rc=$?
            if [ "$cf" = 0 ] && [ "$abad" = 0 ] && [ "$rc" = 0 ] && [ "$(srv_sum)" = "$(bin/verify -c 4 -n 500 2>/dev/null)" ]; then
                ok T-15 "$i$tag: apptest 4개 × $(sed 's/^ok *//' "$W/a10.out")"
            else
                ng T-15 "$i$tag: 그리기 실패 $cf, apptest 실패 $abad, 서버 $rc"; cat "$W"/a1?.out
            fi
        done

        # T-21 클라이언트 8개가 처리받는 도중 서버 SIGINT: 서버와 클라이언트가 끝나고 자원이 남지 않는다
        srv_start "$i" -l tile -w 8
        pids=""
        for c in 0 1 2 3 4 5 6 7; do
            tmo 60 "bin/client_$i" -i "$c" -n 10000000 -f 16 -m 50 -w 0 >"$W/c$c.out" 2>&1 &
            pids="$pids $!"
        done
        sleep 1
        srv_stop; rc=$?
        cto=0
        for p in $pids; do wait "$p"; timed_out $? && cto=$((cto + 1)); done
        sleep 0.3
        lo=$(leftover)
        if [ "$rc" = 0 ] && [ "$cto" = 0 ] && [ -z "$lo" ]; then
            ok T-21 "$i: 처리 도중 SIGINT, 서버 종료 코드 0, 클라이언트 모두 끝남, 남은 자원 없음"
        else
            ng T-21 "$i: 서버 $rc, 끝나지 않은 클라이언트 $cto, 남은 자원 [$lo]"
            make -s ipcclean >/dev/null 2>&1
        fi

        # T-19 사람이 그리는 클라이언트: 명령 줄 둘이 동시에 그린 결과 = 정답, 화면(pty)에서 마우스로 그리기
        if [ -x "bin/draw_$i" ] && command -v python3 >/dev/null; then
            tmo 120 python3 scripts/test-draw.py "$i" "$W" >"$log" 2>&1; rd=$?
            if [ "$rd" -eq 0 ]; then ok T-19 "$(tail -1 "$log" | sed 's/^ok *//')"
            else ng T-19 "$(tail -1 "$log" | sed 's/^ng *//')"; make -s ipcclean >/dev/null 2>&1; fi
        else
            sk T-19 "$i: bin/draw_$i 또는 python3 없음"
        fi
    done

    # T-12 같은 시드와 옵션이면 세 버전의 체크섬이 같다 (T-10 의 -l global, 클라이언트 2개 실행)
    nsum=$(echo "$sums12" | tr ' ' '\n' | sed -n 's/^[a-z]*=//p' | sort -u | wc -l)
    if [ "$(echo "$app_have" | wc -w)" -lt 2 ]; then sk T-12 "버전이 하나뿐이라 비교할 수 없음"
    elif [ "$nsum" = 1 ]; then ok T-12 "세 버전 체크섬 같음:$sums12"
    else ng T-12 "버전마다 체크섬이 다름:$sums12"; fi
fi

# T-20 남은 IPC 자원 없음 (우리 서버가 떠 있지 않을 때 실행한다). 위 실행 검사가 끝난 뒤의 상태도 함께 본다
lo=$(leftover)
if [ -z "$lo" ]; then
    ok T-20 "남은 IPC 자원과 FIFO 없음"
else
    ng T-20 "남은 자원: $lo"
fi

# 끝난 뒤에는 T-04·T-05 앞에서 만든 일반 빌드(make)를 bin/ 에 남긴다. readme.txt 의 순서(make → make test → 실행)를 따라도 실행 파일이 있다
echo "----"
echo "PASS $pass / FAIL $fail / SKIP $skip"
[ "$fail" -eq 0 ]
