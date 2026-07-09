UNAME := $(shell uname)
ifeq ($(UNAME), Darwin)
    CC = clang
else
    CC = gcc
endif

CFLAGS  = -Wall -Wextra -Wshadow -O2 -std=c11
SRCS    = uart_reliable.c uart_reliable_test.c
TARGET  = uart_reliable_test

$(TARGET): $(SRCS) uart_reliable.h
	$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -f $(TARGET) *.o

.PHONY: clean
