CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -pthread
LDFLAGS = -pthread -lrt

all: sum-server-shm sum-client-shm

sum-server-shm: sum-server-shm.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

sum-client-shm: sum-client-shm.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f sum-server-shm sum-client-shm

.PHONY: all clean
