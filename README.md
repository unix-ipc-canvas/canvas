# 공유 캔버스 서버

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
│   ├── transport/    # IPC별 전송 계층 (pipe, mq, shm)
│   ├── common/       # 캔버스, 서버, 클라이언트, 검증기
│   └── bench/        # 1단계 echo 벤치마크
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
    └── CONTRIBUTING.md # 이슈·PR 작성 규칙
```

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
> 지금은 헤더 초안, Makefile, 검증 스크립트, 문서만 있고 구현 코드가 없다. `make`는 통과하지만 실행 파일을 만들지 않는다.

## 사용 방법

구현되면 `bin/`에 `bench_<ipc>`, `server_<ipc>`, `client_<ipc>`, `verify`가 생긴다(`<ipc>`는 `pipe`, `mq`, `shm`). 옵션의 초안은 [실행 옵션](docs/design/README.md#실행-옵션)에 있다. 확정된 실행 예시는 구현한 사람이 이 절에 적는다.

## 테스트

```bash
make test
```

문서 검사(링크, 요구사항과 테스트의 대응)와 빌드·동작 검사를 실행하고 항목마다 PASS, FAIL, SKIP을 출력한다. 항목과 판정 기준은 [테스트 명세](docs/verification/README.md)를 따른다. SKIP은 검증 완료가 아니다.

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
| [구현 계획](docs/design/plan.md) | 구조, 파일 책임, 담당, 일정, 미결정 |
| [설계 명세](docs/design/README.md) | 채널, 스레드, 공유 자원과 동기화, 실행 옵션 |
| [측정 방법](docs/measurement/README.md) | 타임스탬프 위치, 통계, 자원 사용량, 원자료 형식 |
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
