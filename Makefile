CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
LDFLAGS ?= -lm

all: memfreq_bench

memfreq_bench: memfreq_bench.c stats.c stats.h
	$(CC) $(CFLAGS) -o $@ memfreq_bench.c stats.c $(LDFLAGS)

clean:
	rm -f memfreq_bench *.o memfreq_results.json

.PHONY: all clean
