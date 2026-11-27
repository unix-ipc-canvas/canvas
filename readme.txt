UNIX시스템프로그래밍 설계과제 - 2팀 - 다중 그림판 공유 캔버스 서버
====================================================================

1. 실행 환경
   - Linux (측정: Ubuntu 26.04.1 LTS, Intel Core 5 120U, 논리 CPU 12개, 가상화 없음)
   - gcc, GNU make. 외부 라이브러리 없음. 그래프만 Python 3 + matplotlib.

2. 컴파일
   make            전체 빌드 (1단계 벤치마크 4종, 2단계 응용 3종, 실행기, 검증기)
   make bench      1단계 벤치마크만
   make pipe       pipe 버전만 (mq, shm 도 같음)
   make test       검증
   make ipcclean   비정상 종료로 남은 IPC 자원 삭제

3. 실행 방법
   (2단계 응용 구현 뒤 채운다)

4. 측정 재현
   scripts/bench.sh       1단계 측정
