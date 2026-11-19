UNIX시스템프로그래밍 설계과제 - 공유 캔버스 서버
(제출용 readme. 구현이 진행되면 아래 빈칸을 채운다.)

1. 실행 환경
   (모든 측정을 한 측정용 컴퓨터. 측정마다 results/.../bench_env.txt 에 같은 내용이 남는다)
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
   (구현 후 작성: 서버 실행, 클라이언트 2개 이상 실행, 종료 방법)

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
     (구현 후 작성)

5. IPC 자원 확인
   실행 종료 후 ipcs 로 남은 자원이 없는지 확인한다.
   비정상 종료로 남았으면 make ipcclean
