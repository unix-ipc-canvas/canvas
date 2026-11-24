# 공유 캔버스 서버 - UNIX시스템프로그래밍 설계과제
#
#   make            전체 빌드 (소스가 있는 것만)
#   make bench      1단계 벤치마크 (pipe, mq, shm, 익명 pipe upipe)
#   make app        2단계 응용 (서버, 측정 클라이언트, 사람이 그리는 클라이언트 draw) 3종
#   make pipe       pipe 버전만   (mq, shm, upipe 도 같음. upipe 는 벤치마크만)
#   make tptest     시험 도구: 전송 계층 tptest 3종 (scripts/test-transport.sh), 서버 apptest 3종 (scripts/verify.sh)
#   make check      헤더 문법 검사 + 빌드 가능한 대상 출력
#   make test       검증 스크립트 실행 (scripts/verify.sh)
#   make ipcclean   남은 System V 자원과 FIFO 강제 삭제
#   make dist       제출용 osproject_<팀번호>.tar 만들기
#   make clean
#
# 2팀: System V key = 60020 ~ 60029 (60xxy 규칙, xx = 02).
TEAM_NO ?= 2

CC      := gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11
CFLAGS  += -pthread -Iinclude -DTEAM_NO=$(TEAM_NO) $(EXTRA_CFLAGS)
LDLIBS  += -pthread

IPCS := pipe mq shm
# 1단계 벤치마크만 만드는 IPC. 익명 pipe 는 fork 로만 이어져 따로 띄우는 서버·클라이언트에는 쓸 수 없다
BENCH_ONLY := upipe
BIN  := bin
OBJ  := build

