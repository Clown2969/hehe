CC = gcc
CFLAGS = -Wall -O2 -static

.PHONY: all clean

all: test

test: test.c
	$(CC) $(CFLAGS) $< -o test

clean:
	rm -f test
