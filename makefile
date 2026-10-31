CC = gcc
CFLAGS = -Wall -O2 -std=c11
LDFLAGS = -llgpio -lpthread -lm

SRC = src/main.c src/config.c src/motor.c src/ramp.c src/command.c src/status.c src/cli.c lib/inih/ini.c
OBJ = $(SRC:.c=.o)

TARGET = motor_demo

# Host unit tests: each tests/test_*.c is one standalone program
TEST_SRC = $(wildcard tests/test_*.c)
TEST_BIN = $(TEST_SRC:tests/%.c=build/tests/%)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

test: $(TEST_BIN)
	@for t in $(TEST_BIN); do ./$$t || exit 1; done
	@echo "$(words $(TEST_BIN)) test program(s) passed"

build/tests/%: tests/%.c lib/cJSON/cJSON.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Ilib/cJSON -o $@ $^ -lm

clean:
	rm -f $(TARGET) $(OBJ)
	rm -rf build

.PHONY: all clean test
