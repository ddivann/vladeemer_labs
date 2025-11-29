CC = gcc
CFLAGS = -std=c11 -O3 -Wall -Wextra -fPIC
LDFLAGS = -shared
LIBS = -ldl

all: driver alloc_free_list.so alloc_mc_kusick.so

# driver
src/driver: src/driver.c include/allocator_api.h
	$(CC) -std=c11 -O3 -Wall -Wextra -o $@ src/driver.c -ldl -lrt

driver: src/driver

# free list allocator
alloc_free_list.so: alloc_free_list/free_list.o
	$(CC) $(LDFLAGS) -o $@ $^

alloc_free_list/free_list.o: alloc_free_list/free_list.c alloc_free_list/free_list.h
	$(CC) $(CFLAGS) -c -o $@ alloc_free_list/free_list.c

# McKusick-Karels (buddy-like)
alloc_mc_kusick.so: alloc_mc_kusick/mc_kusick.o
	$(CC) $(LDFLAGS) -o $@ $^

alloc_mc_kusick/mc_kusick.o: alloc_mc_kusick/mc_kusick.c alloc_mc_kusick/mc_kusick.h
	$(CC) $(CFLAGS) -c -o $@ alloc_mc_kusick/mc_kusick.c

clean:
	rm -f src/driver alloc_free_list/*.o alloc_mc_kusick/*.o alloc_free_list.so alloc_mc_kusick.so

.PHONY: all clean driver
