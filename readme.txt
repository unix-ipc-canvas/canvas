UNIX시스템프로그래밍 설계과제 - 공유 캔버스 서버
(제출용 readme. 구현이 진행되면 아래 빈칸을 채운다.)

1. 실행 환경
   - OS / 커널:
   - gcc:
   - CPU / 코어 수 / 메모리:
   - 가상화 여부:

2. 컴파일
   make            전체 (1단계 벤치마크 + 2단계 응용, 3가지 IPC)
   make bench      1단계 벤치마크만
   make app        2단계 응용만
   make pipe       pipe 버전만 (mq, shm 도 같음)
   make test       검증 스크립트 실행 (PASS / FAIL / SKIP 출력)

3. 실행
   (구현 후 작성: 서버 실행, 클라이언트 2개 이상 실행, 종료 방법)

4. 측정 재현
   (구현 후 작성: scripts/ 의 어떤 스크립트를 어떤 순서로 돌리는지, 결과가 results/ 의 어느 파일에 생기는지)

5. IPC 자원 확인
   실행 종료 후 ipcs 로 남은 자원이 없는지 확인한다.
   비정상 종료로 남았으면 make ipcclean
