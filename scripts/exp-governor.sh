#!/usr/bin/env bash
# 원인 확인 실험(T10): 워커 4에서 처리량이 떨어지는 것이 CPU 주파수 조절(governor powersave) 때문인지.
# 측정용 컴퓨터에서 사람이 직접 실행한다. governor 를 바꾸려면 관리자 권한이 필요해 처음에 sudo 암호를 묻는다.
# 끝나면(Ctrl+C 로 멈춰도) 처음 governor 로 되돌린다.
#
#   scripts/exp-governor.sh            powersave 와 performance 를 3번 번갈아, 워커 실험(조건마다 6번, 약 30분)
#   ROUNDS=1 scripts/exp-governor.sh   짧게
#
# 결과: results/exp-app-governor/<governor>-free-<번>/  (app-bench.sh 와 같은 형식)
# 요약: python3 scripts/compare-app.py results/exp-app-governor docs/assets/app/exp-app-governor powersave
set -u
cd "$(dirname "$0")/.."
ROUNDS=${ROUNDS:-3}
G=/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
if [ ! -r "$G" ]; then echo "cpufreq 가 없다: $G" >&2; exit 1; fi
avail=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors)
for g in powersave performance; do
    case " $avail " in *" $g "*) ;; *) echo "governor $g 를 쓸 수 없다 (가능: $avail)" >&2; exit 1 ;; esac
done
orig=$(cat "$G")
echo "지금 governor: $orig. 실험 동안 powersave 와 performance 를 번갈아 바꾸고 끝나면 $orig 로 되돌린다."
sudo -v || exit 1

set_gov() {
    echo "$1" | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor >/dev/null
    sleep 2                                         # 주파수가 자리 잡을 시간
    local now; now=$(sort -u /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor | tr '\n' ' ')
    echo "governor: $now"
}
trap 'set_gov "$orig"' EXIT

rc=0
for k in $(seq "$ROUNDS"); do
    for g in powersave performance; do
        sudo -v                                     # 암호 유효 시간 연장
        set_gov "$g"
        REPS=2 EXPS=workers OUT=results/exp-app-governor/$g-free-$k scripts/app-bench.sh || rc=1   # app_env.txt 에 governor 가 남는다
    done
done
exit $rc
