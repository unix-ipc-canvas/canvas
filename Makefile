# 공유 캔버스 서버 - UNIX시스템프로그래밍 설계과제
#
#   make            전체 빌드 (소스가 있는 것만)
#   make bench      1단계 벤치마크 3종
#   make app        2단계 응용 (서버, 클라이언트) 3종
#   make pipe       pipe 버전만   (mq, shm 도 같음)
#   make check      헤더 문법 검사 + 빌드 가능한 대상 출력
#   make ipcclean   남은 System V 자원과 FIFO 강제 삭제
#   make dist       제출용 osproject_<팀번호>.tar 만들기
#   make clean
#
# 팀 번호가 정해지면 TEAM_NO 를 고친다 (System V key = 60xxy 규칙).
TEAM_NO ?= 0

CC      := gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11
CFLAGS  += -pthread -Iinclude -DTEAM_NO=$(TEAM_NO)
LDLIBS  += -pthread

IPCS := pipe mq shm
BIN  := bin
OBJ  := build

# main 이 들어 있는 파일을 뺀 공통 모듈
MAINS      := src/common/server.c src/common/client.c src/common/verify.c
COMMON_SRC := $(filter-out $(MAINS),$(wildcard src/common/*.c))
COMMON_OBJ := $(COMMON_SRC:%.c=$(OBJ)/%.o)

# 소스가 있는 것만 빌드 대상에 넣는다 (뼈대 단계에서도 make 가 실패하지 않게)
HAVE   := $(foreach i,$(IPCS),$(if $(wildcard src/transport/transport_$(i).c),$(i)))
BENCH  := $(if $(wildcard src/bench/bench.c),$(HAVE:%=$(BIN)/bench_%))
APP    := $(if $(wildcard src/common/server.c),$(HAVE:%=$(BIN)/server_%)) \
          $(if $(wildcard src/common/client.c),$(HAVE:%=$(BIN)/client_%))
VERIFY := $(if $(wildcard src/common/verify.c),$(BIN)/verify)

.PHONY: all bench app check clean ipcclean dist
all: check bench app $(VERIFY)
bench: $(BENCH)
app: $(APP)

define IPC_RULES
.PHONY: $(1)
$(1): $$(filter %_$(1),$$(BENCH) $$(APP))

$$(BIN)/bench_$(1): $$(OBJ)/src/bench/bench.o $$(OBJ)/src/transport/transport_$(1).o $$(COMMON_OBJ) | $$(BIN)
	$$(CC) $$(CFLAGS) -o $$@ $$^ $$(LDLIBS)

$$(BIN)/server_$(1): $$(OBJ)/src/common/server.o $$(OBJ)/src/transport/transport_$(1).o $$(COMMON_OBJ) | $$(BIN)
	$$(CC) $$(CFLAGS) -o $$@ $$^ $$(LDLIBS)

$$(BIN)/client_$(1): $$(OBJ)/src/common/client.o $$(OBJ)/src/transport/transport_$(1).o $$(COMMON_OBJ) | $$(BIN)
	$$(CC) $$(CFLAGS) -o $$@ $$^ $$(LDLIBS)
endef
$(foreach i,$(IPCS),$(eval $(call IPC_RULES,$(i))))

$(BIN)/verify: $(OBJ)/src/common/verify.o $(COMMON_OBJ) | $(BIN)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(OBJ)/%.o: %.c $(wildcard include/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BIN):
	@mkdir -p $@

check:
	@$(CC) $(CFLAGS) -fsyntax-only -x c include/proto.h
	@$(CC) $(CFLAGS) -fsyntax-only -x c include/transport.h
	@echo "헤더 검사 통과 (TEAM_NO=$(TEAM_NO))"
	@echo "전송 계층 구현: $(if $(strip $(HAVE)),$(strip $(HAVE)),아직 없음)"
	@echo "빌드 대상: $(if $(strip $(BENCH) $(APP) $(VERIFY)),$(strip $(BENCH) $(APP) $(VERIFY)),아직 없음)"

# 비정상 종료로 남은 자원 치우기. 정상 종료 때는 프로그램이 직접 지워야 한다.
ipcclean:
	@for y in 0 1 2 3 4 5 6 7 8 9; do \
	  k=$$((60000 + $(TEAM_NO) * 10 + y)); \
	  ipcrm -Q $$k 2>/dev/null; ipcrm -M $$k 2>/dev/null; ipcrm -S $$k 2>/dev/null; \
	done; rm -f /tmp/osproject_$(TEAM_NO)_*; echo "정리 완료. 확인: ipcs"

dist: clean
	tar cvf osproject_$(TEAM_NO).tar Makefile readme.txt include src scripts results

clean:
	rm -rf $(BIN) $(OBJ) osproject_*.tar
