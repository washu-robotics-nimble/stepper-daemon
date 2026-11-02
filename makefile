CC = gcc
CFLAGS = -Wall -O2 -std=c11

# The daemon. inih: a key's line number goes to the handler, an empty section
# still reaches it, and an indented line is a key of its own (no multi-line
# values).
DAEMON_SRC = src/daemon/main.c src/daemon/config.c src/daemon/serial.c \
             src/daemon/link.c src/daemon/handshake.c \
             lib/inih/ini.c lib/cJSON/cJSON.c lib/log/log.c
DAEMON_LIB = $(filter-out src/daemon/main.c, $(DAEMON_SRC))
DAEMON_CFLAGS = $(CFLAGS) -D_GNU_SOURCE \
                -DINI_HANDLER_LINENO=1 -DINI_CALL_HANDLER_ON_NEW_SECTION=1 -DINI_ALLOW_MULTILINE=0 \
                -Isrc/daemon -Ilib/inih -Ilib/cJSON -Ilib/log
DAEMON_HDR = $(wildcard src/daemon/*.h) lib/inih/ini.h lib/cJSON/cJSON.h lib/log/log.h

# Host unit tests: each tests/test_*.c is one standalone program. The
# tests/test_daemon_*.c ones are linked with the daemon's modules, the
# others with the SDK-independent firmware modules.
DTEST_SRC = $(wildcard tests/test_daemon_*.c)
FTEST_SRC = $(filter-out $(DTEST_SRC), $(wildcard tests/test_*.c))
TEST_BIN = $(FTEST_SRC:tests/%.c=build/tests/%) $(DTEST_SRC:tests/%.c=build/tests/%)
TEST_LIB = lib/cJSON/cJSON.c \
           src/firmware/linebuf.c src/firmware/linequeue.c src/firmware/hostlink.c src/firmware/board_table.c \
           src/firmware/stepper.c src/firmware/planner.c tests/dummy_stepgen.c tests/dummy_board.c

all: build/nimhubd

build/nimhubd: $(DAEMON_SRC) $(DAEMON_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(DAEMON_CFLAGS) -o $@ $(DAEMON_SRC) -lm

test: $(TEST_BIN)
	@for t in $(TEST_BIN); do ./$$t || exit 1; done
	@echo "$(words $(TEST_BIN)) test program(s) passed"

# nimhubd against tests/dummy_mcu.py, end to end (Python 3)
check-daemon: build/nimhubd
	python3 tests/check_daemon.py build/nimhubd

build/tests/test_daemon_%: tests/test_daemon_%.c $(DAEMON_LIB) $(DAEMON_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(DAEMON_CFLAGS) -o $@ $< $(DAEMON_LIB) -lm

build/tests/%: tests/%.c $(TEST_LIB) $(wildcard tests/*.h src/firmware/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Ilib/cJSON -Isrc/firmware -Itests -o $@ $< $(TEST_LIB) -lm

clean:
	rm -rf build

.PHONY: all check-daemon clean test
