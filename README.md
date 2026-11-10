# 공유 캔버스 서버

UNIX시스템프로그래밍 설계과제 (2026년 2학기). 여러 클라이언트가 하나의 캔버스에 동시에 그리는 서버를 pipe, message queue, shared memory 3가지로 구현하고 성능을 비교합니다.

- 제출: 11월 30일(월), e-class
- 처음 보는 팀원은 [팀 프로젝트 안내서](https://claude.ai/artifact/9rcWLcBnxL2cK6A4RLx5Lc)부터 읽으세요. 쉬운 말로 쓴 전체 설명입니다.

## 현재 상태

뼈대만 있습니다. 헤더 초안과 Makefile, 문서 틀이 들어 있고 구현 코드는 아직 없습니다.

| 단계 | 목표일 | 상태 |
| --- | --- | --- |
| transport.h, proto.h 확정 | 11/13 | 초안 |
| 1단계 기초 벤치마크와 측정 | 11/13 | 시작 전 |
| 워크로드 명세와 예측 커밋 | 11/16 | 시작 전 |
| 3버전 구현과 체크섬 검증 | 11/22 | 시작 전 |
| 응용 측정, 원인 분석, 설계 개선 | 11/26 | 시작 전 |
| 보고서, 발표자료, 동영상 | 11/30 | 시작 전 |

## 폴더

```
include/          proto.h (메시지 형식), transport.h (전송 계층 인터페이스)
src/transport/    transport_pipe.c, transport_mq.c, transport_shm.c  ← 각자 1개
src/common/       canvas.c, server.c, client.c, verify.c, util.c     ← 공통
src/bench/        bench.c (1단계 echo 벤치마크)
scripts/          실험 자동화, 그래프
results/          측정 원자료 CSV (제출물이므로 커밋)
docs/             design.md, prediction.md, ai-log.md, worklog.csv
readme.txt        제출용 readme
```

## 빌드

Linux, gcc, make가 필요합니다.

```bash
make            # 전체 (소스가 있는 것만 빌드)
make bench      # 1단계 벤치마크
make app        # 2단계 응용
make pipe       # pipe 버전만 (mq, shm 도 같음)
make check      # 헤더 검사 + 지금 빌드 가능한 대상 보기
make ipcclean   # 비정상 종료로 남은 IPC 자원 삭제
make dist       # 제출용 tar
```

팀 번호가 정해지면 `Makefile`의 `TEAM_NO`를 고칩니다. System V key가 60xxy 규칙으로 정해집니다.

## 작업 규칙

1. **맡은 파일만 고칩니다.** `include/`의 두 헤더는 확정 후 세 사람이 합의해야 바꿉니다.
2. **main에 바로 푸시하지 않습니다.** 브랜치(`pipe`, `mq`, `shm`, `server`, `client`, `bench` 등)에서 작업하고 PR로 합칩니다. 합치기 전에 `make`가 경고 없이 통과해야 합니다.
3. **설계 선택은 실행 옵션으로 만듭니다.** 원인 분석 실험은 옵션 하나만 바꿔 다시 돌리는 방식으로 합니다. 옵션 목록은 `docs/design.md`.
4. **측정은 측정용 컴퓨터 1대에서만 합니다.** 다른 컴퓨터에서 잰 숫자는 보고서에 쓰지 않습니다.
5. **예측은 구현 전에 커밋합니다.** `docs/prediction.md`를 채워 커밋한 뒤에 `server.c`, `client.c`를 시작합니다.

## 매번 적는 기록 3가지

| 파일 | 언제 | 무엇을 |
| --- | --- | --- |
| `docs/worklog.csv` | 작업할 때마다 | 날짜, 이름, 한 일, 시간. 보고서에 투입 시간을 표시해야 합니다 |
| `docs/ai-log.md` | AI를 쓸 때, AI가 틀린 걸 발견했을 때 | 프롬프트 요약, 틀린 내용, 확인 방법. 틀린 사례가 최소 2개 필요합니다 |
| `docs/prediction.md` | 11/16까지 한 번 | 워크로드 명세와 예측. 커밋 후에는 덧붙이기만 합니다 |

## 팀

| 이름 | 학번 | IPC | 공통 담당 |
| --- | --- | --- | --- |
| | | pipe | |
| | | message queue | |
| | | shared memory | |

측정용 컴퓨터: (CPU, 코어 수, 메모리, 배포판과 커널, gcc 버전, 가상화 여부를 여기에 적기)