# main 이 들어 있는 파일을 뺀 공통 모듈
MAINS      := src/common/server.c src/common/client.c src/common/verify.c src/common/draw.c
COMMON_SRC := $(filter-out $(MAINS),$(wildcard src/common/*.c))
COMMON_OBJ := $(COMMON_SRC:%.c=$(OBJ)/%.o)

# 소스가 있는 것만 빌드 대상에 넣는다 (뼈대 단계에서도 make 가 실패하지 않게)
HAVE   := $(foreach i,$(IPCS),$(if $(wildcard src/transport/transport_$(i).c),$(i)))
HAVE_B := $(HAVE) $(foreach i,$(BENCH_ONLY),$(if $(wildcard src/transport/transport_$(i).c),$(i)))
BENCH  := $(if $(wildcard src/bench/bench.c),$(HAVE_B:%=$(BIN)/bench_%))
APP    := $(if $(wildcard src/common/server.c),$(HAVE:%=$(BIN)/server_%)) \
          $(if $(wildcard src/common/client.c),$(HAVE:%=$(BIN)/client_%)) \
          $(if $(wildcard src/common/draw.c),$(HAVE:%=$(BIN)/draw_%))
VERIFY := $(if $(wildcard src/common/verify.c),$(BIN)/verify)
TPTEST := $(if $(wildcard src/test/tptest.c),$(HAVE:%=$(BIN)/tptest_%)) \
          $(if $(wildcard src/test/apptest.c),$(HAVE:%=$(BIN)/apptest_%))

.PHONY: all bench app tptest check test clean ipcclean dist
all: check bench app $(TPTEST)
bench: $(BENCH)
app: $(APP) $(VERIFY)
tptest: $(TPTEST)

define IPC_RULES
.PHONY: $(1)
$(1): $$(filter %_$(1),$$(BENCH) $$(APP) $$(TPTEST))

$$(BIN)/bench_$(1): $$(OBJ)/src/bench/bench.o $$(OBJ)/src/transport/transport_$(1).o $$(COMMON_OBJ) | $$(BIN)
	$$(CC) $$(CFLAGS) -o $$@ $$^ $$(LDLIBS)

$$(BIN)/server_$(1): $$(OBJ)/src/common/server.o $$(OBJ)/src/transport/transport_$(1).o $$(COMMON_OBJ) | $$(BIN)
	$$(CC) $$(CFLAGS) -o $$@ $$^ $$(LDLIBS)

$$(BIN)/client_$(1): $$(OBJ)/src/common/client.o $$(OBJ)/src/transport/transport_$(1).o $$(COMMON_OBJ) | $$(BIN)
	$$(CC) $$(CFLAGS) -o $$@ $$^ $$(LDLIBS)

$$(BIN)/draw_$(1): $$(OBJ)/src/common/draw.o $$(OBJ)/src/transport/transport_$(1).o $$(COMMON_OBJ) | $$(BIN)
	$$(CC) $$(CFLAGS) -o $$@ $$^ $$(LDLIBS)

$$(BIN)/tptest_$(1): $$(OBJ)/src/test/tptest.o $$(OBJ)/src/transport/transport_$(1).o $$(COMMON_OBJ) | $$(BIN)
	$$(CC) $$(CFLAGS) -o $$@ $$^ $$(LDLIBS)

$$(BIN)/apptest_$(1): $$(OBJ)/src/test/apptest.o $$(OBJ)/src/transport/transport_$(1).o $$(COMMON_OBJ) | $$(BIN)
	$$(CC) $$(CFLAGS) -o $$@ $$^ $$(LDLIBS)
endef
$(foreach i,$(IPCS) $(BENCH_ONLY),$(eval $(call IPC_RULES,$(i))))

$(BIN)/verify: $(OBJ)/src/common/verify.o $(COMMON_OBJ) | $(BIN)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(OBJ)/%.o: %.c $(wildcard include/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BIN):
	@mkdir -p $@

check:
	@for h in include/*.h; do $(CC) $(CFLAGS) -fsyntax-only -x c $$h || exit 1; done
	@echo "헤더 검사 통과 (TEAM_NO=$(TEAM_NO))"
	@echo "전송 계층 구현: $(if $(strip $(HAVE_B)),$(strip $(HAVE_B)),아직 없음)"
	@echo "빌드 대상: $(if $(strip $(BENCH) $(APP) $(VERIFY) $(TPTEST)),$(strip $(BENCH) $(APP) $(VERIFY) $(TPTEST)),아직 없음)"

test:
	@./scripts/verify.sh

# 비정상 종료로 남은 자원 치우기. 정상 종료 때는 프로그램이 직접 지워야 한다.
# mq 큐 나누기(OSP_MQ_QUEUES)의 key 없는 큐는 key 로 찾을 수 없어, 잠금 파일을 지우기 전에 그 안의 기록을 읽어 지운다.
# 기록: MQ_REC_OFF 바이트부터 4바이트 정수 130개 = magic(0x4d515332), 쌍 수 n, 응답 큐 64개, 요청 큐 64개.
# 1 .. n-1 번 쌍만 key 없는 큐다 (src/transport/transport_mq.c 의 SHARD_OFF, shard_rec_t 와 같아야 한다)
MQ_REC_OFF := 5120
ipcclean:
	@f=/tmp/osproject_$(TEAM_NO)_mq.lock; \
	if [ -f "$$f" ]; then \
	  set -- $$(od -An -v -t d4 -j $(MQ_REC_OFF) -N 520 "$$f" 2>/dev/null); \
	  if [ $$# -eq 130 ] && [ "$$1" = 1297175346 ]; then \
	    n=$$2; shift 2; k=0; \
	    for q in "$$@"; do \
	      s=$$((k % 64)); k=$$((k + 1)); \
	      if [ $$s -ge 1 ] && [ $$s -lt $$n ]; then ipcrm -q $$q 2>/dev/null; fi; \
	    done; \
	  fi; \
	fi; \
	for y in 0 1 2 3 4 5 6 7 8 9; do \
	  k=$$((60000 + $(TEAM_NO) * 10 + y)); \
	  ipcrm -Q $$k 2>/dev/null; ipcrm -M $$k 2>/dev/null; ipcrm -S $$k 2>/dev/null; \
	done; rm -f /tmp/osproject_$(TEAM_NO)_*; echo "정리 완료. 확인: ipcs"

dist: clean
	tar cvf osproject_$(TEAM_NO).tar Makefile readme.txt include src scripts results docs/records/ai-log.md docs/records/ai-chats

clean:
	rm -rf $(BIN) $(OBJ) osproject_*.tar
