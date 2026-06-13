#!/usr/bin/env python3
"""
memfreq_sweep.py — Run memfreq_bench and visualize the sweet spot.

Usage:
    sudo python3 memfreq_sweep.py                  # default args
    sudo python3 memfreq_sweep.py -c 2 -s 16       # pass-through to C binary
    sudo python3 memfreq_sweep.py --file results.txt  # parse existing output
    sudo python3 memfreq_sweep.py --json            # also save JSON

Requires: root (for cpufreq writes), memfreq_bench compiled in same dir.
"""

import argparse
import json
import os
import subprocess
import sys

BENCH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "memfreq_bench")
BAR_W = 40


def run_bench(extra_args: list[str]) -> str:
    if not os.path.isfile(BENCH):
        print(f"ERROR: {BENCH} not found.  Compile first:", file=sys.stderr)
        print(f"  gcc -O2 -o memfreq_bench memfreq_bench.c -lm", file=sys.stderr)
        sys.exit(1)
    cmd = [BENCH] + extra_args
    print(f"$ {' '.join(cmd)}\n", file=sys.stderr)
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stderr, file=sys.stderr)
        sys.exit(result.returncode)
    return result.stdout


def parse_output(text: str) -> dict:
    meta = {}
    rows = []
    sweet = {}

    for line in text.splitlines():
        if line.startswith("# cpu="):
            for kv in line[2:].split():
                k, v = kv.split("=", 1)
                meta[k] = v
        elif line.startswith("# stride  sweet spot:"):
            sweet["stride"] = int(line.split(":")[1].strip().split()[0])
        elif line.startswith("# chase   sweet spot:"):
            sweet["chase"] = int(line.split(":")[1].strip().split()[0])
        elif line.startswith("#"):
            continue
        elif line.strip():
            parts = line.split("\t")
            if len(parts) == 7:
                rows.append({
                    "freq_mhz": int(parts[0]),
                    "stride_mops": float(parts[1]),
                    "stride_pct": float(parts[2]),
                    "chase_mops": float(parts[3]),
                    "chase_pct": float(parts[4]),
                    "compute_mops": float(parts[5]),
                    "compute_pct": float(parts[6]),
                })
            elif len(parts) == 5:
                rows.append({
                    "freq_mhz": int(parts[0]),
                    "stride_mops": float(parts[1]),
                    "stride_pct": float(parts[2]),
                    "compute_mops": float(parts[3]),
                    "compute_pct": float(parts[4]),
                })

    return {"meta": meta, "rows": rows, "sweet": sweet}


def bar(pct: float, width: int = BAR_W) -> str:
    filled = int(pct / 100 * width)
    return "█" * filled + "░" * (width - filled)


def visualize(data: dict):
    rows = data["rows"]
    sweet = data["sweet"]
    has_chase = "chase_mops" in rows[0] if rows else False

    print("=" * 78)
    print("  Memory-Bound Frequency Sweet-Spot Analysis")
    print("=" * 78)

    if data["meta"]:
        m = data["meta"]
        print(f"  CPU={m.get('cpu','?')}  array={m.get('array','?')}  "
              f"stride={m.get('stride','?')}  duration={m.get('duration','?')}s")
    print()

    # --- Stride chart ---
    print("── Stride (sequential memory access) ──────────────────────────────")
    print(f"{'MHz':>6}  {'Mops':>8}  {'%':>5}  {'throughput':<{BAR_W}}")
    for r in rows:
        marker = " ◄ sweet spot" if (
            "stride" in sweet and r["freq_mhz"] == sweet["stride"]) else ""
        print(f"{r['freq_mhz']:>6}  {r['stride_mops']:>8.1f}  "
              f"{r['stride_pct']:>5.1f}  {bar(r['stride_pct'])}{marker}")
    print()

    # --- Chase chart ---
    if has_chase:
        print("── Pointer Chase (random memory access) ───────────────────────────")
        print(f"{'MHz':>6}  {'Mops':>8}  {'%':>5}  {'throughput':<{BAR_W}}")
        for r in rows:
            marker = " ◄ sweet spot" if (
                "chase" in sweet and r["freq_mhz"] == sweet["chase"]) else ""
            print(f"{r['freq_mhz']:>6}  {r['chase_mops']:>8.1f}  "
                  f"{r['chase_pct']:>5.1f}  {bar(r['chase_pct'])}{marker}")
        print()

    # --- Compute (control) chart ---
    print("── Compute (FP arithmetic, no memory) ─────────────────────────────")
    print(f"{'MHz':>6}  {'Mops':>8}  {'%':>5}  {'throughput':<{BAR_W}}")
    for r in rows:
        print(f"{r['freq_mhz']:>6}  {r['compute_mops']:>8.1f}  "
              f"{r['compute_pct']:>5.1f}  {bar(r['compute_pct'])}")
    print()

    # --- Summary ---
    max_mhz = rows[-1]["freq_mhz"] if rows else 0
    print("=" * 78)
    print("  SWEET SPOT SUMMARY  (lowest freq with ≥95% of max throughput)")
    print("=" * 78)

    if "stride" in sweet:
        ratio = sweet["stride"] / max_mhz * 100
        print(f"  Stride  : {sweet['stride']:>5} MHz  ({ratio:.0f}% of {max_mhz} MHz)")
    if "chase" in sweet:
        ratio = sweet["chase"] / max_mhz * 100
        print(f"  Chase   : {sweet['chase']:>5} MHz  ({ratio:.0f}% of {max_mhz} MHz)")
    print(f"  Compute :   always scales linearly — no sweet spot")
    print()

    # --- Interpretation ---
    if "stride" in sweet and sweet["stride"] <= max_mhz * 0.6:
        print("  → STRIDE is heavily memory-bound.")
        print("    Throughput barely drops at low frequency.")
        print("    DVFS can aggressively lower freq to save energy.")
    elif "stride" in sweet:
        print("  → STRIDE has a significant compute component.")
        print("    Be conservative with frequency reduction.")

    if "chase" in sweet and sweet["chase"] <= max_mhz * 0.5:
        print("  → CHASE is purely latency-bound (DRAM round-trip).")
        print("    This is the strongest signal for DVFS energy savings.")
    elif "chase" in sweet:
        print("  → CHASE sweet spot is high — possible prefetcher effect")
        print("    or L3 large enough to hold the working set.")
        print("    Try larger array (-m 512).")
    print()


def main():
    parser = argparse.ArgumentParser(
        description="Run memfreq_bench and visualize sweet spot")
    parser.add_argument("--file", "-f",
                        help="Parse existing output file instead of running")
    parser.add_argument("--json", "-j", action="store_true",
                        help="Also save results as JSON")
    args, extra = parser.parse_known_args()

    if args.file:
        with open(args.file) as fh:
            text = fh.read()
    else:
        text = run_bench(extra)

    data = parse_output(text)
    if not data["rows"]:
        print("ERROR: no data rows parsed", file=sys.stderr)
        sys.exit(1)

    visualize(data)

    if args.json:
        out = {
            "meta": data["meta"],
            "sweet_spot_mhz": data["sweet"],
            "data": data["rows"],
        }
        jpath = "memfreq_results.json"
        with open(jpath, "w") as f:
            json.dump(out, f, indent=2)
        print(f"  JSON saved to {jpath}")


if __name__ == "__main__":
    main()
