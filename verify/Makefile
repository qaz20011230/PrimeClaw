CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -pthread
LDFLAGS ?=

verify_primes: verify_primes.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f verify_primes

.PHONY: clean
