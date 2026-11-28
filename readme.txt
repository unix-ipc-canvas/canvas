UNIX시스템프로그래밍 설계과제 - 2팀(Canvas) - 다중 그림판 공유 캔버스 서버
=========================================================================

팀원: 이동건(2022152025, 팀장, shm), 김병수(2022152008, pipe), 오현근(2022152024, mq)
같은 응용(여러 사람이 같은 그림판에 동시에 그리는 서버)을 pipe(FIFO), System V message queue,
System V shared memory + semaphore 세 버전으로 구현했다. 전송 계층만 다르고 나머지는 같은 코드다.
System V key 는 60020 ~ 60029(60xxy, 팀 번호 02), FIFO 는 /tmp/osproject_2_* 이다.

1. 실행 환경
   - Linux (측정: Ubuntu 26.04.1 LTS, 커널 7.0.0-44, Intel Core 5 120U, 논리 CPU 12개, 메모리 14GiB, 가상화 없음)
   - gcc 15.2, GNU make. 외부 라이브러리 없음. 그래프만 Python 3 + matplotlib.

2. 컴파일
   make            전체 빌드 (1단계 벤치마크 4종, 2단계 응용 3종 x 3버전, 실행기, 검증기, 시험 도구)
   make bench      1단계 벤치마크만 (bench_pipe, bench_mq, bench_shm, bench_upipe)
   make app        2단계 응용만 (server_*, client_*, draw_*, loadgen, verify)
   make pipe       pipe 버전만 (make mq, make shm 도 같음)
   make test       검증 (1~3분, 우리 서버가 떠 있지 않을 때)
   make ipcclean   비정상 종료로 남은 IPC 자원과 FIFO 삭제
   make dist       제출용 osproject_2.tar

3. 실행 방법 (<ipc> 는 pipe, mq, shm)
   (1) 서버. Ctrl+C 로 끝나면 그림판별 체크섬을 출력하고 IPC 자원을 모두 지운다.
       ./bin/server_<ipc>                    그림판 8개, 워커 4, 타일 rwlock(64 x 64)
       ./bin/server_<ipc> -K 1 -l room       그림판 1개, 그림판 전체 mutex
   (2) 그리기 클라이언트 (마우스를 지원하는 터미널에서, 여러 터미널에 띄우면 함께 그린다)
       ./bin/draw_<ipc> -k 0                 왼쪽 버튼 끌기: 그리기, 1~9: 색, b/r: 붓/사각형, 휠: 확대, q: 끝
   (3) 부하 클라이언트와 실행기
       ./bin/client_<ipc> -i 0               기준 워크로드(DRAW 90%, SNAPSHOT 10%) 10,000개
       ./bin/loadgen -N 1024 -K 8 -M /tmp/man.txt -- ./bin/client_<ipc> -d 3 -w 50
                                             클라이언트 1024개를 함께 시작시키고 결과를 합친다
       ./bin/verify -K 8 -M /tmp/man.txt     실행기의 목록으로 정답 체크섬 (서버 출력과 같아야 한다)
   (4) 데모 순서: 서버 -> draw 2개 이상 -> loadgen 으로 부하 -> ipcs -> 서버 Ctrl+C -> ipcs (남은 자원 없음)
   (5) 설계 개선 모드 (서버에만 환경 변수, 기본값은 처음 설계)
       OSP_MQ_RESP_Q=8 OSP_MQ_ADMIT=1 ./bin/server_mq     mq 응답 큐 8개 + 요청 큐 입장 제한
       OSP_SHM_REQ=per ./bin/server_shm                   shm 클라이언트별 요청 링
       OSP_PIPE_RESP=1 ./bin/server_pipe -N 64            pipe 응답 FIFO 용량 키우기 (-N = 받을 클라이언트 수)

4. 측정 재현 (측정용 컴퓨터 한 대에서, 다른 무거운 작업을 멈추고)
   scripts/bench.sh                          1단계 측정 -> results/bench_*.csv
   python3 scripts/plot-bench.py results/stage1-r20 docs/assets/bench/stage1-r20
   scripts/app-bench.sh                      2단계 측정(실험 11개, 조건마다 30번, 약 3시간) -> results/app/<실험>.csv
   REPS=2 EXPS="scale" OUT=/tmp/x scripts/app-bench.sh     일부만 짧게
   python3 scripts/plot-app.py               2단계 요약표와 그래프 -> docs/assets/app/
   scripts/race-start.sh -n 150 -l 12        시작 구간 경쟁 재현 실험
   EXPS="cause-workers cause-lockw cause-mix cause-ring" scripts/app-bench.sh
                                             본 측정 뒤에 추가한 원인 확인 실험 4개 (약 30분)
   EXPS="target-ph imp-target-ph cause-pipe imp-pipe" scripts/app-bench.sh
                                             위상을 흩은 목표 부하, pipe 원인 확인·개선 (약 1시간)
   scripts/syscall-count.sh 20000 -m 90 -f 16   요청 하나당 시스템 콜 수 (strace -c)
   scripts/demo-check.sh 1024 3 [그림판 수]   세 버전 x 클라이언트 1024개: 체크섬과 남은 자원 확인
   측정 원자료는 results/ 에 있다 (1단계: bench_*.csv, stage1-r20/, exp-*/ / 2단계: app/, 측정 화면 기록 run-*.log, 감시 기록 monitor-*.log).
