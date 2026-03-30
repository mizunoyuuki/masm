CC = gcc
CFLAGS = -Wall -Wextra -g -std=c11

masm: main.c
	$(CC) $(CFLAGS) -o masm main.c

test: masm
	bash test.sh

clean:
	rm -f masm a.out

.PHONY: test clean
