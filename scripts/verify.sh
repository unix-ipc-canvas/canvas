#!/usr/bin/env bash
# 검증 스크립트. 항목과 판정 기준의 원본은 docs/test-specs/README.md 다.
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

# T-00 문서 검사: 링크, 요구사항과 테스트의 대응, 예측 선행
if python3 scripts/check-docs.py >"$log" 2>&1; then
    ok T-00 "문서 검사"
else
    ng T-00 "문서 검사"; cat "$log"
fi

# T-01 경고 없는 전체 빌드
if make -s clean >/dev/null 2>&1 && make -s EXTRA_CFLAGS=-Werror >"$log" 2>&1; then
    ok T-01 "make 전체 빌드 (경고 없음)"
else
    ng T-01 "make 전체 빌드"; cat "$log"
fi

# T-02 IPC 버전별 따로 빌드
ipcs_have=""
for i in pipe mq shm; do [ -f "src/transport/transport_$i.c" ] && ipcs_have="$ipcs_have $i"; done
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
common=$(ls src/common/*.c src/bench/*.c 2>/dev/null)
if [ -z "$common" ]; then
    sk T-03 "공통 코드의 IPC 호출 검사: 소스 없음"
elif grep -nE '\b(msgget|msgsnd|msgrcv|msgctl|shmget|shmat|shmdt|shmctl|semget|semop|semctl|mkfifo)\b' $common >"$log"; then
    ng T-03 "공통 코드에 IPC 고유 호출이 있음"; cat "$log"
else
    ok T-03 "공통 코드에 IPC 고유 호출 없음"
fi

# T-10 ~ T-41 실행 검사: 서버·클라이언트 구현 뒤 이 자리에 추가한다
if ! ls bin/server_* >/dev/null 2>&1; then
    for t in T-10 T-11 T-12 T-13 T-14 T-15 T-16 T-17 T-18 T-21 T-22 T-30 T-31 T-40 T-41; do
        sk "$t" "실행 파일 없음 (구현 전)"
    done
else
    for t in T-10 T-11 T-12 T-13 T-14 T-15 T-16 T-17 T-18 T-21 T-22 T-30 T-31 T-40 T-41; do
        sk "$t" "검사 코드 미작성"
    done
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

make -s clean >/dev/null 2>&1
echo "----"
echo "PASS $pass / FAIL $fail / SKIP $skip"
[ "$fail" -eq 0 ]
