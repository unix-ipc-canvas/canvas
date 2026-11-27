# 측정 방법 — 공유 캔버스 서버

[문서 안내](../../README.md#관련-문서)

1단계 벤치마크와 2단계 응용을 같은 방법으로 잰다. 과제가 요구하는 조건의 원본은 [측정 방법 필수 요건](../assignment/README.md#측정-방법-필수-요건)이고, 이 문서는 우리가 그것을 어떻게 지키는지 정한다. 보고서 2.2절과 6.2절은 이 문서에서 옮긴다.

> [!NOTE]
> 1단계 측정 방법은 1단계 때 확정한 그대로다. 2단계는 11/27 재수행에서 [2단계 실험](#2단계-실험) 절을 새 워크로드에 맞춰 다시 정했다.

## 원칙

- 모든 수치는 측정용 컴퓨터 1대에서 스크립트로 잰 값이다. 손으로 옮겨 적거나 다른 컴퓨터의 값을 섞지 않는다.
- 한 번에 요인 하나만 바꾼다. 바꾼 옵션과 고정한 옵션을 원자료에 함께 남긴다.
- 측정에 쓴 커밋을 기록한다. 코드를 고친 뒤의 값과 섞이지 않게 한다.

## 측정용 컴퓨터

이동건의 PC 1대에서 모든 수치를 잰다. 네이티브 Linux라 가상머신·WSL2보다 측정 잡음이 적다.

| 항목 | 값 |
| --- | --- |
| CPU | Intel Core 5 120U, 논리 CPU 12개 (P코어 2개 × 하이퍼스레딩 = CPU 0~3, 최대 5.0GHz / E코어 8개 = CPU 4~11, 최대 3.8GHz) |
| 메모리 | 14GiB |
| OS, 커널 | Ubuntu 26.04.1 LTS, 가상화 없음. 커널은 1단계 측정 때 7.0.0-43-generic, 2단계 측정 때 7.0.0-44-generic(그사이 배포판 업데이트) |
| gcc | 15.2.0 |
| CPU 주파수 설정 | `powersave` (intel_pstate 기본값). 바꾸지 않는다 |

- P코어와 E코어가 섞인 CPU다. 스레드가 어느 코어에 놓이는지에 따라 값이 흔들릴 수 있다. `taskset` 으로 고정하지 않고 운영체제 배치 그대로 재며, 실행 사이의 편차로 그 영향을 본다.
- 측정하는 동안 다른 무거운 작업(시뮬레이션, 브라우저 등)은 멈춘다. Claude 데스크톱 앱처럼 끌 수 없는 프로그램은 측정 환경 기록에 남긴다.

## 측정 환경 기록

측정용 컴퓨터에서 아래를 실행해 보고서 2.1절 표에 옮긴다. `scripts/bench.sh` 가 측정을 시작할 때 같은 내용과 IPC 한도를 `results/bench_env.txt` 에 남기고, 측정 시작과 끝에 CPU를 쓰는 프로세스(`top`)도 함께 남긴다. 끌 수 없는 프로그램은 거기서 확인한다. 출력은 영어 기준이라 `LC_ALL=C`를 붙인다(한국어 설정에서는 `lscpu`의 항목 이름이 한국어로 나와 아래 `grep`이 빈 결과를 낸다).

```bash
LC_ALL=C lscpu | grep -E 'Model name|^CPU\(s\)|Thread|Core'
LC_ALL=C free -h | head -2
. /etc/os-release && echo "$PRETTY_NAME"; uname -r
gcc --version | head -1
systemd-detect-virt        # none 이면 네이티브
```

## 타임스탬프 위치

시각은 모두 `clock_gettime(CLOCK_MONOTONIC, ...)`으로 잰다. 같은 컴퓨터의 프로세스는 같은 시계를 쓰므로 클라이언트와 서버의 시각을 직접 뺄 수 있다.

```mermaid
sequenceDiagram
    participant CS as 클라이언트 송신
    participant SR as 서버 수신
    participant W as 서버 워커
    participant CR as 클라이언트 수신
    Note over CS: t0
    CS->>SR: 요청 (요청 채널)
    Note over SR: t_recv
    SR->>W: 작업 큐
    Note over W: t_deq
    Note over W: 락 획득 뒤 t_lock
    Note over W: 처리 뒤 t_send
    W->>CR: 응답 (응답 채널)
    Note over CR: t5
```

| 시각 | 찍는 곳 | 이유 |
| --- | --- | --- |
| `t0` | 클라이언트, `tp_send_req` 호출 직전 | 요청 생성 비용을 빼고 전송부터 잰다 |
| `t_recv` | 서버 수신 스레드, `tp_recv_req`가 돌아온 직후 | 요청 전송 구간의 끝 |
| `t_deq` | 워커, 작업 큐에서 꺼낸 직후 | 큐 대기와 스레드 깨우기 비용을 분리한다 |
| `t_lock` | 워커, 필요한 락을 모두 잡은 직후 | 락 대기를 분리한다 |
| `t_send` | 워커, `tp_send_resp` 호출 직전 | 처리 구간의 끝이자 응답 전송의 시작 |
| `t5` | 클라이언트 수신 스레드, `tp_recv_resp`가 돌아온 직후 | 응답을 통째로 받은 시점 |

| 구간 | 계산 |
| --- | --- |
| 요청 전송 | `t_recv - t0` |
| 큐 대기 | `t_deq - t_recv` |
| 락 대기 | `t_lock - t_deq` |
| 처리 | `t_send - t_lock` |
| 응답 전송 | `t5 - t_send` |
| 왕복 지연 | `t5 - t0` |

1단계 벤치마크는 서버 쪽 구간이 없으므로 `t0`과 `t5`만 쓴다. 작은 메시지에서는 시계를 읽는 비용이 결과에 섞인다. `clock_gettime`을 연달아 불러 그 비용을 따로 재고 보고서에 함께 쓴다.

## 반복과 통계

- 조건마다 워밍업 요청을 먼저 보내고 버린 뒤 잰다.
- 지연은 요청 하나를 표본 하나로 본다. 상위 99%가 의미를 가지려면 표본이 수천 개는 있어야 한다.
- 보고하는 값은 평균, 중앙값, 상위 99%, 최대다. 백분위수는 nearest-rank 방식이다(작은 쪽부터 ceil(q × n)번째 값, `util.c`의 `lat_stats`).
- 과제의 "30회 이상"은 표본 수로 보면 충분히 넘는다. 1단계는 같은 조건을 20번, 2단계는 30번 실행하고, 실행마다 낸 값(평균, 중앙값, 상위 99%, 최대, 처리량)의 중앙값을 그래프와 표에 쓴다. 실행 사이의 편차((최대 - 최소) / 중앙값)를 요약표에 함께 낸다.
- IPC 사이의 순위는 실행별 값에서 좋은 쪽과 나쁜 쪽을 4분의 1씩 뺀 범위(20번이면 가운데 10번, 30번이면 가운데 15번)가 겹치지 않을 때만 매긴다. 겹치면 차이가 실행 사이의 흔들림보다 작은 것이므로 `≈`로 묶는다(`scripts/plot-bench.py`). 실행 하나가 튀어도 순위가 뒤집히지 않고, 흔들림보다 작은 차이로 순위를 매기지도 않게 하려는 것이다.

## 1단계 벤치마크

`src/bench/bench.c` 하나가 각 IPC에 링크되어 `bin/bench_<ipc>` 가 된다. 한 번 실행하면 조건 하나를 재고 CSV에 두 줄(client, server)을 붙인다. 실행 옵션은 [README의 사용 방법](../../README.md#사용-방법)에 있다.

| 항목 | 정한 것 | 이유 |
| --- | --- | --- |
| 구조 | 조정 프로세스가 서버 1개와 클라이언트 N개를 `fork` 한다. 서버는 받은 메시지를 같은 크기로 돌려준다(echo) | 실행 한 번이 조건 하나가 되어 스크립트로 반복하기 쉽다 |
| 서버 처리 | 받은 스레드에서 바로 돌려준다. 작업 큐·워커를 거치지 않는다 | 스레드 깨우기 비용이 섞이면 IPC 자체의 차이가 가려진다. 큐 비용은 2단계 구간별 시간으로 잰다 |
| 스레드 | 서버 echo·admin·stats + main, 클라이언트 send·recv·stats + main | 과제 조건(프로세스마다 3개 이상)을 벤치에도 지킨다 |
| HELLO | 측정 메시지와 같은 크기로 보낸다 | pipe 버전은 `PIPE_BUF` 이하 요청을 잠금 없이 쓰므로 크기가 섞이면 요청 스트림이 섞인다 |
| 측정 시작 | 모든 클라이언트가 워밍업 응답을 다 받은 뒤 조정 프로세스가 실시간 신호로 한꺼번에 시작시킨다 | 클라이언트 수별 측정에서 모두가 같은 시간대에 경쟁하게 한다 |
| IPC | pipe(FIFO), upipe(익명 pipe), mq, shm | 강의 5장의 pipe 두 가지를 모두 잰다. upipe는 채널 구조가 FIFO 버전과 같고 이름이 있는지만 다르다. 세 IPC의 순위 비교는 pipe, mq, shm으로 한다 |
| 메시지 크기 | 64B, 1KB, 4KB, 16KB, 64KB, 256KB, 1MB (payload) | 과제 최소 5단계에 `PIPE_BUF`(4KB)와 `msgmax`(8KB) 앞뒤를 더했다 |
| 클라이언트 수 | 1, 2, 4, 8 | 과제 조건 |
| 표본 수 (클라이언트 합) | 16KB 이하 20,000, 64KB 10,000, 256KB 4,000, 1MB 1,000 | 큰 메시지는 실행 시간을 줄이고, 작은 메시지는 상위 99%가 뜻을 갖게 |
| 워밍업 (클라이언트마다) | 16KB 이하 500, 64KB 200, 256KB 100, 1MB 30 | |
| 동시 요청 | 지연 실행 1, 처리량 실행 16 | 처리량은 요청을 쌓아 두고 보내야 IPC 한계가 드러난다 |
| 반복 | 같은 조건 20번. 반복마다 IPC 순서를 돌린다 | 처음 5번으로 잰 결과에서 16KB 이하의 실행 사이 편차가 9~85%로 IPC 사이의 차이보다 컸다. 반복을 늘려 중앙값과 순위를 믿을 만하게 한다. 시간에 따른 변화(발열, 다른 작업)가 한 IPC에만 몰리지 않게 순서를 돌린다 |
| 결과 모으기 | 지연은 모든 클라이언트의 표본을 합쳐 통계를 낸다. 처리량은 (잰 메시지 수) / (가장 먼저 시작한 시각부터 가장 늦게 끝난 시각까지) | 클라이언트마다의 백분위수를 평균하면 전체 분포의 백분위수가 아니다 |
| 자원 사용량 | client 줄은 모든 클라이언트의 합(VmRSS·VmHWM은 가장 큰 값), server 줄은 서버 하나. 둘 다 측정 구간만 | |
| clock_gettime 비용 | 100만 번 연달아 불러 한 번의 비용을 잰다 (`bench -K`, `results/bench_clock.csv`) | 작은 메시지의 지연에 섞이는 측정 비용을 보고서에 함께 쓴다 |

`scripts/bench.sh` 가 위 조합을 모두 돌린다. 커밋하지 않은 코드 변경이 있으면 멈춘다. 그래프와 요약표는 `scripts/plot-bench.py` 가 `docs/assets/bench/` 에 만든다. 측정 기록과 해석은 [1단계 측정 결과](stage1.md)에 있다.

## 2단계 실험

기준 조건은 [워크로드 명세](../records/prediction.md#워크로드-명세)를 따르고, 예측(P1~P8)마다 그것을 확인할 실험을 둔다. 실험은 기준에서 요인 하나만 바꾼다. 원인 확인 실험과 설계 개선 실험은 예측과 비교한 뒤 [2단계 측정 결과](stage2.md)에 덧붙인다.

| 실험 (`-e`) | 바꾸는 요인 | 고정한 것 | 반복 | 확인하는 예측 |
| --- | --- | --- | --- | --- |
| `type` | 요청 종류와 크기: DRAW만 16 × 16 / SNAPSHOT만 한 변 16, 64, 128, 256 | 클라이언트 1, 동시 요청 1, 그림판 1 | 30 | P1, P2, P7 |
| `thr` | (기준 혼합의 처리량) | 클라이언트 1, 동시 요청 16, 그림판 1 | 30 | P3 |
| `scale` | 클라이언트 1, 4, 16, 64, 256, 1024 × 기준 혼합 / DRAW만 | 동시 요청 1, 그림판 8 | 30 | P4, P5, P7 |
| `lock` | 락 `rw` / `room` × 클라이언트 16, 64, 256 | 기준 혼합, 동시 요청 1, 그림판 1 | 30 | P6 |
| `target` | (사람 속도 부하) 클라이언트마다 `-H 67`, 동시 요청 4 | 클라이언트 1024, 그림판 8 | 30 | P8, 요구사항 N-2 |

원인 확인과 설계 개선 실험(11/28 추가). 구현을 마친 뒤 시험 삼아 돌린 측정(원자료로 쓰지 않음)에서 클라이언트 1024개의 mq·shm 처리량이 64개일 때보다 크게 떨어져, 그 원인을 가리는 실험과 개선 전후 실험을 본 측정에 함께 넣었다. 개선은 서버 환경 변수로 켜고 끄므로 같은 실행 파일로 잰다(기본값 = 처음 설계).

| 실험 (`-e`) | 바꾸는 요인 | 고정한 것 | IPC | 확인하는 것 |
| --- | --- | --- | --- | --- |
| `cause-procs` | 클라이언트 64 × 동시 요청 16 / 256 × 4 / 1024 × 1 | 띄운 요청 수 1024, 기준 혼합, 그림판 8 | 셋 | 떨어지는 원인이 요청 양인지, 기다리는 프로세스 수인지 |
| `imp-mq-q` | mq 응답 큐 수 Q = 1, 2, 4, 8 (`OSP_MQ_RESP_Q`) | 클라이언트 1024, 기준 혼합, 동시 요청 1 | mq | 응답 큐 하나에 수신자가 몰리는 것이 원인인지 + 개선 1 |
| `imp-mq-req` | 요청 큐 용량 16,384B / 8,192B(`OSP_MQ_REQ_QBYTES`) × 입장 제한 없음 / 있음(`OSP_MQ_ADMIT=1`) | 응답 큐 8개, 클라이언트 256(용량 8,192B면 띄운 요청 > 큐에 들어가는 요청), 1024 | mq | 응답 큐를 나눈 뒤 드러난 요청 큐 병목의 원인(가득 찬 큐에서 막힌 송신자를 커널이 한꺼번에 깨움) + 개선 2 |
| `imp-scale` | 처음 설계(클라이언트 1024) / 개선한 설계(클라이언트 1, 64, 256, 1024) | 기준 혼합, 동시 요청 1, 그림판 8 | mq, shm | 개선 전후(요구사항 N-6), 클라이언트가 적을 때 손해가 없는지 |
| `imp-target` | (사람 속도 부하) 개선한 설계 | `target`과 같음 | mq, shm | 개선 뒤 요구사항 N-2 |
| `mem-shm` | shm 응답 링 칸 수 34(응답 2개) / 17(응답 1개) × 동시 요청 1, 4 | 클라이언트 1024, 개선한 요청 링 | shm | 메모리 절감 방안 2(요구사항 N-5, [구현 계획의 미결정](../design/plan.md#리스크와-미결정)) |
| `cause-workers` (본 측정 뒤 추가) | 서버 워커 수 1, 2, 4, 8 (`-w`) | `thr`와 같음(클라이언트 1, 동시 요청 16, 기준 혼합, 그림판 1) | 셋 | P3(처리량 shm 1위)가 틀린 원인: 1단계 echo에는 없던 워커끼리의 응답 채널 경쟁인지 |
| `cause-lockw` (본 측정 뒤 추가) | 서버 워커 수 1, 4, 16 (`-w`) | `lock`의 그림판 전체 mutex 조건(그림판 1, 클라이언트 64, 동시 요청 1, 기준 혼합) | 셋 | P6(락 대기 30% 이상)가 틀린 원인: 락을 다투는 스레드가 워커 수(4)로 묶여 있기 때문인지 |
| `cause-mix` (본 측정 뒤 추가) | 요청 종류: DRAW만(`-m 100`) / SNAPSHOT만(`-m 0`) | `thr`와 같음 | 셋 | P3가 틀린 원인: 작은 요청과 64KB 요청의 비용이 IPC마다 어떻게 다른지 |
| `cause-ring` (본 측정 뒤 추가) | shm 응답 링 칸 수 34·68·136 (`OSP_SHM_RESP_CHUNKS`) | `thr`와 같음 | shm | P3가 틀린 원인 후보: 응답 링에 64KB 응답이 2개만 들어가서인지 |

본 측정 뒤에 추가한 네 실험은 기본 목록에 없어 `EXPS="cause-workers cause-lockw cause-mix cause-ring" scripts/app-bench.sh`로 따로 돌린다. 요청 하나당 시스템 콜 수는 `scripts/syscall-count.sh 20000 -m 90 -f 16`이 서버와 클라이언트를 `strace -f -c`로 감싸 센다(결과 `results/app/syscalls.txt`, strace가 느리게 하므로 시간은 쓰지 않는다).

개선한 설계는 mq 응답 큐 8개 + 요청 큐 입장 제한(`OSP_MQ_RESP_Q=8 OSP_MQ_ADMIT=1`)과 shm 클라이언트별 요청 링(`OSP_SHM_REQ=per`)이다. 설계는 [설계 명세의 변경 기록](../design/README.md#변경-기록)에, 결과는 [2단계 측정 결과](stage2.md)에 있다.

공통 조건: 서버 워커 4(`cause-workers`·`cause-lockw` 빼고), 타일 64, 클라이언트마다 워밍업 요청 200개(클라이언트 256 이상은 50개). 측정 시간(`-d`)은 `type`·`thr`·`cause-workers`·`cause-mix`·`cause-ring` 1초, `scale`·`lock`·`imp-scale`·`cause-lockw` 2초(클라이언트 256 이상 3초), `target`과 그 밖의 원인 확인·개선 실험 3초다.

실행 한 번의 순서(`scripts/app-bench.sh`)

1. 서버를 새로 띄운다(`-O`로 서버 줄을 남기게).
2. 실행기(`loadgen`)가 클라이언트 N개를 `-G`로 띄운다. 모두 워밍업을 마치고 준비를 알리면 서버에 SIGUSR1(측정 시작 표시)을 보내고 클라이언트 전체에 SIGUSR2를 보내 측정을 함께 시작시킨다.
3. 모든 클라이언트가 끝나면 서버에 다시 SIGUSR1(측정 끝 표시)을 보낸다. 실행기는 클라이언트들의 표본을 합쳐 한 실행의 결과(클라이언트 줄)를 CSV에 쓴다.
4. 서버를 SIGINT로 끝낸다. 서버는 두 표시 사이의 자원 사용량으로 서버 줄을 쓴다. 서버의 그림판별 체크섬을 검증기 정답과 비교해 `checksum_ok` 칸에 남긴다. 측정한 모든 실행이 경쟁 상태 없이 끝났는지 이 칸으로 확인한다.
5. 반복마다 IPC 순서를 돌린다(pipe → mq → shm, mq → shm → pipe, …). 시간에 따른 변화(발열, 다른 작업)가 한 IPC에만 몰리지 않게 한다.

| 항목 | 정한 것 |
| --- | --- |
| 지연 표본 | 측정 요청마다 `t5 - t0` 하나. 클라이언트가 표본을 파일로 남기면 실행기가 모든 클라이언트의 표본을 합쳐 평균·중앙값·상위 99%·최대를 낸다(요청 종류별로도). 클라이언트마다의 백분위수를 평균하지 않는다 |
| 구간별 시간 | 측정 요청 전체의 [구간](#타임스탬프-위치) 평균 |
| 처리량 | (모든 클라이언트가 받은 측정 응답 수) / (가장 먼저 시작한 클라이언트부터 가장 늦게 끝난 클라이언트까지). MB/s는 받은 응답 payload(서버 시각 포함)의 합 |
| 자원 사용량 | 클라이언트 줄: 모든 클라이언트의 CPU 시간·컨텍스트 스위치 합, VmRSS·VmHWM은 가장 큰 값. 서버 줄: 서버 프로세스의 측정 구간 값, 서버 VmRSS·VmHWM, 우리 공유 메모리의 실제 사용량(`/proc/sysvipc/shm`의 rss), IPC 버퍼 용량 |
| 실패로 보는 실행 | 클라이언트 하나라도 종료 코드가 0이 아니거나, 응답 검사(종류, 길이, 서버 시각 순서)에 실패하거나, 체크섬이 다른 실행. CSV에는 남기되 요약에서 뺀다(남긴 이유는 재현을 위해서다) |

## 지연과 처리량

| 지표 | 재는 방법 |
| --- | --- |
| 지연 | 동시에 띄워 둘 요청 수를 1로 둔다. 응답을 받아야 다음 요청을 보낸다 |
| 처리량 | 동시 요청 수를 늘려 정해진 건수를 보내고, 전체 걸린 시간으로 초당 메시지 수와 초당 바이트 수를 구한다. 바이트 수는 한쪽 방향의 payload만 센다(1MB/s = 10^6 B/s) |

두 지표를 한 실행에서 함께 구하지 않는다. 우리 응용에 어느 쪽이 중요한지는 [워크로드 명세](../records/prediction.md#어느-지표가-더-중요한가)에서 정했다.

## 자원 사용량

| 항목 | 방법 |
| --- | --- |
| CPU 시간 | `getrusage(RUSAGE_SELF)`의 `ru_utime`, `ru_stime`. 측정 구간 앞뒤의 차이 |
| 컨텍스트 스위치 | 같은 호출의 `ru_nvcsw`(자발적), `ru_nivcsw`(비자발적) |
| 메모리 | `/proc/self/status`의 `VmRSS`, `VmHWM` |

서버와 클라이언트 양쪽에서 재고, 어느 프로세스의 값인지 원자료에 적는다.

## 간섭 줄이기

- 측정 구간 안에서 `printf` 등 출력을 하지 않는다. 기록은 메모리에 쌓고 끝난 뒤에 쓴다.
- 측정 중에는 브라우저 같은 무거운 프로그램을 끈다.
- CPU 주파수 설정(governor)과 `taskset` 사용 여부는 바꿨으면 바꾼 대로 기록한다. 클라이언트 8개가 코어 수를 넘는지 함께 본다.
- 기록 배열은 미리 잡아 둔다. 측정 구간 안에서 `malloc`을 부르지 않는다.

## 원인 분석 도구

추정한 원인은 아래 근거 중 하나 이상으로 확인한다.

| 알고 싶은 것 | 방법 |
| --- | --- |
| 어느 구간이 느린가 | 구간별 시간 |
| 시스템 콜을 몇 번 부르는가 | `strace -c -f ./bin/...` |
| 스레드가 얼마나 자주 멈추는가 | 자발적·비자발적 컨텍스트 스위치 수 |
| 전체 자원 사용 | `/usr/bin/time -v ./bin/...` |
| 락을 얼마나 기다리는가 | 락 대기 구간, 락 방식 옵션만 바꾼 재측정 |

`strace`는 프로그램을 느리게 만든다. 횟수를 세는 데만 쓰고, 그 실행의 시간 값은 버린다.

## 원자료 형식

실행 하나가 요약 CSV의 한 줄(1단계는 client, server 두 줄)이다. 파일은 `results/<단계>_<실험이름>.csv`에 둔다. 열은 아래로 확정했고 `include/util.h`의 `CSV_HEADER`가 같은 순서로 쓴다. 바꾸면 이 문서부터 고친다.

```text
stage,ipc,role,experiment,commit,run,
msg_size,clients,inflight,workers,lock,tile,draw_pct,rect,snap,
n,elapsed_s,lat_mean_us,lat_p50_us,lat_p99_us,lat_max_us,thr_msg_s,thr_mb_s,
req_us,queue_us,lock_us,work_us,resp_us,
utime_s,stime_s,nvcsw,nivcsw,vmrss_kb,vmhwm_kb
```

- `stage`는 `bench` 또는 `app`, `ipc`는 `pipe`·`mq`·`shm`(1단계는 `upipe`도), `role`은 `server` 또는 `client`.
- `experiment`는 실험 이름이다. 1단계는 `latency`(동시 요청 1), `throughput`(동시 요청 16), `clock`(clock_gettime 비용, `ipc` 칸이 비어 있다).
- `commit`은 측정한 코드의 커밋, `run`은 같은 조건의 반복 번호, `elapsed_s`는 측정 구간의 길이(초)다.
- `workers`, `lock`, `tile`은 서버 옵션 `-w`, `-l`, `-t`의 값이다.
- 해당 없는 칸은 비운다. 1단계에는 `workers`부터 `snap`까지와 구간별 시간이 없다.
- 클라이언트 프로세스 여럿이 같은 파일에 써도 줄이 섞이지 않게 `csv_append`(`util.c`)가 파일 잠금을 걸고 쓴다. 2단계 `client.c`도 이 함수를 쓴다. 머리글이 다른 파일(열을 바꾸기 전에 잰 결과)에는 덧붙이지 않고 실패한다.
- 1단계 파일: `results/bench_latency.csv`, `results/bench_throughput.csv`, `results/bench_clock.csv`, 측정 환경 `results/bench_env.txt`. 처음 측정(반복 5번, 성능 예측의 근거)은 `results/` 바로 아래, 반복 20번으로 다시 잰 것은 `results/stage1-r20/`, 원인 확인 실험은 `results/exp-<실험>/`에 같은 이름으로 둔다([1단계 측정 결과](stage1.md#측정-기록)).
- 2단계 CSV 의 열은 `include/util.h`의 `APP_CSV_HEADER`다. 실행 하나가 줄 두 개다: `role=clients`(실행기가 모든 클라이언트를 합친 줄), `role=server`(서버의 측정 구간 줄). 클라이언트를 혼자 돌려 `-o`로 쓰면 `role=client`.

```text
stage,ipc,role,experiment,commit,run,
clients,rooms,inflight,rate_hz,workers,lock,tile,draw_pct,rect,snap,
n,errors,elapsed_s,thr_req_s,thr_mb_s,
lat_mean_us,lat_p50_us,lat_p99_us,lat_max_us,
draw_n,draw_mean_us,draw_p50_us,draw_p99_us,draw_max_us,
snap_n,snap_mean_us,snap_p50_us,snap_p99_us,snap_max_us,
req_us,queue_us,lock_us,work_us,resp_us,
utime_s,stime_s,nvcsw,nivcsw,vmrss_kb,vmhwm_kb,
ipc_cap_kb,ipc_rss_kb,ipc_min_chan,checksum_ok,env
```

- 2단계 파일: `results/app/<실험>.csv`(실행마다 클라이언트 줄 하나와 서버 줄 하나), 측정 환경 `results/app/app_env.txt`. 열은 `include/util.h`의 `APP_CSV_HEADER`이고 [2단계 실험](#2단계-실험)의 표와 같은 뜻이다. 원인 확인·설계 개선 실험도 같은 폴더의 `<실험>.csv`다(`cause-procs.csv`, `imp-mq-q.csv`, `imp-mq-req.csv`, `imp-scale.csv`, `imp-target.csv`, `mem-shm.csv`).
- 1단계 원인 확인 실험의 비교 표는 `scripts/compare-bench.py`가 `docs/assets/bench/experiments.md`로 만든다. 2단계 요약표와 그래프는 `scripts/plot-app.py`가 `docs/assets/app/`에 만든다.
- 그래프는 이 CSV에서만 만든다. 축 이름과 단위(µs, ms, MB/s)를 표시하고, 같은 조건의 세 IPC를 한 그래프에 그린다.

## IPC 한도 확인

설계가 기대는 숫자다. 측정용 컴퓨터에서 직접 확인하고 보고서에 실제 값을 쓴다.

```bash
ipcs -l                               # System V 한도 전체
cat /proc/sys/kernel/msgmax           # 메시지 하나의 최대 크기
cat /proc/sys/kernel/msgmnb           # 큐 하나의 최대 바이트
getconf PIPE_BUF /                    # 원자적으로 써지는 최대 크기
cat /proc/sys/fs/pipe-max-size        # 파이프 버퍼를 키울 수 있는 한도
```

파이프 버퍼의 현재 크기는 코드에서 `fcntl(fd, F_GETPIPE_SZ)`로 읽는다.
