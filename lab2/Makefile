CC = gcc
CFLAGS = -std=c11 -O3 -Wall -Wextra -pthread
LDFLAGS = -pthread

all: bitonic-sync bitonic-atomic

bitonic-sync: bitonic.c
	$(CC) $(CFLAGS) -DMODE_SYNC -o $@ $< $(LDFLAGS)

bitonic-atomic: bitonic.c
	$(CC) $(CFLAGS) -DMODE_ATOMIC -o $@ $< $(LDFLAGS)

clean:
	rm -f bitonic-sync bitonic-atomic

.PHONY: all clean
