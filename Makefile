CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
LDFLAGS ?= -lm

all: memfreq_bench

memfreq_bench: memfreq_bench.c stats.c stats.h
	$(CC) $(CFLAGS) -o $@ memfreq_bench.c stats.c $(LDFLAGS)

# Unit tests for stats.c (no root / Linux / cpufreq needed).
# Run with: ./test_stats
test_stats: tests/test_stats.c stats.c stats.h
	$(CC) $(CFLAGS) -I. -o $@ tests/test_stats.c stats.c $(LDFLAGS)

clean:
	rm -f memfreq_bench test_stats *.o memfreq_results.json

.PHONY: all clean
