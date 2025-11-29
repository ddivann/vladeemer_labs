CC=gcc
CFLAGS=-Wall -Wextra -std=c11 -g
OBJDIR=obj
BINDIR=bin

SOURCES = sum-server.c sum-client.c
BINARIES = $(patsubst %.c,$(BINDIR)/%,$(SOURCES))

all: $(BINARIES)

$(BINDIR)/%: %.c | $(BINDIR)
	$(CC) $(CFLAGS) $< -o $@

$(BINDIR):
	mkdir -p $(BINDIR)

clean:
	rm -rf $(OBJDIR) $(BINDIR)

run-sum: $(BINDIR)/sum-server $(BINDIR)/sum-client
	./$(BINDIR)/sum-server

install-deps:
	sudo apt update
	sudo apt install -y build-essential gdb valgrind

.PHONY: all clean run-sum install-deps
