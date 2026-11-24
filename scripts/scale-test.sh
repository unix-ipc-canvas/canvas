#!/usr/bin/env bash
# 대규모 버티기 시험: 클라이언트 N 개를 그림판(방) R 개에 나눠 한꺼번에 붙여 그리게 하고,
# 모두 끝나는지, 방마다 체크섬이 검증기와 같은지, 끝난 뒤 자원이 남지 않는지 본다.
#
#   scripts/scale-test.sh <ipc> [클라이언트 N=1024] [방 R=16] [요청 n=200] [동시 요청 f=4] [서버 옵션...]
#   예: OSP_MQ_QUEUES=64 scripts/scale-test.sh mq 1024 16 200 4 -l tile -w 8
#
# 클라이언트 i 는 방 i % R 에 그린다(검증기 -K 와 같은 규칙). 시드는 기본값(번호 + 1).
# 환경 변수
#   LIMIT=초          모든 클라이언트가 이 안에 끝나지 않으면 남은 클라이언트를 SIGKILL 로 끝낸다 (기본 300)
#   BIN=폴더          다른 빌드로 돌린다 (기본 bin)
#   CLIENT_OPTS, VERIFY_OPTS   클라이언트와 검증기에 함께 줄 요청 옵션 (예: "-m 100")
#   CSV=파일 EXP=이름 RUN=번호  주면 측정한다: 클라이언트를 -G 로 띄워 모두 준비되면 SIGUSR2 로 함께 시작하고,
#                     클라이언트마다·서버 한 줄씩 CSV 에 쓴다(원자료 형식). clients 칸은 N 이다
# 출력 맨 끝 줄: ok|ng <ipc> N=<수> R=<수> ... 끝남 <수>/<N> <초>초 체크섬 같음|다름 서버 ... 남은 자원 <수>
set -u
cd "$(dirname "$0")/.."
ipc=$1; N=${2:-1024}; R=${3:-16}; n=${4:-200}; f=${5:-4}; shift $(( $# < 5 ? $# : 5 ))
LIMIT=${LIMIT:-300}
B=${BIN:-bin}
CSV=${CSV:-}; EXP=${EXP:-scale}; RUN=${RUN:-1}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
[ -x "$B/server_$ipc" ] || { echo "$B/server_$ipc 가 없다 (make)" >&2; exit 2; }
TEAM_NO=$(sed -n 's/^TEAM_NO ?= *//p' Makefile)
KEYS=$(for y in 0 1 2 3 4 5 6 7 8 9; do printf '0x%08x|' $((60000 + TEAM_NO * 10 + y)); done); KEYS=${KEYS%|}
if ls /tmp/osproject_"$TEAM_NO"_* >/dev/null 2>&1; then echo "우리 서버나 남은 파일이 있다(/tmp/osproject_${TEAM_NO}_*)" >&2; exit 2; fi
# key 없는(IPC_PRIVATE) 큐 수. mq 큐 나누기의 큐는 key 가 없어, 끝난 뒤 이 수가 늘었으면 남은 자원으로 센다
pq0=$(ipcs -q 2>/dev/null | grep -c '^0x00000000 ')

sopt=(); copt=(); warm=0
if [ -n "$CSV" ]; then
    sopt=(-O "$CSV" -e "$EXP" -R "$RUN")
    copt=(-G -o "$CSV" -e "$EXP" -R "$RUN" -c "$N")
    warm=20                                         # 워밍업 DRAW 는 값 0 이라 캔버스를 바꾸지 않는다
fi
"$B/server_$ipc" -K "$R" -p "$W/c.pgm" "${sopt[@]}" "$@" >"$W/srv.log" 2>&1 &
sp=$!
sleep 0.5
t0=$(date +%s.%N)
pids=""
for i in $(seq 0 $((N - 1))); do
    # shellcheck disable=SC2086
    "$B/client_$ipc" -i "$i" -k $((i % R)) -n "$n" -w "$warm" -f "$f" "${copt[@]}" ${CLIENT_OPTS:-} \
        >"$W/c$i.out" 2>"$W/c$i.err" &
    pids="$pids $!"
done
# 제한 시간 감시. 출력을 닫아 두어야 이 스크립트의 출력을 읽는 쪽(| tail)이 감시가 끝나기를 기다리지 않는다
( for _ in $(seq "$LIMIT"); do sleep 1; [ -e "$W/done" ] && exit 0; done; kill -9 $pids ) >/dev/null 2>&1 &
dog=$!
t_go=$t0
if [ -n "$CSV" ]; then
    while :; do
        ready=$(cat "$W"/c*.err 2>/dev/null | grep -c '준비')
        [ "$ready" -ge "$N" ] && break
        alive=0; for p in $pids; do kill -0 "$p" 2>/dev/null && alive=$((alive + 1)); done
        [ "$alive" -lt "$N" ] && break                  # 준비 전에 끝난 클라이언트가 있다
        sleep 0.2
    done
    t_go=$(date +%s.%N)
    kill -USR2 $pids 2>/dev/null
fi
done_n=0
for p in $pids; do wait "$p" && done_n=$((done_n + 1)); done
sec=$(awk -v a="$t_go" -v b="$(date +%s.%N)" 'BEGIN{printf "%.1f", b - a}')
touch "$W/done"; wait "$dog" 2>/dev/null
kill -INT "$sp"; wait "$sp"; src=$?
got=$(grep '^checksum' "$W/srv.log")
# shellcheck disable=SC2086
want=$(bin/verify -c "$N" -n "$n" -K "$R" ${VERIFY_OPTS:-} 2>/dev/null | tail -1)
cpu=$(grep -o "CPU 사용자 [0-9.]*s, 커널 [0-9.]*s" "$W/srv.log" | tail -1)
hwm=$(grep -o 'VmHWM [0-9]* kB' "$W/srv.log" | tail -1)
cnt=$(grep -oE 'DRAW [0-9]+, SNAPSHOT [0-9]+, 오류 응답 [0-9]+' "$W/srv.log" | tail -1)
sleep 0.3
left=$(ipcs 2>/dev/null | grep -ciE "^($KEYS) "); left=$((left + $(ls /tmp/osproject_"$TEAM_NO"_* 2>/dev/null | wc -l)))
pq1=$(ipcs -q 2>/dev/null | grep -c '^0x00000000 '); [ "$pq1" -gt "$pq0" ] && left=$((left + pq1 - pq0))
if [ "$done_n" = "$N" ] && [ "$src" = 0 ] && [ "$got" = "$want" ] && [ "$left" = 0 ]; then r=ok; else r=ng; fi
if [ "$r" = ng ]; then
    tail -3 "$W/srv.log"
    for i in $(seq 0 $((N - 1))); do
        grep -q '서버:' "$W/c$i.out" 2>/dev/null || { echo "클라이언트 $i:"; tail -2 "$W/c$i.out" "$W/c$i.err" 2>/dev/null; break; }
    done
fi
echo "$r $ipc N=$N R=$R n=$n f=$f 끝남 $done_n/$N ${sec}초 체크섬 $([ "$got" = "$want" ] && echo 같음 || echo "다름(${got#checksum } / ${want#checksum })") 서버 $cnt, $cpu, $hwm, 종료 코드 $src, 남은 자원 $left"
[ "$r" = ok ]
