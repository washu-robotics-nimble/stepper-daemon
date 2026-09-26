CC = gcc
CFLAGS = -Wall -O2 -std=c11
LDFLAGS = -llgpio -lpthread -lm

SRC = src/main.c src/config.c src/motor.c src/ramp.c src/command.c src/status.c src/cli.c lib/inih/ini.c
OBJ = $(SRC:.c=.o)

TARGET = motor_demo

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJ)

.PHONY: all clean