#!/usr/bin/env bash
# 검증 스크립트. 항목과 판정 기준의 원본은 docs/verification/README.md 다.
# 실행 파일이 없는 항목은 SKIP 으로 남는다. SKIP 은 검증 완료가 아니다.
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
            for args in "-c 1 -s 64 -n 2000 -f 1" "-c 8 -s 4097 -n 800 -f 16" "-c 4 -s $maxp -n 40 -f 4" "-c 2 -s 0 -n 500 -f 8"; do
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

# T-10 ~ T-31 2단계 실행 검사: IPC 버전마다 scripts/test-app.sh 를 돌리고, T-12 는 버전 사이의 체크섬을 비교한다
app_have=""
for i in pipe mq shm; do [ -x "bin/server_$i" ] && [ -x "bin/apptest_$i" ] && app_have="$app_have $i"; done
if [ -z "$app_have" ]; then
    for t in T-10 T-11 T-12 T-13 T-14 T-15 T-16 T-17 T-18 T-21 T-22 T-30 T-31; do
        sk "$t" "실행 파일 없음"
    done
else
    sums=""
    for i in $app_have; do
        timeout 900 scripts/test-app.sh "$i" >"$log" 2>&1
        while read -r st t rest; do
            case $st in
                PASS) ok "$t" "$rest" ;;
                FAIL) ng "$t" "$rest" ;;
                SUM)  sums="$sums $i=$t" ;;
            esac
        done < "$log"
        grep -q '^SUM' "$log" || ng T-10 "$i: test-app.sh 가 끝까지 돌지 않았다 ($(tail -1 "$log"))"
    done
    # T-12 같은 입력이면 IPC 버전과 상관없이 최종 캔버스가 같다
    distinct=$(echo "$sums" | tr ' ' '\n' | sed -n 's/^[a-z]*=//p' | sort -u)
    if [ "$(echo "$app_have" | wc -w)" -lt 2 ]; then
        sk T-12 "IPC 버전이 하나뿐이라 비교할 수 없다"
    elif [ "$(echo "$distinct" | wc -l)" = 1 ] && [ "$distinct" != none ]; then
        ok T-12 "같은 입력의 체크섬이 모든 버전에서 같다:$sums"
    else
        ng T-12 "버전마다 체크섬이 다르다:$sums"
    fi
fi

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
