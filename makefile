CC = gcc
CFLAGS = -Wall -O2 -std=c11
LDFLAGS = -llgpio -lpthread

OBJ = lib/inih/ini.o src/config.o src/motor.o src/main.o 

all: motor_demo

motor_demo: $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -f motor_demo $(OBJ)