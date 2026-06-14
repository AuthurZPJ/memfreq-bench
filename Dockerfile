# Dockerfile — build environment for memfreq_bench
#
# This image lets you compile the binary and run the unit tests
# (test_stats). It does NOT include a cpufreq-enabled kernel, so the
# actual benchmark cannot run end-to-end here — that requires:
#   - Linux host kernel with CONFIG_CPU_FREQ=y and a cpufreq driver
#     loaded (intel_pstate, acpi-cpufreq, etc.)
#   - root access to /sys/devices/system/cpu/cpu*/cpufreq/
#
# Use this image to:
#   - Verify the build works on a clean system
#   - Run the C unit tests in CI
#   - Cross-compile before deploying to a real Linux machine
#
# Build:    docker build -t memfreq-bench .
# Test:     docker run --rm memfreq-bench
# Shell:    docker run --rm -it memfreq-bench bash

FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        python3 \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src

# Build the benchmark and the C unit tests for stats.c.
# These targets don't need root or cpufreq.
RUN make clean && make && make test_stats

# The binary itself requires root + Linux cpufreq at runtime, which
# the container can't provide. Document the constraint and provide a
# non-privileged smoke test instead.
RUN ./memfreq_bench -h > /dev/null && ./test_stats

CMD ["/bin/bash", "-c", "./test_stats && echo OK: build + unit tests passed"]
