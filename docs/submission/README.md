# 제출 준비 — 다중 그림판 공유 캔버스 서버

[문서 안내](../../README.md#관련-문서)

제출물, 보고서 절별 출처, 캡처 목록을 관리한다. 과제가 정한 제출물과 배점의 원본은 [과제 핵심 정리](../assignment/README.md)다.

## 제출물

| 제출물 | 파일 | 만드는 방법 |
| --- | --- | --- |
| 최종보고서 | `docs/submission/최종보고서.docx` | [최종보고서 서식](../assignment/UNIX시스템프로그래밍_설계과제_최종보고서서식.docx)의 목차와 항목 순서 그대로 |
| 소스 | `osproject_2.tar` | `make dist` (`tar cvf osproject_2.tar ...`) |
| 발표자료 | `docs/submission/발표자료.pptx` | 서식의 [별첨] 구성 그대로 |
| 발표 대본 | `docs/submission/발표대본.md` | 슬라이드마다 |
| 데모·발표 동영상 | 저장소 밖(용량) | 측정용 컴퓨터의 실제 화면을 녹화 |

## 보고서 절별 출처

| 절 | 원본 |
| --- | --- |
| 1장 | [요구사항](../requirements/README.md) |
| 2장 | [측정 방법](../measurement/README.md), [1단계 측정 결과](../measurement/stage1.md) |
| 3장 | [워크로드 명세와 성능 예측](../records/prediction.md) |
| 4장 | [설계 명세](../design/README.md) |
| 5장 | 소스 코드, [README의 사용 방법](../../README.md#사용-방법), 캡처 |
| 6장 | [측정 방법](../measurement/README.md#2단계-실험), [2단계 측정 결과](../measurement/stage2.md) |
| 7장 | [구현 계획의 역할 분담](../design/plan.md#역할-분담), [작업 시간](../records/worklog.csv) |
| 8장 | [AI 활용 기록](../records/ai-log.md) |

## 캡처 목록

1단계 캡처는 1단계 측정 때 찍은 그대로다(`docs/assets/captures/stage1-*.png`). 2단계 캡처는 측정용 컴퓨터의 실제 화면에서 본 측정 창과 데모 녹화 중에 찍었다(`stage2-*.png`).

| 파일 | 내용 | 보고서 |
| --- | --- | --- |
| `stage1-env.png` | 측정 환경 명령 출력 | 2.1절 |
| `stage1-ipc-limits.png` | IPC 한도 | 2.1절 |
| `stage1-bench-run.png`, `stage1-r20-run.png` | 1단계 측정 실행 화면 | 2.2절 |
| `stage1-ipcs-running.png`, `stage1-ipcs-after.png` | 측정 중·후 `ipcs` | 2.2절 |
| `stage1-make.png`, `stage1-make-test.png` | 1단계 때의 `make`, `make test` | 부록 |
| `stage2-meas-run.png` | 2단계 본 측정 창(마지막 실험과 남은 자원 검사) | 6.3절 |
| `stage2-make.png`, `stage2-make-test.png` | 2단계 `make`, `make test`(PASS 75 / FAIL 0 / SKIP 0) | 5.3절 |
| `stage2-demo-draw.png` | 그리기 클라이언트 2개가 같은 그림을 보는 화면 | 5.3절, 발표자료 |
| `stage2-demo-bots.png`, `stage2-ipcs-running.png` | 봇 1022개 + 그리기 2개(접속 1024)와 실행 중 `ipcs` | 5.3절, 발표자료 |
| `stage2-ipcs-after.png` | 서버 종료(체크섬)와 종료 뒤 `ipcs`(남은 자원 0개) | 5.3절, 발표자료 |
| `stage2-demo-check.png` | `scripts/demo-check.sh`: 세 버전 × 클라이언트 1024개 | 5.3절 |
