UNIX시스템프로그래밍 설계과제 - 공유 캔버스 서버

1. 실행 환경
   (모든 측정을 한 측정용 컴퓨터. 측정마다 results/.../bench_env.txt, results/app_env.txt 에 같은 내용이 남는다)
   - OS / 커널: Ubuntu 26.04.1 LTS / 7.0.0-43-generic
   - gcc: gcc (Ubuntu 15.2.0-16ubuntu1) 15.2.0
   - CPU / 코어 수 / 메모리: Intel Core 5 120U, 10코어(P코어 2개 + E코어 8개) 12스레드 / 메모리 14GiB
   - 가상화 여부: 없음 (systemd-detect-virt: none). CPU 주파수 관리자 powersave
   - IPC 한도(기본값): msgmax 8192, msgmnb 16384, PIPE_BUF 4096, pipe 버퍼 65536

2. 컴파일
   make            전체 (1단계 벤치마크 + 2단계 응용, 3가지 IPC)
   make bench      1단계 벤치마크만
   make app        2단계 응용만
   make pipe       pipe 버전만 (mq, shm 도 같음)
   make test       검증 스크립트 실행 (PASS / FAIL / SKIP 출력)

3. 실행
   서버를 먼저 띄우고(같은 IPC 의 서버는 하나만), 다른 터미널에서 같은 IPC 의 클라이언트를 띄운다.
     ./bin/server_shm                                 워커 4, 전역 락. 준비되면 stderr 에 "준비"
     for i in 0 1 2 3; do ./bin/client_shm -i $i -f 16 & done; wait    클라이언트 4개 (번호는 서로 달라야 한다)
     ./bin/verify -c 4                                위 4개 클라이언트의 정답 체크섬
   서버는 Ctrl+C(SIGINT) 또는 kill -TERM 으로 끝낸다. 받은 요청을 다 처리한 뒤 canvas_shm.pgm 을 저장하고
   "checksum <16진수>" 를 출력한다. 이 값이 verify 의 출력과 같아야 한다.
   주요 옵션: 서버 -w 워커수 -l global|tile|tile-rw -t 타일한변, 클라이언트 -n 요청수 -m DRAW비율 -f 동시요청수
   (자세한 옵션은 각 프로그램의 -h)

4. 측정 재현
   1단계 (IPC 기초 성능, 측정용 컴퓨터에서 다른 무거운 프로그램을 끄고 실행)
     OUT=results/stage1-r20 scripts/bench.sh    반복 20번, 약 32분 -> results/stage1-r20/bench_*.csv, bench_env.txt
     scripts/exp-stage1.sh                      CPU 배치 실험 -> results/exp-cpu1, exp-pbig, exp-pcore, exp-ecore
     python3 scripts/plot-bench.py results/stage1-r20 docs/assets/bench/stage1-r20    그래프와 요약표
     python3 scripts/compare-bench.py docs/assets/bench/experiments.md results/stage1-r20:results/exp-msgmax-up results/stage1-r20:results/exp-pbig results/exp-pbig:results/exp-cpu1 results/stage1-r20:results/exp-pcore results/stage1-r20:results/exp-ecore
     mq 한도 상향 실험(관리자 권한): sudo sysctl -w kernel.msgmax=1048640 kernel.msgmnb=16777216
       뒤 IPCS=mq OUT=results/exp-msgmax-up scripts/bench.sh, 끝나면 sudo sysctl -w kernel.msgmax=8192 kernel.msgmnb=16384
     results/ 바로 아래의 bench_*.csv 는 처음 측정(반복 5번)으로, 성능 예측의 근거다.
   2단계 (응용)
     REPS=20 scripts/app-bench.sh               실험 7가지 × IPC 3 × 반복 20번 -> results/app_*.csv, app_env.txt
     python3 scripts/plot-app.py                그래프와 요약표 -> docs/assets/app/

5. IPC 자원 확인
   실행 종료 후 ipcs 로 남은 자원이 없는지 확인한다.
   비정상 종료로 남았으면 make ipcclean
