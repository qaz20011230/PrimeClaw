CC ?= cc
CFLAGS ?= -O3 -Wall -Wextra -pthread
BIN := ../bin

all: $(BIN)/sieve_gap $(BIN)/primes_tool

$(BIN):
	mkdir -p $(BIN)

$(BIN)/sieve_gap: sieve_gap.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ sieve_gap.c -lm

$(BIN)/primes_tool: primes_tool.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ primes_tool.c -lm

clean:
	rm -f $(BIN)/sieve_gap $(BIN)/primes_tool

.PHONY: all clean
