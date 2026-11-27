# 다중 그림판 공유 캔버스 서버

![공유 캔버스 서버](docs/assets/social-preview.png)

UNIX 시스템 프로그래밍 2팀

![C](https://img.shields.io/badge/C-gnu11-A8B9CC?style=flat-square&logo=c&logoColor=white)
![Linux](https://img.shields.io/badge/Linux-POSIX-FCC624?style=flat-square&logo=linux&logoColor=black)
![GNU Make](https://img.shields.io/badge/build-GNU%20Make-A42E2B?style=flat-square&logo=gnu&logoColor=white)
![IPC](https://img.shields.io/badge/IPC-pipe%20%C2%B7%20message%20queue%20%C2%B7%20shared%20memory-2563EB?style=flat-square)

여러 사람이 한 컴퓨터 안에서 같은 그림판에 동시에 그리는 협업 그림판 서버다. 서버는 그림판 여러 개를 두고, 클라이언트 최대 1024개가 작은 그리기 명령(DRAW)을 보내고 보는 영역의 그림(SNAPSHOT, 최대 64KB)을 받아 간다. 같은 응용을 pipe(FIFO), message queue, shared memory 3버전으로 구현해, 1단계 측정으로 세운 예측이 응용에서도 맞는지 검증하고 설계를 개선한다. 2026년 2학기 UNIX시스템프로그래밍 설계과제다.

> [!NOTE]
> 11/27에 2단계(응용 기획부터 제출물까지)를 처음부터 다시 수행했다. 1단계(벤치마크 구현, 측정, 해석)는 그대로다. 이유와 범위는 [요구사항](docs/requirements/README.md)의 머리말에 있다.

```text
canvas/
├── AGENTS.md         # 작업 지침
├── Makefile
├── readme.txt        # 제출용 readme
├── include/          # proto.h, transport.h, util.h, canvas.h, workload.h
├── src/
│   ├── transport/    # IPC별 전송 계층 (pipe, mq, shm, 벤치마크 전용 익명 pipe upipe)
│   ├── common/       # 그림판, 서버, 클라이언트, 그리기 클라이언트, 실행기, 검증기
│   ├── bench/        # 1단계 echo 벤치마크
│   └── test/         # 전송 계층 시험 도구
├── scripts/          # 검증·측정·그래프 스크립트
├── results/          # 측정 원자료 CSV
└── docs/             # 과제, 요구사항, 설계, 측정, 검증, 제출, 기록
```

## 역할 분담

IPC는 한 사람이 하나씩 맡고, 공통 코드·보고서·발표는 셋으로 나눈다. 자세한 것은 [구현 계획](docs/design/plan.md#역할-분담)에 있다.

| | A: pipe + 서버 | B: mq + 클라이언트 | C: shm + 측정 |
| --- | --- | --- | --- |
| 팀원 | 김병수 ([@pinoba](https://github.com/pinoba)) | 오현근 ([@hyungunoh321](https://github.com/hyungunoh321)) | 이동건 ([@Movinggun-bit](https://github.com/Movinggun-bit)) |
| IPC | `transport_pipe.c` | `transport_mq.c` | `transport_shm.c`, `transport_upipe.c` |
| 공통 코드 | `canvas.c`, `server.c` | `client.c`, `draw.c`, `verify.c`, `workload.c` | `loadgen.c`, `util.c`, `bench.c`, 측정 스크립트 |

## 기술 스택

C(gnu11), gcc, GNU make, POSIX 스레드. IPC는 FIFO, System V message queue, System V shared memory와 semaphore다. 외부 라이브러리는 쓰지 않는다. 측정 그래프에만 Python 3와 matplotlib를 쓴다.

## 시작하기

Linux에서 gcc와 make만 있으면 된다.

```bash
git clone https://github.com/unix-ipc-canvas/canvas.git
cd canvas
make            # 전체 빌드
make test       # 검증 (1~3분)
```

| 명령 | 동작 |
| --- | --- |
| `make` | 전체 빌드 |
| `make bench` | 1단계 벤치마크 4종 (`pipe`, `mq`, `shm`, 익명 `upipe`) |
| `make app` | 2단계 응용 3종(서버, 부하 클라이언트, 그리기 클라이언트)과 실행기·검증기 |
| `make pipe` / `make mq` / `make shm` | 그 IPC 버전만 |
| `make ipcclean` | 비정상 종료로 남은 IPC 자원과 FIFO 삭제 |
| `make dist` | 제출용 `osproject_2.tar` 만들기 |

## 사용 방법

`bin/`에 `bench_<ipc>`, `server_<ipc>`, `client_<ipc>`, `draw_<ipc>`, `loadgen`, `verify`가 생긴다(`<ipc>`는 `pipe`, `mq`, `shm`). 옵션의 뜻은 [설계 명세의 실행 옵션](docs/design/README.md#실행-옵션)에 있다.

### 1단계 벤치마크

한 번 실행하면 서버 1개와 클라이언트 N개를 띄워 조건 하나를 재고, 결과 한 줄을 화면(stderr)에 낸다. `-o`를 주면 CSV에 두 줄(client, server)을 덧붙인다. 측정 방법은 [1단계 벤치마크](docs/measurement/README.md#1단계-벤치마크)를 따른다.

```bash
make bench
./bin/bench_shm -c 4 -s 65536 -n 10000            # 클라이언트 4개, 64KB, 지연 (동시 요청 1)
./bin/bench_shm -c 4 -s 65536 -n 10000 -f 16      # 동시 요청 16개, 처리량
scripts/bench.sh                                  # 1단계 전체 측정 → results/
python3 scripts/plot-bench.py results/stage1-r20 docs/assets/bench/stage1-r20
```

### 2단계 응용

2단계 응용의 실행 예시는 구현하면서 이 절에 적는다.

## 테스트

```bash
make test
```

문서 검사와 빌드·동작 검사를 실행하고 항목마다 PASS, FAIL, SKIP을 출력한다. 항목과 판정 기준은 [테스트 명세](docs/verification/README.md)를 따른다. 우리 서버가 떠 있지 않을 때 돌린다.

## 관련 문서

문서는 일의 순서대로 폴더를 나눴다: 과제 → 요구사항 → 설계 → 측정 → 검증 → 제출. 덧붙이기만 하는 기록은 `records`에 둔다. 작업 원칙은 [작업 지침](AGENTS.md)을 따른다.

| 문서 | 담당 내용 |
| --- | --- |
| [과제 핵심 정리](docs/assignment/README.md) | 과제 조건, 제출물, 배점 (원문 3개 포함) |
| [요구사항](docs/requirements/README.md) | 문제 정의, 범위, 기능·비기능 요구사항, 제약, 완료 기준 (보고서 1장) |
| [워크로드 명세와 성능 예측](docs/records/prediction.md) | 구현 전에 커밋한 워크로드와 예측 (보고서 3장) |
| [설계 명세](docs/design/README.md) | 구조, 채널, 스레드, 공유 자원과 동기화, 실행 옵션, 메모리 (보고서 4장) |
| [구현 계획](docs/design/plan.md) | 파일 책임, 역할, 일정, 리스크와 미결정 |
| [측정 방법](docs/measurement/README.md) | 타임스탬프 위치, 통계, 1·2단계 실험, 원자료 형식 |
| [1단계 측정 결과](docs/measurement/stage1.md) | 1단계 측정 기록, 결과, 해석 (보고서 2장) |
| [테스트 명세](docs/verification/README.md) | 검증 항목과 판정 기준 |
| [제출 준비](docs/submission/README.md) | 제출물, 보고서 절별 출처, 캡처 목록 |
| [AI 활용 기록](docs/records/ai-log.md) | AI 사용 내용, AI가 틀린 사례, AI 코드의 결함 (보고서 8.2절) |
| [작업 시간](docs/records/worklog.csv) | 날짜, 이름, 한 일, 시간 |
| [이슈·PR 작성 규칙](docs/CONTRIBUTING.md) | 커밋·브랜치·이슈·PR 형식 |
