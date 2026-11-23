# 공유 캔버스 서버

![공유 캔버스 서버](docs/assets/social-preview.png)

UNIX 시스템 프로그래밍 2팀

![C](https://img.shields.io/badge/C-gnu11-A8B9CC?style=flat-square&logo=c&logoColor=white)
![Linux](https://img.shields.io/badge/Linux-POSIX-FCC624?style=flat-square&logo=linux&logoColor=black)
![GNU Make](https://img.shields.io/badge/build-GNU%20Make-A42E2B?style=flat-square&logo=gnu&logoColor=white)
![IPC](https://img.shields.io/badge/IPC-pipe%20%C2%B7%20message%20queue%20%C2%B7%20shared%20memory-2563EB?style=flat-square)

여러 클라이언트가 하나의 캔버스에 동시에 그리고, 캔버스의 일부나 전체를 받아 가는 서버다. 같은 응용을 pipe, message queue, shared memory 3버전으로 구현해, 작은 그리기 명령과 큰 화면 데이터가 섞인 통신에서 어떤 IPC가 언제 유리한지 측정으로 비교한다. 2026년 2학기 UNIX시스템프로그래밍 설계과제이며 11월 30일(월)에 제출한다.

처음 보는 팀원은 쉬운 말로 쓴 [팀 프로젝트 안내서](https://claude.ai/artifact/9rcWLcBnxL2cK6A4RLx5Lc)부터 읽는다.

프로젝트 파일은 다음과 같이 배치한다. 파일별 책임은 [구현 방향](docs/design/plan.md#구현-방향)을 참고한다.

```text
canvas/
├── AGENTS.md         # 작업 지침
├── Makefile
├── readme.txt        # 제출용 readme
├── include/          # proto.h, transport.h
├── src/
│   ├── transport/    # IPC별 전송 계층 (pipe, mq, shm, 벤치마크 전용 익명 pipe upipe)
│   ├── common/       # 캔버스, 서버, 클라이언트, 검증기
│   ├── bench/        # 1단계 echo 벤치마크
│   └── test/         # 시험 도구 (전송 계층 tptest, 서버 apptest)
├── scripts/          # 검증·측정 스크립트
├── results/          # 측정 원자료 CSV
└── docs/
    ├── assignment/     # 과제 원문과 핵심 정리
    ├── requirements/   # 요구사항
    ├── design/         # 설계 명세, 구현 계획
    ├── measurement/    # 측정 방법
    ├── verification/   # 테스트 명세
    ├── submission/     # 최종보고서·발표 준비
    ├── records/        # 성능 예측, AI 활용 기록, 작업 시간
    ├── assets/         # 문서에 쓰는 이미지
    └── CONTRIBUTING.md # 이슈·PR 작성 규칙
```

## 역할 분담

IPC는 한 사람이 하나씩 맡고, 공통 코드·보고서·발표는 셋으로 나눈다. 쉬운 IPC에는 무거운 공통 코드를 붙여 양을 맞췄다. 1단계 측정, 2단계 분석, 보고서 6.6절(IPC 선택 가이드라인)과 8.1절(결론)은 담당과 관계없이 셋이 함께 한다. 파일별 책임은 [구현 방향](docs/design/plan.md#구현-방향), 기한은 [단계별 계획](docs/design/plan.md#단계별-계획)을 따른다.

| | A: pipe + 서버 | B: mq + 클라이언트, 예측·문서 총괄 | C: shm + 측정 |
| --- | --- | --- | --- |
| 팀원 | [@pinoba](https://github.com/pinoba) | [@hyungunoh321](https://github.com/hyungunoh321) | [@Movinggun-bit](https://github.com/Movinggun-bit) |
| IPC | `transport_pipe.c` | `transport_mq.c` (큰 메시지 분할·재조립, 응답 큐 방식 결정) | `transport_shm.c` (링 버퍼, 슬롯, 세마포어, 알림 방식 결정) |
| 공통 코드 | `canvas.c`, `server.c` (스레드, 작업 큐, 락 옵션, 종료 처리) | `client.c` (워크로드 생성, CSV 출력), `verify.c` | `util.c`, `bench.c` |
| 검증·측정 | `verify.sh`의 실행 검사 T-10~T-31 | T-40·T-41, `readme.txt`, `make dist`와 tar 점검 | 측정 스크립트, 그래프, CSV 열 확정, 측정용 컴퓨터 관리 |
| 앞장설 단계 | T6 설계 개선 구현 (타일 락, rwlock, 메모리 절감) | T3 워크로드 명세와 예측 | T2 1단계 측정, T5 원인 확인 실험 설계 |
| 보고서 | 4장, 5장, 6.1, 6.5 | 표지, 1장, 3장, 7장, 8.2 | 2장, 6.2~6.4 |
| 발표 | 데모 촬영 | 발표자료 취합, 제출 점검 | 측정 결과 슬라이드 |
| 그 밖 | FIFO를 pipe로 인정하는지 교수님께 확인 | AI 활용 기록 정리 | 측정 환경 기록, 저장소 관리(owner: 헤더 변경 승인, `main` 보호 규칙) |

- 세 사람 모두 11월 15일까지 자기 `transport_*.c`의 기본 송수신을 끝낸다. 늦으면 C의 1단계 측정이 시작되지 못해 예측과 구현이 함께 밀린다.
- 예측을 커밋하기 전에는 A는 `canvas.c`, B는 `verify.c`, C는 `util.c`를 먼저 만든다. `server.c`, `client.c`는 예측 커밋 뒤에 시작한다.

## 기술 스택

C(gnu11), gcc, GNU make, POSIX 스레드를 쓴다. IPC는 FIFO, System V message queue, System V shared memory와 semaphore다. 외부 라이브러리는 쓰지 않는다. 세부 제약은 [요구사항](docs/requirements/README.md#실행-환경과-제약)을 참고한다.

## 시작하기

Linux에서 gcc와 make만 있으면 된다.

```bash
git clone https://github.com/unix-ipc-canvas/canvas.git
cd canvas
git config commit.template .gitmessage   # 커밋 형식 안내 (처음 한 번)
make
```

| 명령 | 동작 |
| --- | --- |
| `make` | 전체 빌드. 소스가 있는 것만 빌드한다 |
| `make bench` | 1단계 벤치마크 3종 |
| `make app` | 2단계 응용(서버, 클라이언트) 3종 |
| `make pipe` / `make mq` / `make shm` | 그 IPC 버전만 |
| `make check` | 헤더 검사와 지금 빌드 가능한 대상 보기 |
| `make ipcclean` | 비정상 종료로 남은 IPC 자원과 FIFO 삭제 |
| `make dist` | 제출용 `osproject_2.tar` 만들기 |

> [!NOTE]
> 지금은 전송 계층 3개(+ 벤치마크 전용 익명 pipe), 1단계 벤치마크, 서버, 클라이언트, 검증기가 있고 `make test`의 모든 항목이 동작한다.

## 사용 방법

`bin/`에 `bench_<ipc>`, `server_<ipc>`, `client_<ipc>`, `verify`가 생긴다(`<ipc>`는 `pipe`, `mq`, `shm`). 1단계 벤치마크에는 강의 5장의 익명 pipe로 만든 `bench_upipe`도 있다. 익명 pipe는 fork로 물려받아야 이어지므로 서버·클라이언트는 만들지 않는다. 서버·클라이언트 옵션의 뜻은 [실행 옵션](docs/design/README.md#실행-옵션)에 있다. 확정된 실행 예시는 구현한 사람이 이 절에 적는다(서버는 서버 담당이 적는다).

### 1단계 벤치마크

한 번 실행하면 서버 1개와 클라이언트 N개를 띄워 조건 하나를 재고, 결과 한 줄을 화면(stderr)에 낸다. `-o`를 주면 CSV에 두 줄(client, server)을 덧붙인다. 측정 방법은 [1단계 벤치마크](docs/measurement/README.md#1단계-벤치마크)를 따른다.

```bash
make bench
./bin/bench_shm -c 4 -s 65536 -n 10000            # 클라이언트 4개, 64KB, 지연 (동시 요청 1)
./bin/bench_shm -c 4 -s 65536 -n 10000 -f 16      # 동시 요청 16개, 처리량
./bin/bench_pipe -c 8 -s 1048576 -n 500 -V        # 모든 응답의 내용까지 검사 (시험용, 측정에 쓰지 않는다)
```

| 옵션 | 뜻 (기본값) |
| --- | --- |
| `-c N` | 동시 클라이언트 수, 1~16 (1) |
| `-s B` | 메시지 payload 바이트, 0~1048608(`MAX_PAYLOAD`, 1MB + 32B) (64) |
| `-n N` | 잴 메시지 수, 모든 클라이언트 합 (10000) |
| `-w N` | 클라이언트마다 워밍업 메시지 수 (100) |
| `-f N` | 클라이언트마다 동시에 띄워 둘 요청 수. 1이면 지연, 크면 처리량 (1) |
| `-e S`, `-r N` | CSV의 실험 이름, 반복 번호 |
| `-o F`, `-C S` | 결과 CSV 경로, 측정한 커밋 (기본: 환경 변수 `BENCH_COMMIT`) |
| `-V` | 모든 응답의 내용을 검사한다 |
| `-K` | `clock_gettime` 비용만 잰다 |

1단계 전체 측정은 측정용 컴퓨터에서 다른 무거운 작업을 멈추고 돌린다(약 20분). 코드를 커밋한 뒤에만 돈다.

```bash
scripts/bench.sh                  # results/bench_*.csv, results/bench_env.txt
python3 scripts/plot-bench.py     # docs/assets/bench/ 에 그래프와 요약표 (matplotlib 필요: sudo apt install python3-matplotlib)
python3 scripts/plot-bench.py results/stage1-r20 docs/assets/bench/stage1-r20   # 다른 폴더의 원자료
scripts/exp-stage1.sh             # CPU 배치를 바꾼 원인 확인 실험 → results/exp-*/
python3 scripts/compare-bench.py docs/assets/bench/experiments.md results/stage1-r20:results/exp-pcore   # 기준:실험 비교 표
```

`REPS`, `SIZES`, `CLIENTS`, `IPCS`, `INFLIGHT`, `OUT` 환경 변수로 일부만 돌릴 수 있다. 예: `REPS=1 SIZES="64 1048576" CLIENTS="1 8" OUT=/tmp/x scripts/bench.sh`

### 2단계 서버

서버를 띄우면 Ctrl+C(SIGINT)나 SIGTERM을 받을 때까지 요청을 처리한다. 끝날 때 캔버스를 PGM으로 저장하고, stdout에 `checksum <16진수>`(검증기와 같은 형식), 처리 건수, 실행 전체의 CPU 시간·컨텍스트 스위치·VmRSS를 쓴다. 이때 만든 IPC 자원과 FIFO를 모두 지운다.

```bash
make app
./bin/server_pipe                                  # 기준 조건: 워커 4, 전역 락
./bin/server_shm -w 8 -l tile -t 16                # 워커 8, 타일 락, 타일 16 × 16
./bin/server_mq -l tile-rw -p /tmp/mq.pgm          # 타일 rwlock, PGM 경로 지정
```

| 옵션 | 뜻 (기본값) |
| --- | --- |
| `-w N` | 워커 수, 1~64 (4) |
| `-l S` | 락 방식 `global`, `tile`, `tile-rw`, `none` (global). `none`은 경쟁 상태 시연 전용이다 |
| `-t N` | 타일 한 변, 16~1024의 2의 거듭제곱 (64). `tile`, `tile-rw`에서만 뜻이 있다 |
| `-d` | 작업 큐와 워커 없이 수신 스레드가 직접 처리한다(큐 비용 확인용) |
| `-p F` | 끝날 때 저장할 PGM 경로 (`canvas_<ipc>.pgm`) |
| `-O F` | 끝날 때 CSV F에 서버 줄(`stage=app`, `role=server`) 하나를 덧붙인다. CPU 시간·컨텍스트 스위치는 스레드를 만든 때부터 거둔 때까지의 값이다 |
| `-e S`, `-R N`, `-C S` | `-O` 줄의 실험 이름, 반복 번호(1), 커밋(환경 변수 `BENCH_COMMIT`) |

- pipe 서버는 환경 변수 `OSP_PIPE_SZ`(바이트)로 FIFO 버퍼 크기를 바꾼다. 예: `OSP_PIPE_SZ=1048576 ./bin/server_pipe`. 주지 않으면 커널 기본값(64KB)이다. shm 서버의 링 크기 환경 변수는 [설계 명세의 실행 옵션](docs/design/README.md#실행-옵션)에 있다.
- 서버는 IPC마다 하나만 띄울 수 있다. 이미 떠 있으면 stderr에 한 줄을 쓰고 종료 코드 1로 끝나며, 떠 있는 서버의 자원은 건드리지 않는다.
- 캔버스를 벗어나거나 크기가 0인 요청에는 오류로 응답하고 캔버스를 바꾸지 않는다.
- 멈춘 클라이언트(Ctrl+Z 등)가 있으면 첫 Ctrl+C로는 그 응답을 기다리느라 끝나지 않는다. 한 번 더 보내면 기다리지 않고 자원을 지운 뒤 종료 코드 1로 끝난다(PGM 저장과 체크섬 출력 없음).

### 2단계 클라이언트

서버(`server_<ipc>`)를 먼저 띄운 뒤 같은 IPC의 클라이언트를 띄운다. 클라이언트는 워밍업 뒤 정해진 수의 요청을 보내고, 끝나면 통계(왕복 지연, 구간별 시간, 처리량, CPU 시간)를 stdout에 쓴다. `-o`를 주면 CSV에 한 줄(`stage=app`, `role=client`)을 덧붙인다. 재는 방법은 [2단계 실험](docs/measurement/README.md#2단계-실험)을 따른다.

```bash
make app
./bin/client_mq -i 0                                   # 기준 조건: DRAW 90%, 사각형 16, SNAPSHOT 256, 요청 20000개, 지연 (동시 요청 1)
for i in 0 1 2 3; do ./bin/client_shm -i $i -f 16 -c 4 -o /tmp/app.csv & done; wait   # 클라이언트 4개, 처리량
./bin/client_pipe -i 0 -m 0 -S 1024 -n 500             # 캔버스 전체(1MB) SNAPSHOT만
```

| 옵션 | 뜻 (기본값) |
| --- | --- |
| `-i N` | 클라이언트 번호, 0~15 (0). 동시에 띄우는 클라이언트는 번호가 달라야 한다 |
| `-s N` | 시드 (번호 + 1) |
| `-n N` | 잴 요청 수 (20000) |
| `-w N` | 워밍업 요청 수. DRAW의 v를 0으로 보내 캔버스를 바꾸지 않는다 (500) |
| `-m N` | DRAW 비율 %, 나머지는 SNAPSHOT (90) |
| `-r N`, `-S N` | DRAW 사각형 한 변, SNAPSHOT 영역 한 변, 1~1024 (16, 256) |
| `-f N` | 동시에 띄워 둘 요청 수, 1~1024. 1이면 지연, 크면 처리량 (1) |
| `-o F` | 결과 CSV 경로 |
| `-e S`, `-R N`, `-C S` | CSV의 실험 이름(latency 또는 throughput), 반복 번호(1), 커밋(환경 변수 `BENCH_COMMIT`) |
| `-c N`, `-L S`, `-T N` | CSV의 clients, lock, tile 칸. 서버 설정을 그대로 적는다. 동작은 바꾸지 않는다 |
| `-G` | 워밍업이 끝나면 stderr에 `준비`를 쓰고 SIGUSR2를 받을 때까지 측정을 미룬다. 여러 클라이언트의 측정 시작을 맞출 때 쓴다 |

- 서버가 받는 요청은 클라이언트마다 HELLO 1 + 워밍업 `-w` + 측정 `-n` + STATS 1 + BYE 1개다. 워밍업은 캔버스를 바꾸지 않으므로 검증기는 측정 요청(시드, `-n`, `-m`, `-r`, `-S`)만 재생하면 된다.
- Ctrl+C(SIGINT)나 SIGTERM을 받으면 새 요청을 멈추고, 띄워 둔 요청의 응답을 다 받은 뒤 그때까지의 통계를 쓰고 끝난다. 다 재지 못한 실행이라 CSV에 쓰지 않고 종료 코드 1로 끝난다. 서버가 멈춰 응답이 오지 않으면 한 번 더 보내면 남은 응답을 기다리지 않고 바로 끝난다.
- 서버가 없거나, 같은 번호의 클라이언트가 실행 중이거나, 도중에 서버가 끝나면 stderr에 한 줄을 쓰고 종료 코드 1로 끝난다. 응답의 종류·길이·서버 시각 순서가 어긋나도 오류로 세어 종료 코드 1로 끝난다.

### 2단계 측정

측정용 컴퓨터에서 커밋한 코드로 돌린다. 서버와 클라이언트를 실험 조건마다 새로 띄우고, 모든 클라이언트가 워밍업을 마치면 SIGUSR2로 함께 측정을 시작시킨다. 결과는 `results/app_<실험>.csv`(실행마다 클라이언트 줄들과 서버 줄 하나)와 `results/app_env.txt`다. 실험 목록은 [2단계 실험](docs/measurement/README.md#2단계-실험), 결과는 [2단계 측정 결과](docs/measurement/stage2.md)에 있다.

```bash
scripts/app-bench.sh                                      # 전체, 반복 REPS번(기본 20, 약 26분)
REPS=1 EXPS="base clients" OUT=/tmp/x scripts/app-bench.sh # 일부만, 다른 곳에
python3 scripts/plot-app.py                               # docs/assets/app/ 에 요약표와 그래프 (matplotlib 필요)
scripts/exp-stage2.sh                                     # CPU 배치·워커 수를 바꾼 원인 확인 실험 → results/exp-app-*/
python3 scripts/plot-app.py results/exp-app-pcore-mix docs/assets/app/exp-app-pcore-mix   # 실험 요약
```

### 검증기

클라이언트들이 보낸 측정 요청을 같은 시드와 옵션으로 다시 만들어 스레드 하나로 그리고, 정답 체크섬을 `checksum <16진수>` 한 줄로 stdout에 쓴다. 서버가 끝날 때 출력하는 줄과 같은 형식이다. IPC를 쓰지 않으므로 세 버전 모두 이 검증기 하나와 비교한다. `make`가 `bin/verify`를 만든다.

```bash
./bin/verify -c 4                          # 기본 시드로 띄운 클라이언트 0~3 (client -i 0 ~ -i 3), 기준 조건
./bin/verify -s 7 -s 9 -n 5000 -m 100      # 시드 7, 9인 클라이언트 2개, 요청 5000개, DRAW만
./bin/verify                               # 시드가 없으면 빈 캔버스 (T-17)
```

| 옵션 | 뜻 (기본값) |
| --- | --- |
| `-s N` | 재생할 클라이언트의 시드. 클라이언트마다 하나씩 여러 번 준다 |
| `-c K` | 클라이언트 번호 0~K-1의 기본 시드(번호 + 1)를 더한다, 1~16 |
| `-n N`, `-m N`, `-r N`, `-S N` | 클라이언트와 같은 뜻과 기본값. 모든 클라이언트가 같은 값으로 돌았다고 본다 |

- 클라이언트가 신호로 도중에 멈췄으면 보낸 요청 수가 `-n`과 달라 체크섬이 맞지 않는다. 비교에는 끝까지 돈 실행만 쓴다.

## 테스트

```bash
make test
```

문서 검사(링크, 요구사항과 테스트의 대응)와 빌드·동작 검사를 실행하고 항목마다 PASS, FAIL, SKIP을 출력한다. 항목과 판정 기준은 [테스트 명세](docs/verification/README.md)를 따른다. SKIP은 검증 완료가 아니다. 우리 서버가 떠 있지 않을 때 돌린다. 서버·클라이언트를 세 버전 각각 여러 번 띄우므로 1~2분 걸린다.

전송 계층 시험(T-04)만 따로 돌리거나 새니타이저로 돌릴 수 있다.

```bash
make && scripts/test-transport.sh shm              # 고른 IPC 만 (생략하면 셋 다)
make clean && make EXTRA_CFLAGS='-fsanitize=address,undefined' && scripts/test-transport.sh
make clean && make EXTRA_CFLAGS='-fsanitize=thread' && scripts/test-transport.sh
```

## 관련 문서

문서는 일의 순서대로 폴더를 나눴다: 과제(`assignment`) → 요구사항(`requirements`) → 설계(`design`) → 측정(`measurement`) → 검증(`verification`) → 제출(`submission`). 덧붙이기만 하는 기록은 `records`에 둔다.

문서 관리 원칙은 [작업 지침](AGENTS.md)을 따른다. 아래에서 변경할 내용의 담당 문서를 찾는다.

### 과제

| 문서 | 담당 내용 |
| --- | --- |
| [과제 핵심 정리](docs/assignment/README.md) | 과제가 요구하는 조건, 제출물, 배점, 확인이 필요한 것 |
| [과제 소개](docs/assignment/UNIX시스템프로그래밍설계과제물소개%282026년%29.pdf) | 원문. 일정, 수행 방법, 주의 사항 |
| [설계과제 안내문](docs/assignment/UNIX시스템프로그래밍_설계과제_안내문.docx) | 원문. 1·2단계 요건, 측정 요건, 제한 요건, 배점 |
| [최종보고서 서식](docs/assignment/UNIX시스템프로그래밍_설계과제_최종보고서서식.docx) | 원문. 보고서 목차, 발표자료 구성 |

### 요구사항·설계·측정·검증

| 문서 | 담당 내용 |
| --- | --- |
| [요구사항](docs/requirements/README.md) | 범위, 용어, 기능, 제약, 완료 기준, 문서 배치 |
| [구현 계획](docs/design/plan.md) | 구조, 파일 책임, 일정, 미결정 |
| [설계 명세](docs/design/README.md) | 채널, 스레드, 공유 자원과 동기화, 실행 옵션 |
| [측정 방법](docs/measurement/README.md) | 타임스탬프 위치, 통계, 자원 사용량, 원자료 형식 |
| [1단계 측정 결과](docs/measurement/stage1.md) | 1단계 측정 기록, 결과, 해석과 확인할 가설 |
| [2단계 측정 결과](docs/measurement/stage2.md) | 2단계 측정 기록, 결과, 원인 확인 실험, 설계 개선 실험, 예측과 비교 |
| [IPC 선택 가이드라인](docs/measurement/guideline.md) | 측정 결과로 정리한 상황별 IPC 선택 (보고서 6.6절) |
| [테스트 명세](docs/verification/README.md) | 검증 항목과 판정 기준 |

### 협업

| 문서 | 담당 내용 |
| --- | --- |
| [작업 지침](AGENTS.md) | 읽는 순서, 문서별 역할, 코드·측정·기록 규칙 |
| [이슈·PR 작성 규칙](docs/CONTRIBUTING.md) | 이슈·브랜치·커밋·PR의 제목과 본문 작성 방법 |

### 발표·제출

| 문서 | 담당 내용 |
| --- | --- |
| [최종보고서·제출 준비 기준](docs/submission/report.md) | 제출물 점검, 보고서 절별 출처, 캡처 목록 |
| [최종 발표 준비 기준](docs/submission/presentation.md) | 발표·제출 조건, 발표자료 구성, 데모 시나리오 |

### 기록

| 문서 | 담당 내용 |
| --- | --- |
| [성능 예측](docs/records/prediction.md) | 워크로드 명세와 예측. 구현 전에 커밋하고 이후에는 덧붙이기만 한다 |
| [AI 활용 기록](docs/records/ai-log.md) | AI 사용 내용, AI가 틀린 사례, AI 코드의 결함 |
| [작업 시간](docs/records/worklog.csv) | 날짜, 이름, 한 일, 시간 |
