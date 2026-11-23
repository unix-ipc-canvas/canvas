# 최종보고서·제출 준비 기준 — 공유 캔버스 서버

[문서 안내](../../README.md#관련-문서)

최종보고서의 각 절을 어떤 자료로 채우는지와 제출물 점검을 담당한다. 제출 요건의 원본은 [과제 핵심 정리](../assignment/README.md#제출물), 보고서 목차의 원본은 [최종보고서 서식](../assignment/UNIX시스템프로그래밍_설계과제_최종보고서서식.docx)이다. 발표자료와 동영상은 [최종 발표 준비 기준](presentation.md)을 따른다.

## 제출물 점검

| 제출물 | 만드는 방법 | 확인 |
| --- | --- | --- |
| 최종보고서 | 서식의 목차와 항목 순서 그대로. 아래 [보고서 절별 출처](#보고서-절별-출처)에서 옮긴다 | 안내 문구 삭제, 그래프의 축 이름과 단위, 수치마다 측정 조건 |
| `osproject_2.tar` | `make dist` | 풀어서 `make` 통과, `make test` FAIL 0, `readme.txt` 빈칸 없음, `results/`에 원자료 있음 |
| 발표자료(PPT) | [발표자료 구성](presentation.md#발표자료-구성) | 쪽수 범위 |
| 발표 동영상 | PPT 진행 + 데모 화면 | 재생 확인. 용량이 크면 동영상만 이메일 제출 |

제출은 11월 30일(월), e-class 업로드 시각 기준이다.

## 보고서 절별 출처

보고서용으로 내용을 따로 관리하지 않고 아래 원본에서 옮긴다. 출처가 아직 비어 있으면 해당 절을 쓸 수 없다. 요구사항에 적혀 있다는 이유만으로 구현·측정 완료로 쓰지 않는다.

| 절 | 내용 | 출처 | 담당 |
| --- | --- | --- | --- |
| 표지 | 팀명, 팀 번호, 팀원(학번, 성명, 담당 IPC) | [역할 분담](../../README.md#역할-분담) | |
| 1.1 | 문제 정의, IPC 선택이 중요한 이유 | [요구사항 개요](../requirements/README.md#개요). 리눅스 화면 시스템(X11의 공유 메모리 확장, Wayland의 공유 메모리 버퍼) 사례는 출처를 직접 찾아 확인하고 인용한다 | |
| 1.2 | 기능·비기능 요구사항, 제약 조건 충족 방법 | [요구사항](../requirements/README.md) | |
| 2.1 | 측정 환경 | [측정 환경 기록](../measurement/README.md#측정-환경-기록)의 명령 출력, `results/bench_env.txt`, [1단계 측정 기록](../measurement/stage1.md#측정-기록) | |
| 2.2 | 벤치마크 설계 | [측정 방법](../measurement/README.md) | |
| 2.3, 2.4 | 1단계 결과와 해석 | [1단계 측정 결과](../measurement/stage1.md), [1단계 측정 요약(반복 20번)](../assets/bench/stage1-r20/summary.md)과 그래프(`results/stage1-r20/`에서 `scripts/plot-bench.py`가 만든다), [1단계 실험 비교](../assets/bench/experiments.md) | |
| 3 | 워크로드 명세와 예측 | [성능 예측](../records/prediction.md). 작성 당시 내용 그대로, 수정 기록은 구분해서 | |
| 4 | 설계도, 스레드, 공유 자원과 동기화 | [설계 명세](../design/README.md) | |
| 5.1 | 소스 주요 부분 | `src/transport/`의 생성·송수신·해제 코드, 락 코드 | |
| 5.2, 5.3 | 데모 시나리오와 실행 결과 | [데모 시나리오](presentation.md#데모-시나리오), `make test` 출력, `ipcs` 캡처 | |
| 6.1 | 제한 요소 반영 | [요구사항의 제약](../requirements/README.md#실행-환경과-제약), 메모리 절감 측정([shm 링 크기](../measurement/stage2.md#shm-요청-링을-줄이면-메모리가-줄고-처리량은-그대로다-n-5)) | |
| 6.2, 6.3 | 응용 측정 방법과 결과 | [측정 방법](../measurement/README.md#2단계-실험), [2단계 측정 결과](../measurement/stage2.md), [2단계 측정 요약](../assets/app/summary.md)과 그래프(`results/`에서 `scripts/plot-app.py`가 만든다) | |
| 6.4 | 예측 대비 실측, 원인 규명 | [2단계 측정 결과의 예측과 비교](../measurement/stage2.md#예측과-비교)와 [원인 확인 실험](../measurement/stage2.md#원인-확인-실험), `results/exp-app-*/` | |
| 6.5 | 설계 개선 전후 | [2단계 측정 결과의 설계 개선 실험](../measurement/stage2.md#설계-개선-실험-t6), `results/exp-app-pipesz*/`, `results/exp-pipesz-bench/`, `results/exp-app-shmmem/`(요약은 `scripts/compare-app.py`가 만든다) | |
| 6.6 | IPC 선택 가이드라인 (1쪽 이내) | [IPC 선택 가이드라인](../measurement/guideline.md) | |
| 7 | 팀워크, 역할, 작업 시간 | [역할 분담](../../README.md#역할-분담), [작업 시간](../records/worklog.csv), GitHub의 커밋·PR 기록 | |
| 8.1 | 결론 | 팀 회의 | |
| 8.2 | AI 활용, AI가 틀린 사례 2개 이상, AI 코드 결함 | [AI 활용 기록](../records/ai-log.md) | |
| 9 | 부록 | 소스 전체, Makefile, readme.txt, 스크립트, 원자료, AI 대화 기록(`docs/records/ai-chats/`) | |

## 캡처 목록

제출 직전에 한꺼번에 찍으면 빠뜨린다. 해당 단계에서 바로 찍는다. 캡처는 `docs/assets/captures/`에 둔다.

- [x] 측정 환경 명령 출력: [측정 환경](../assets/captures/stage1-env.png), [IPC 한도](../assets/captures/stage1-ipc-limits.png)
- [x] 1단계 측정 실행 결과: 처음 측정의 [`scripts/bench.sh` 출력 처음과 끝](../assets/captures/stage1-bench-run.png), 반복 20번 재측정과 실험의 [기록](../assets/captures/stage1-r20-run.png)
- [x] 세 버전 각각의 서버·클라이언트 실행 화면: [pipe](../assets/captures/stage2-run-pipe.png), [mq](../assets/captures/stage2-run-mq.png), [shm](../assets/captures/stage2-run-shm.png). 서버, 클라이언트 4개, 서버 종료의 체크섬과 검증기의 체크섬이 같다
- [x] 실행 중과 종료 뒤의 `ipcs`: 응용 세 버전을 함께 띄운 [실행 중과 종료 뒤](../assets/captures/stage2-ipcs.png). 1단계 벤치마크로 찍은 [실행 중](../assets/captures/stage1-ipcs-running.png), [측정 뒤](../assets/captures/stage1-ipcs-after.png)도 있다
- [x] `make`와 `make test` 출력: 응용을 넣은 뒤의 [make](../assets/captures/stage2-make.png), [make test](../assets/captures/stage2-make-test.png). 응용 전(1단계 시점, SKIP 15)의 [make](../assets/captures/stage1-make.png), [make test](../assets/captures/stage1-make-test.png)도 있다
- [x] 체크섬 일치와 불일치 장면: [락 있음은 일치, `-l none`은 불일치](../assets/captures/stage2-checksum.png)
- [x] 캔버스 이미지: [요청 120개로 그린 데모](../assets/captures/stage2-canvas-demo.png)(클라이언트 2, `-n 60 -m 100 -r 160`), [shm 실행 화면의 캔버스](../assets/captures/stage2-canvas.png)(클라이언트 4 × 기준 조건 20,000개라 값이 고르게 섞여 잡음처럼 보인다)
- [x] 2단계 측정 실행 기록: [서버 비교, 본 측정, 원인 확인 실험](../assets/captures/stage2-run-record.png), [설계 개선 실험](../assets/captures/stage2-t6-record.png)
- [ ] 주요 AI 대화 기록

## 제출 전 수동 확인

[테스트 명세의 수동 확인](../verification/README.md#수동-확인)을 따른다.
