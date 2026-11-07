# Analyst tooling -- self-contained (writes only inside primes/reports/)
PYTHON ?= python3
CC ?= cc
CFLAGS ?= -O3 -std=gnu11 -Wall -Wextra -Wpedantic

.PHONY: all selftest clean

all: fast_stats

fast_stats: fast_stats.c
	$(CC) $(CFLAGS) -o $@ $<

selftest: fast_stats
	$(PYTHON) selftest.py

clean:
	rm -f fast_stats
	rm -rf _selftest
