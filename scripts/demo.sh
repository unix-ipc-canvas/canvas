#!/usr/bin/env bash
# 데모 시나리오(최종보고서 5.2절, 발표 동영상의 데모)를 터미널에서 순서대로 실행한다.
# 녹화용이라 명령을 한 글자씩 보여 주고 단계마다 잠깐 쉰다. 약 70초. make 로 빌드한 뒤 실행한다.
#   scripts/demo.sh
# GO_FILE 을 주면 그 파일이 생길 때까지 기다렸다 시작한다(녹화 프로그램과 맞추기).
cd "$(dirname "$0")/.." || exit 1
go=${GO_FILE:-}
if [ -n "$go" ]; then while [ ! -f "$go" ]; do sleep 0.1; done; fi
say() { printf '\033[1;36m# %s\033[0m\n' "$*"; sleep 1.2; }
run() {
    printf '\033[1;32m$\033[0m '
    local s="$*" i
    for ((i = 0; i < ${#s}; i++)); do printf '%s' "${s:i:1}"; sleep 0.02; done
    echo; sleep 0.3
    eval "$*"
    sleep 1.5
}
ours() { ipcs | grep -E '^------|0x0000ea7[4-9a-d]'; }
clear
say "1. 시작 전: 우리 key(0xea74~0xea7d)의 IPC 자원이 없다"
run "ours"
say "2. shm 서버 실행 (기본: 워커 4, 전역 락)"
run "./bin/server_shm -p /tmp/canvas_shm.pgm & S=\$!; sleep 0.5"
say "3. 클라이언트 4개를 서로 다른 시드로 동시에 실행 (요청 20,000개씩, DRAW 90%)"
run "for i in 0 1 2 3; do ./bin/client_shm -i \$i -f 16 > /tmp/demo_c\$i.txt & done; sleep 0.2"
say "4. 실행 중: 공유 메모리·세마포어, 서버와 클라이언트의 스레드 수"
run "ours; echo 서버 스레드 \$(ls /proc/\$S/task | wc -l)개, 클라이언트 스레드 \$(ls /proc/\$(pgrep -x client_shm | head -1)/task 2>/dev/null | wc -l)개"
run "while pgrep -x client_shm > /dev/null; do sleep 0.2; done; head -3 /tmp/demo_c0.txt"
say "5. 서버를 Ctrl+C(SIGINT)로 끝낸다: 캔버스 저장과 체크섬"
run "kill -INT \$S; wait \$S"
say "6. 검증기: 같은 시드의 요청을 스레드 하나로 다시 그린 정답"
run "./bin/verify -c 4"
say "7. 락을 끄면(-l none) 같은 칸 갱신이 사라져 체크섬이 어긋난다 (캔버스 전체를 400번씩 그리기)"
run "./bin/server_shm -l none -w 8 -p /tmp/canvas_none.pgm > /tmp/demo_none.txt & S=\$!; sleep 0.5"
run "for i in 0 1 2 3; do ./bin/client_shm -i \$i -n 400 -m 100 -r 1024 -f 8 > /dev/null & done; while pgrep -x client_shm > /dev/null; do sleep 0.2; done"
run "kill -INT \$S; wait \$S; grep checksum /tmp/demo_none.txt; ./bin/verify -c 4 -n 400 -m 100 -r 1024 | head -1"
say "8. 끝난 뒤: 남은 IPC 자원과 /tmp/osproject_2_* 파일이 없다"
run "ours; ls /tmp/osproject_2_* 2>&1"
say "9. pipe와 mq 버전도 같은 요청열이면 같은 체크섬 (정답과 같다)"
run "for v in pipe mq; do ./bin/server_\$v -p /tmp/canvas_\$v.pgm > /tmp/demo_\$v.txt & S=\$!; sleep 0.5; for i in 0 1 2 3; do ./bin/client_\$v -i \$i -f 16 > /dev/null & done; while pgrep -x client_\$v > /dev/null; do sleep 0.2; done; kill -INT \$S; wait \$S; echo \$v \$(grep checksum /tmp/demo_\$v.txt); done"
run "ours; ls /tmp/osproject_2_* 2>&1"
rm -f /tmp/demo_*.txt /tmp/canvas_*.pgm
say "끝"
