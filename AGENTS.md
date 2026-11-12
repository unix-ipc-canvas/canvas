# 작업 지침 — 공유 캔버스 서버

사람과 AI 도구 모두에게 적용한다.

## 읽는 순서와 문서 역할

작업 전 [요구사항](docs/requirements/README.md) → [설계 명세](docs/design/README.md) → [구현 계획](docs/design/plan.md) 순으로 읽는다. 충돌하면 요구사항을 우선한다.

| 문서 | 담당 내용 |
| --- | --- |
| `docs/assignment/` | 과제 원문 3개와 핵심 정리(조건, 제출물, 배점). 정리와 원문이 다르면 원문이 우선이다 |
| `docs/requirements/README.md` | 우리 응용의 범위·용어·요구사항·완료 기준, 문서 배치 |
| `docs/design/plan.md` | 구조, 파일 책임, 담당, 단계별 계획, 미결정 |
| `docs/design/README.md` | 채널, 스레드, 동기화, 실행 옵션, 종료 |
| `docs/measurement/README.md` | 타임스탬프 위치, 통계, 원자료 형식 |
| `docs/verification/README.md` | 검증 범위·방법·판정 기준 |
| `docs/submission/` | 보고서 절별 출처와 제출 점검, 발표자료 구성과 데모 시나리오 |
| `docs/records/` | 성능 예측, AI 활용 기록, 작업 시간. 덧붙이기만 한다 |
| `docs/CONTRIBUTING.md` | 커밋·브랜치·이슈·PR 형식 |
| `README.md` | 빌드·실행·테스트 명령, 문서 목록 |

## 변경 원칙

- 요구사항·계획·설계는 구현 전에 갱신한다. 문서와 어긋나는 변경은 코드보다 담당 문서를 먼저 고친다.
- 문서는 짧게 쓰고, 같은 내용은 담당 문서 한 곳에만 둔 뒤 나머지는 링크한다. 문서를 옮기거나 제목을 바꾸면 참조 링크와 앵커도 함께 고친다.
- README 순서는 소개 → 기술 스택 → 시작하기 → 사용 방법 → 테스트 → 관련 문서다. 새 문서는 [문서 배치](docs/requirements/README.md#문서-관리)에 맞는 폴더에 두고 [관련 문서](README.md#관련-문서)에 등록한다. 과제 원문 파일은 수정하지 않는다.
- 완료 이력은 문서에 쌓지 않는다. 미확정 항목에는 대기 이유와 재개 조건만 남긴다. 미확정 값을 추정해 채우지 않는다.
- 테스트 범위·방법·판정 기준은 [테스트 명세](docs/verification/README.md)에서 관리한다. 완료 기준은 요구사항, 실행 명령은 루트 README에 둔다.

## 코드 규칙

- `include/proto.h`, `include/transport.h`는 세 사람이 합의한 PR로만 바꾼다.
- `src/common/`, `src/bench/`에는 IPC 고유 호출(`msgsnd`, `shmat`, `mkfifo` 등)을 넣지 않는다. `transport.h`의 함수만 부른다.
- 락 방식 같은 설계 선택은 실행 옵션으로 만든다. 실험을 위해 코드를 고쳐 다시 빌드하는 방식을 쓰지 않는다.
- 만든 IPC 자원은 모든 종료 경로에서 지운다. key는 `IPC_KEY(y)`로만 만든다.
- 측정 구간 안에 출력과 `malloc`을 넣지 않는다.
- 명세에 없는 결정은 가장 단순한 쪽으로 하고 `/* DECISION: ... */`로 남긴 뒤 PR에서 알린다.
- 합치기 전에 `make test`에서 FAIL이 0이어야 한다. 요구사항을 추가하면 테스트 명세에 대응 항목을, 테스트 항목을 추가하면 `scripts/verify.sh`에 같은 번호를 넣는다. 문서 검사가 이를 확인한다.

## 측정과 기록

- `results/`에는 측정용 컴퓨터에서 스크립트가 만든 원자료만 둔다. 수치를 지어내거나 손으로 고치지 않는다. AI가 제시한 수치를 측정값으로 쓰지 않는다.
- `docs/records/prediction.md`는 커밋한 뒤 위쪽을 고치지 않는다. 바뀐 생각은 맨 아래 수정 기록에 덧붙인다. 예측을 커밋하기 전에는 `server.c`, `client.c`를 만들지 않는다.
- AI 도구를 쓰면 `docs/records/ai-log.md`에 단계, 프롬프트 요약, 결과를 적는다. AI의 설명이나 코드가 틀린 것을 발견하면 같은 날 적는다. 과제의 필수 기록이다.
- `docs/records/worklog.csv`의 시간은 작업한 사람이 직접 적는다. 추정해서 채우지 않는다.

## 커밋·이슈·PR

형식은 [이슈·PR 작성 규칙](docs/CONTRIBUTING.md)을 따른다. 커밋 메시지, PR, 이슈에 AI 도구의 서명(`Co-Authored-By`, 생성 도구 표시, 세션 링크)을 넣지 않는다. AI 사용 사실은 `docs/records/ai-log.md`에만 기록한다.
