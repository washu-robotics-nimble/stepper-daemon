CC = gcc
CFLAGS = -Wall -O2 -std=c11
LDFLAGS = -llgpio -lpthread -lm

SRC = src/main.c src/config.c src/motor.c src/ramp.c src/command.c src/status.c src/cli.c lib/inih/ini.c
OBJ = $(SRC:.c=.o)

TARGET = motor_demo

# Host unit tests: each tests/test_*.c is one standalone program, linked
# with the SDK-independent firmware modules
TEST_SRC = $(wildcard tests/test_*.c)
TEST_BIN = $(TEST_SRC:tests/%.c=build/tests/%)
TEST_LIB = lib/cJSON/cJSON.c \
           src/firmware/linebuf.c src/firmware/hostlink.c src/firmware/board_table.c \
           src/firmware/stepper.c tests/dummy_stepgen.c

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

test: $(TEST_BIN)
	@for t in $(TEST_BIN); do ./$$t || exit 1; done
	@echo "$(words $(TEST_BIN)) test program(s) passed"

build/tests/%: tests/%.c $(TEST_LIB) $(wildcard tests/*.h src/firmware/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Ilib/cJSON -Isrc/firmware -Itests -o $@ $< $(TEST_LIB) -lm

clean:
	rm -f $(TARGET) $(OBJ)
	rm -rf build

.PHONY: all clean test
