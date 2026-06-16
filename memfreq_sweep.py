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
import re
from datetime import datetime
from pathlib import Path
import statistics
import subprocess
import sys

BENCH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "memfreq_bench")
BAR_W = 40
EMDASH = "\u2014"  # em-dash for missing data (compute, n=1 std)

# Regexes for the new "# --- NAME (workload) ---" block headers emitted
# by memfreq_bench.c. Each block has a header line of the form
# `# --- <name> (<workload>) ---` (or `# --- plateau ---`, no workload).
_PERFREQ_HEADER = re.compile(r"^# --- per-freq stats \(([^)]+)\) ---$")
_SENS_HEADER = re.compile(r"^# --- sensitivity \(([^)]+)\) ---$")
_RAW_HEADER = re.compile(r"^# --- raw_samples \(([^)]+)\) ---$")
_PLATEAU_HEADER = re.compile(r"^# --- plateau ---$")
_CI_HEADER = re.compile(r"^# --- sweet-spot CI ---$")
# Generic catch-all so we can detect end-of-block before falling through.
_ANY_HEADER = re.compile(r"^# --- [^-].* ---$")

# Plateau row: "# <workload>  plateau_breakpoint: <value>  (annotation)"
# Examples (note the value after the colon can be an em-dash or a number):
#   "# stride   plateau_breakpoint: 2050 MHz  (slope ratio 18.3x, 95% sweet spot 2000 MHz)"
#   "# chase    plateau_breakpoint: \u2014  (no plateau; throughput keeps rising ...)"
_PLATEAU_VALUE = re.compile(r"^(\d+)\s*MHz\s*\(slope ratio ([0-9.]+)x,\s*"
                            r"(\d+)%\s*sweet spot (\d+)\s*MHz\)\s*$")
# Power sub-row: "45W at sweet spot (savings: 36% vs 71W at 2600 MHz)"
_POWER_VALUE = re.compile(r"^([0-9.]+)W\s+at sweet spot\s+"
                          r"\(savings:\s+([0-9.]+)%")


def run_bench(extra_args: list[str]) -> str:
    if not os.path.isfile(BENCH):
        print(f"ERROR: {BENCH} not found.  Compile first:", file=sys.stderr)
        print(f"  gcc -O2 -o memfreq_bench memfreq_bench.c stats.c -lm", file=sys.stderr)
        sys.exit(1)
    cmd = [BENCH] + extra_args
    print(f"$ {' '.join(cmd)}\n", file=sys.stderr)
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stderr, file=sys.stderr)
        sys.exit(result.returncode)
    return result.stdout


def _parse_per_freq_stats_block(lines):
    """Parse a `per-freq stats (X)` block.

    Header row may use either `Mops` or `MOps` (the C source has minor
    capitalization drift between stride/chase and random/compute blocks);
    we tolerate both. Data rows are tab-separated:
        freq_MHz  min_Mops  max_MOps  median_MOps  iqr_MOps
    """
    rows = []
    for ln in lines:
        if not ln.startswith("#"):
            continue
        # Skip comment/blank header lines within the block.
        body = ln[1:].strip()
        if not body or body.startswith("---") or "min" in body.lower():
            continue
        parts = body.split("\t")
        if len(parts) < 5:
            continue
        try:
            freq = int(parts[0])
            mn = float(parts[1])
            mx = float(parts[2])
            med = float(parts[3])
            iqr = float(parts[4])
        except ValueError:
            continue
        rows.append({
            "freq_mhz": freq,
            "min": mn,
            "max": mx,
            "median": med,
            "iqr": iqr,
        })
    return rows


def _parse_sensitivity_block(lines):
    """Parse a `sensitivity (X)` block.

    Data rows: `threshold<TAB>sweet_spot_MHz`, where sweet spot is either
    an integer (MHz) or an em-dash ("no plateau").
    """
    rows = []
    for ln in lines:
        if not ln.startswith("#"):
            continue
        body = ln[1:].strip()
        if not body or body.startswith("---") or "threshold" in body.lower():
            continue
        parts = body.split("\t")
        if len(parts) < 2:
            continue
        try:
            thr = float(parts[0])
        except ValueError:
            continue
        spot_raw = parts[1].strip()
        if spot_raw == EMDASH or spot_raw == "":
            spot = None
        else:
            try:
                spot = int(spot_raw)
            except ValueError:
                spot = None
        rows.append({"threshold": thr, "sweet_spot_mhz_or_null": spot})
    return rows


def _parse_plateau_block(lines):
    """Parse the `plateau` block (one row per workload).

    Each row: `# <workload>  plateau_breakpoint: <value>  (annotation)`
    where value is either `N MHz  (slope ratio X.Xx, 95% sweet spot N MHz)`
    or the em-dash + `(no plateau; ...)` annotation.

    Power sub-rows follow on the next line as
    `# <workload>  power: NNNW at sweet spot (savings: NN% vs NNNW at NNNN MHz)`
    or `# <workload>  power: N/A (...)`. We attach them to the matching workload
    entry (None when absent).
    """
    rows = []
    # First pass: collect all workload rows.
    for ln in lines:
        if not ln.startswith("#"):
            continue
        body = ln[1:].strip()
        if not body or body.startswith("---"):
            continue
        if "power:" in body:
            continue  # handled in second pass
        if "plateau_breakpoint:" not in body:
            continue
        # Split into "<workload>  plateau_breakpoint: <rest>"
        try:
            wl_part, rest = body.split("plateau_breakpoint:", 1)
        except ValueError:
            continue
        workload = wl_part.strip()
        rest = rest.strip()
        if rest.startswith(EMDASH):
            rows.append({
                "workload": workload,
                "breakpoint_mhz_or_null": None,
                "slope_ratio": None,
                "sweet_spot_mhz": None,
                "has_plateau": False,
                "power_w_or_null": None,
                "savings_pct_or_null": None,
            })
            continue
        m = _PLATEAU_VALUE.match(rest)
        if m:
            rows.append({
                "workload": workload,
                "breakpoint_mhz_or_null": int(m.group(1)),
                "slope_ratio": float(m.group(2)),
                "threshold_pct": int(m.group(3)),
                "sweet_spot_mhz": int(m.group(4)),
                "has_plateau": True,
                "power_w_or_null": None,
                "savings_pct_or_null": None,
            })
        else:
            # Unrecognized annotation — record the em-dash fallback so the
            # block still surfaces in the JSON output.
            rows.append({
                "workload": workload,
                "breakpoint_mhz_or_null": None,
                "slope_ratio": None,
                "sweet_spot_mhz": None,
                "has_plateau": False,
                "power_w_or_null": None,
                "savings_pct_or_null": None,
            })
    # Second pass: attach power sub-rows to their workload.
    for ln in lines:
        if not ln.startswith("#") or "power:" not in ln:
            continue
        body = ln[1:].strip()
        try:
            wl_part, rest = body.split("power:", 1)
        except ValueError:
            continue
        workload = wl_part.strip()
        # Find the matching row.
        for row in rows:
            if row["workload"] != workload:
                continue
            if "N/A" in rest:
                # No data — leave both fields None.
                break
            # Parse "NNNW at sweet spot (savings: NN% vs NNNW at NNNN MHz)".
            m = _POWER_VALUE.match(rest.strip())
            if m:
                row["power_w_or_null"] = float(m.group(1))
                row["savings_pct_or_null"] = float(m.group(2))
            break
    return rows


def _parse_raw_samples_block(lines):
    """Parse a `raw_samples (X)` block.

    Data rows: `freq_MHz<TAB>sample_idx<TAB>mops`.
    """
    rows = []
    for ln in lines:
        if not ln.startswith("#"):
            continue
        body = ln[1:].strip()
        if not body or body.startswith("---") or "freq_mhz" in body.lower():
            continue
        parts = body.split("\t")
        if len(parts) < 3:
            continue
        try:
            freq = int(parts[0])
            idx = int(parts[1])
            mops = float(parts[2])
        except ValueError:
            continue
        rows.append({"freq_mhz": freq, "sample_idx": idx, "mops": mops})
    return rows


def _parse_sweet_spot_ci_block(lines):
    """Parse a `sweet-spot CI` block.

    Data rows: `workload<TAB>sweet_MHz<TAB>low_MHz<TAB>high_MHz<TAB>method`
    where any of the MHz columns may be an em-dash ("no plateau").
    """
    rows = []
    for ln in lines:
        if not ln.startswith("#"):
            continue
        body = ln[1:].strip()
        if not body or body.startswith("---") or "workload" in body.lower():
            continue
        parts = body.split()
        if len(parts) < 5:
            continue
        wl = parts[0].strip()
        method = parts[-1].strip()
        def _int_or_none(s):
            s = s.strip()
            if s in ("\u2014", "-", ""):
                return None
            try:
                return int(s)
            except ValueError:
                return None
        rows.append({
            "workload": wl,
            "sweet_mhz": _int_or_none(parts[1]),
            "low_mhz":   _int_or_none(parts[2]),
            "high_mhz":  _int_or_none(parts[3]),
            "method":    method,
        })
    return rows


def parse_output(text: str) -> dict:
    meta = {}
    rows = []
    sweet = {}

    # Split into lines once; iterate as a list so we can pull header
    # boundaries for the new statistical blocks.
    lines = text.splitlines()

    per_freq_stats: dict[str, list] = {}
    sensitivity: dict[str, list] = {}
    plateau: list = []
    raw_samples: dict[str, list] = {}
    sweet_spot_ci: list = []

    # Column header mapping: built from the "# target_MHz ..." line emitted
    # by print_column_header() in the C code. Maps column names to their
    # tab-separated index so we handle all 6 output variants (3 power modes
    # × 2 chase modes) without hardcoding column counts.
    _COL_MAP = {
        "target_MHz":   "freq_mhz",
        "stride_Mops":  "stride_mops",
        "stride_%":     "stride_pct",
        "chase_Mops":   "chase_mops",
        "chase_%":      "chase_pct",
        "compute_Mops": "compute_mops",
        "compute_%":    "compute_pct",
    }
    col_indices: dict[str, int] = {}  # field_name → column index

    i = 0
    n = len(lines)
    while i < n:
        line = lines[i]

        # Existing meta + sweet-spot parsing (preamble).
        if line.startswith("# cpu="):
            for kv in line[2:].split():
                k, v = kv.split("=", 1)
                meta[k] = v
            i += 1
            continue
        if line.startswith("# stride  sweet spot:"):
            sweet["stride"] = int(line.split(":")[1].strip().split()[0])
            i += 1
            continue
        if line.startswith("# chase   sweet spot:"):
            sweet["chase"] = int(line.split(":")[1].strip().split()[0])
            i += 1
            continue
        if line.startswith("# stride_l3 sweet spot:"):
            sweet["stride_l3"] = int(line.split(":")[1].strip().split()[0])
            i += 1
            continue

        # New block headers — collect lines until next "# ---" header or EOF,
        # then dispatch to the appropriate sub-parser.
        m_perfreq = _PERFREQ_HEADER.match(line)
        if m_perfreq:
            wl = m_perfreq.group(1)
            j = i + 1
            block_lines = []
            while j < n and not _ANY_HEADER.match(lines[j]):
                block_lines.append(lines[j])
                j += 1
            per_freq_stats[wl] = _parse_per_freq_stats_block(block_lines)
            i = j
            continue
        m_sens = _SENS_HEADER.match(line)
        if m_sens:
            wl = m_sens.group(1)
            j = i + 1
            block_lines = []
            while j < n and not _ANY_HEADER.match(lines[j]):
                block_lines.append(lines[j])
                j += 1
            sensitivity[wl] = _parse_sensitivity_block(block_lines)
            i = j
            continue
        if _PLATEAU_HEADER.match(line):
            j = i + 1
            block_lines = []
            while j < n and not _ANY_HEADER.match(lines[j]):
                block_lines.append(lines[j])
                j += 1
            plateau = _parse_plateau_block(block_lines)
            i = j
            continue
        m_raw = _RAW_HEADER.match(line)
        if m_raw:
            wl = m_raw.group(1)
            j = i + 1
            block_lines = []
            while j < n and not _ANY_HEADER.match(lines[j]):
                block_lines.append(lines[j])
                j += 1
            raw_samples[wl] = _parse_raw_samples_block(block_lines)
            i = j
            continue
        if _CI_HEADER.match(line):
            j = i + 1
            block_lines = []
            while j < n and not _ANY_HEADER.match(lines[j]):
                block_lines.append(lines[j])
                j += 1
            sweet_spot_ci = _parse_sweet_spot_ci_block(block_lines)
            i = j
            continue

        if line.startswith("#"):
            # Detect column header: "# target_MHz\tactual_MHz\t..."
            if line.startswith("# target_MHz") and not col_indices:
                header_cols = line[2:].split("\t")
                for idx, name in enumerate(header_cols):
                    field = _COL_MAP.get(name.strip())
                    if field:
                        col_indices[field] = idx
            i += 1
            continue

        if line.strip() and col_indices and "freq_mhz" in col_indices:
            parts = line.split("\t")
            row = {}
            for field, idx in col_indices.items():
                if idx < len(parts):
                    val = parts[idx].strip()
                    row[field] = int(val) if field == "freq_mhz" else float(val)
            if "freq_mhz" in row:
                rows.append(row)
        i += 1

    return {
        "meta": meta,
        "rows": rows,
        "sweet": sweet,
        "per_freq_stats": per_freq_stats,
        "sensitivity": sensitivity,
        "plateau": plateau,
        "raw_samples": raw_samples,
        "sweet_spot_ci": sweet_spot_ci,
    }


def parse_lmbench_output(text: str) -> dict:
    """Parse lmbench_freq_sweep.sh output TSV (cross-validation format).

    Format (from lmbench_freq_sweep.sh):
        target_MHz<TAB>actual_MHz<TAB>bw_mem_MBps<TAB>lat_mem_rd_ns
    First non-empty line is the header. Lines starting with '#' are skipped.

    Returns dict shaped like parse_output() so the existing JSON pipeline
    can consume it with a "format": "lmbench" marker. Sweet-spot detection
    differs from memfreq_bench because the lmbench metrics have different
    monotonicity:
      - bw_mem: higher is better → 95% of peak (matches stride logic)
      - lat_mem_rd: lower is better → within 5% of min (matches chase logic)
    """
    meta = {"format": "lmbench"}
    rows = []

    for line in text.splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        if s.startswith("target_MHz"):  # header row
            continue
        parts = s.split("\t")
        if len(parts) < 4:
            continue
        try:
            target = int(parts[0])
            actual = int(parts[1])
            bw     = float(parts[2])
            lat    = float(parts[3])
        except ValueError:
            continue
        rows.append({
            "target_mhz": target,
            "actual_mhz": actual,
            "bw_mem_mbps": bw,
            "lat_mem_rd_ns": lat,
        })

    if not rows:
        return {"meta": meta, "rows": [], "sweet": {}}

    rows.sort(key=lambda r: r["target_mhz"])

    bw_max  = max(r["bw_mem_mbps"]  for r in rows)
    lat_min = min(r["lat_mem_rd_ns"] for r in rows)
    for r in rows:
        r["bw_mem_pct"] = 100.0 * r["bw_mem_mbps"]  / bw_max
        # latency: lower is better → percent = min/lat (so smaller lat → higher %)
        r["lat_pct"]    = 100.0 * lat_min / r["lat_mem_rd_ns"]

    sweet: dict = {}
    # bw_mem sweet spot: lowest freq meeting 95% peak (mirrors memfreq_bench stride)
    for r in rows:
        if r["bw_mem_pct"] >= 95.0:
            sweet["bw_mem"] = r["target_mhz"]
            break
    # lat_mem_rd sweet spot: lowest freq whose latency is within 5% of min
    # (mirrors memfreq_bench chase: latency is DRAM-bound, plateau wide)
    for r in rows:
        if r["lat_mem_rd_ns"] <= 1.05 * lat_min:
            sweet["lat_mem_rd"] = r["target_mhz"]
            break

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

    # --- Sweet-spot CI (only when -r was used) ---
    if data.get("sweet_spot_ci"):
        print("=" * 78)
        print("  SWEET-SPOT CONFIDENCE INTERVAL  (bootstrap, B=1000, requires -r)")
        print("=" * 78)
        print(f"  {'workload':<10}  {'sweet':>7}  {'[low, high]':>16}  {'method':<15}")
        for r in data["sweet_spot_ci"]:
            sweet = r["sweet_mhz"]
            low = r["low_mhz"]
            high = r["high_mhz"]
            if sweet is None:
                rng = f"[{EMDASH}, {EMDASH}]"
                sweet_s = EMDASH
            else:
                rng = f"[{low}, {high}]"
                sweet_s = f"{sweet}"
            print(f"  {r['workload']:<10}  {sweet_s:>7}  {rng:>16}  {r['method']:<15}")
        print()

    # --- Sensitivity (only when -L was used) ---
    if data.get("sensitivity"):
        print("=" * 78)
        print("  THRESHOLD SENSITIVITY  (sweet spot at each threshold, requires -L)")
        print("=" * 78)
        # Find a workload to determine threshold list (assume all workloads share it).
        for wl, rows in data["sensitivity"].items():
            if not rows:
                continue
            print(f"\n  Workload: {wl}")
            print(f"  {'threshold':>10}  {'sweet_MHz':>10}")
            for row in rows:
                spot = row["sweet_spot_mhz_or_null"]
                spot_s = f"{spot}" if spot is not None else EMDASH
                print(f"  {row['threshold']:>10.2f}  {spot_s:>10}")
        print()

    # --- Plateau (default on, suppressed by -P) ---
    if data.get("plateau"):
        print("=" * 78)
        print("  PLATEAU DETECTION  (piecewise-linear breakpoint)")
        print("=" * 78)
        print(f"  {'workload':<10}  {'breakpoint':>11}  {'slope':>6}  "
              f"{'power@ss':>9}  {'savings':>8}")
        for r in data["plateau"]:
            if r["has_plateau"]:
                bp = f"{r['breakpoint_mhz_or_null']}"
                slope = f"{r['slope_ratio']:.1f}x"
            else:
                bp = EMDASH
                slope = EMDASH
            pw = r.get("power_w_or_null")
            sav = r.get("savings_pct_or_null")
            pw_s = f"{pw:.0f}W" if pw is not None else EMDASH
            sav_s = f"{sav:.0f}%" if sav is not None else EMDASH
            print(f"  {r['workload']:<10}  {bp:>11}  {slope:>6}  "
                  f"{pw_s:>9}  {sav_s:>8}")
        print()
        print("  → slope_ratio > 2.0 ⇒ real plateau (memory-bound signal).")
        print("  → savings = power saved by running at sweet spot vs. max freq.")
        print()


def visualize_lmbench(data: dict):
    """ASCII visualization for lmbench cross-validation results.

    Two bar charts:
      - bw_mem MB/s vs freq (bandwidth, parallels memfreq_bench's stride)
      - lat_mem_rd ns vs freq (latency, parallels memfreq_bench's chase)
    Plus a side-by-side sweet-spot summary that the user can compare
    against the corresponding memfreq_bench numbers.
    """
    rows = data["rows"]
    sweet = data["sweet"]

    print("=" * 78)
    print("  lmbench Cross-Validation — Frequency Sweep")
    print("=" * 78)
    print(f"  Source format: {data['meta'].get('format', '?')}")
    print(f"  Rows: {len(rows)}")
    if rows:
        fmin, fmax = rows[0]["target_mhz"], rows[-1]["target_mhz"]
        print(f"  Freq range:   {fmin} – {fmax} MHz")
    print()

    # --- bw_mem chart ---
    print("── bw_mem (memory bandwidth, ~stride) ───────────────────────────")
    print(f"{'MHz':>6}  {'MB/s':>10}  {'%':>5}  {'throughput':<{BAR_W}}")
    for r in rows:
        marker = " ◄ sweet spot" if (
            "bw_mem" in sweet and r["target_mhz"] == sweet["bw_mem"]) else ""
        print(f"{r['target_mhz']:>6}  {r['bw_mem_mbps']:>10.1f}  "
              f"{r['bw_mem_pct']:>5.1f}  {bar(r['bw_mem_pct'])}{marker}")
    print()

    # --- lat_mem_rd chart ---
    print("── lat_mem_rd (memory read latency, ~chase) ───────────────────────")
    print(f"{'MHz':>6}  {'ns':>8}  {'%':>5}  {'inverse-throughput':<{BAR_W}}")
    for r in rows:
        marker = " ◄ sweet spot" if (
            "lat_mem_rd" in sweet and r["target_mhz"] == sweet["lat_mem_rd"]) else ""
        # Bar shrinks as latency increases (since lat_pct is "min/lat" in %)
        print(f"{r['target_mhz']:>6}  {r['lat_mem_rd_ns']:>8.1f}  "
              f"{r['lat_pct']:>5.1f}  {bar(r['lat_pct'])}{marker}")
    print()

    # --- Summary ---
    max_mhz = rows[-1]["target_mhz"] if rows else 0
    print("=" * 78)
    print("  SWEET SPOT SUMMARY  (lowest freq meeting threshold)")
    print("=" * 78)
    if "bw_mem" in sweet:
        ratio = sweet["bw_mem"] / max_mhz * 100 if max_mhz else 0
        print(f"  bw_mem    : {sweet['bw_mem']:>5} MHz  "
              f"({ratio:.0f}% of {max_mhz} MHz, 95% peak)")
    else:
        print(f"  bw_mem    :   {EMDASH}  (no row met 95% peak)")
    if "lat_mem_rd" in sweet:
        ratio = sweet["lat_mem_rd"] / max_mhz * 100 if max_mhz else 0
        print(f"  lat_mem_rd: {sweet['lat_mem_rd']:>5} MHz  "
              f"({ratio:.0f}% of {max_mhz} MHz, latency within 5%)")
    else:
        print(f"  lat_mem_rd:   {EMDASH}  (no row within 5% of min)")
    print()
    print("  → Cross-validate against memfreq_bench:")
    print("    stride sweet spot  ≈  bw_mem sweet spot   (bandwidth-bound)")
    print("    chase  sweet spot  ≈  lat_mem_rd sweet spot (latency-bound)")
    print("    If they agree within ±5%, two independent tools confirm the")
    print("    sweet-spot signal.  Larger gap = measurement-method difference")
    print("    (parallelism, prefetch, TLB) rather than a bug.")
    print()


def compare_runs(file_paths: list[str]) -> int:
    """Read N result files (JSON or TSV) and print cross-run sweet-spot stats.

    Stdlib only. Each file is autodetected: JSON if the first non-whitespace
    char is '{', else parsed as TSV via the existing parse_output(). For
    n >= 2, std uses sample stdev (n-1 denominator). For n = 1, std is
    reported as an em-dash (matching the C code's "no data" convention).
    """
    runs: list[dict] = []
    for path in file_paths:
        with open(path) as fh:
            text = fh.read()
        if text.lstrip().startswith("{"):
            runs.append(json.loads(text))
        else:
            runs.append(parse_output(text))

    if not runs:
        print("ERROR: no input files", file=sys.stderr)
        return 1

    # Normalize: --json output uses key "sweet_spot_mhz"; parse_output uses
    # "sweet". Pick whichever is present.
    normalized: list[dict] = []
    for r in runs:
        sweet = r.get("sweet") or r.get("sweet_spot_mhz") or {}
        normalized.append({"sweet": sweet})

    workloads = ["stride", "chase", "random"]
    print("=" * 78)
    print(f"  Cross-run sweet-spot comparison ({len(normalized)} runs)")
    print("=" * 78)
    print(f"{'workload':<10} {'mean_MHz':>10} {'std_MHz':>10} "
          f"{'min_MHz':>10} {'max_MHz':>10} {'range_MHz':>10}")
    for w in workloads:
        values = [n["sweet"].get(w) for n in normalized
                  if isinstance(n["sweet"].get(w), (int, float))]
        if not values:
            print(f"{w:<10} {EMDASH:>10} {EMDASH:>10} {EMDASH:>10} "
                  f"{EMDASH:>10} {EMDASH:>10}")
            continue
        mean_v = statistics.mean(values)
        std_v = statistics.stdev(values) if len(values) >= 2 else None
        min_v = min(values)
        max_v = max(values)
        std_cell = f"{std_v:>10.1f}" if std_v is not None else f"{EMDASH:>10}"
        print(f"{w:<10} {mean_v:>10.0f} {std_cell} "
              f"{min_v:>10.0f} {max_v:>10.0f} {max_v - min_v:>10.0f}")
    print()
    return 0



def generate_report(dir_path: str, output_path: str | None = None) -> int:
    """Scan a results directory and generate a Markdown report.

    Args:
        dir_path: Path to directory containing .txt result files
        output_path: Path for the report .md file (default: dir/REPORT.md)

    Returns:
        0 on success, 1 on error
    """
    data_dir = Path(dir_path)
    if not data_dir.is_dir():
        print(f"ERROR: not a directory: {data_dir}", file=sys.stderr)
        return 1

    out_path = Path(output_path) if output_path else data_dir / "REPORT.md"

    txt_files = sorted([
        f for f in data_dir.glob("*.txt")
        if f.stem not in ("SUMMARY", "COMPARISON", "FULL_REPORT")
    ])
    if not txt_files:
        print(f"ERROR: no test files found in {data_dir}", file=sys.stderr)
        return 1

    def _ss(data):
        s = data.get("sweet", {}) or {}
        st = s.get("stride", None)
        ch = s.get("chase", None)
        st_l3 = s.get("stride_l3", None)
        return (
            f"{st} MHz" if st else "\u2014",
            f"{ch} MHz" if ch else "\u2014",
            f"{st_l3} MHz" if st_l3 else "\u2014"
        )

    def _max_bw(data):
        rows = data.get("rows", [])
        if not rows:
            return 0.0
        peak = max((r.get("stride_mops", 0) for r in rows), default=0)
        return peak * 8.0 / 1.048576

    def _plateau(data):
        for p in (data.get("plateau", []) or []):
            if p.get("workload") == "stride" and p.get("breakpoint_mhz_or_null"):
                return f'{p["breakpoint_mhz_or_null"]} MHz'
        return "\u2014"

    parsed = []
    for f in txt_files:
        try:
            parsed.append((f.stem, parse_output(f.read_text(encoding="utf-8", errors="replace"))))
        except Exception as exc:
            parsed.append((f.stem, None))
            print(f"  WARN: {f.name}: {exc}", file=sys.stderr)

    groups = {"stride": [], "multicore": [], "numa": [], "special": [], "scale": [], "other": []}
    for name, data in parsed:
        if re.match(r'^s\d+$', name):
            groups["stride"].append((name, data))
        elif re.match(r'^mc\d+$', name):
            groups["multicore"].append((name, data))
        elif re.match(r'^n\d+c_m\d+mem_', name):
            groups["numa"].append((name, data))
        elif name in ("random", "flush", "randflush"):
            groups["special"].append((name, data))
        elif name.startswith("half_") or name.startswith("full_"):
            groups["scale"].append((name, data))
        else:
            groups["other"].append((name, data))

    lines = [
        "# memfreq_bench 测试报告",
        "",
        f"> **测试目录**: {data_dir.resolve()}",
        f"> **测试数量**: {len(txt_files)}",
        f"> **报告生成**: {datetime.now():%Y-%m-%d %H:%M:%S}",
        "",
    ]

    # Summary table
    lines.append("## 测试总览")
    lines.append("")
    lines.append("| 测试 | Stride Sweet Spot | Chase Sweet Spot | Stride L3-Resident |")
    lines.append("|------|-----|-----|-----|")
    for name, data in parsed:
        if data is None:
            lines.append(f"| {name} | (解析失败) | (解析失败) | (解析失败) |")
        else:
            st, ch, st_l3 = _ss(data)
            lines.append(f"| {name} | {st} | {ch} | {st_l3} |")
    lines.append("")

    # Per-group tables
    for gname, gtitle in [
        ("stride", "Stride Grid — 不同 stride 的甜点"),
        ("multicore", "多核扫描 — 核心数对带宽和甜点的影响"),
        ("numa", "NUMA 矩阵 — 本地访问 vs 远程访问"),
        ("special", "特殊访问模式 — Random / Flush"),
        ("scale", "半核与全核对比"),
        ("other", "其他测试"),
    ]:
        items = groups[gname]
        if not items:
            continue
        lines.append(f"## {gtitle}")
        lines.append("")
        lines.append("| 测试 | 最大带宽 MB/s | Stride Spot | Chase Spot | Stride L3 | 平台期 |")
        lines.append("|------|-------------:|:-----------:|:----------:|:---------:|:------:|")
        for name, data in items:
            if data is None:
                lines.append(f"| {name} | \u2014 | \u2014 | \u2014 | \u2014 | \u2014 |")
                continue
            st, ch, st_l3 = _ss(data)
            bw = _max_bw(data)
            bp = _plateau(data)
            lines.append(f"| {name} | {bw:.0f} | {st} | {ch} | {st_l3} | {bp} |")
        lines.append("")

    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"报告已生成: {out_path}")
    return 0



def main():
    parser = argparse.ArgumentParser(
        description="Run memfreq_bench and visualize sweet spot")
    parser.add_argument("--file",
                        help="Parse existing output file instead of running")
    parser.add_argument("--format", choices=["memfreq_bench", "lmbench"],
                        default="memfreq_bench",
                        help="Input format: 'memfreq_bench' (default, C binary) "
                             "or 'lmbench' (lmbench_freq_sweep.sh output)")
    parser.add_argument("--json", "-j", action="store_true",
                        help="Also save results as JSON")
    parser.add_argument("--report", metavar="DIR",
                    help="Scan a results directory and generate a Markdown report")
    parser.add_argument("--compare", nargs="+", metavar="FILE",
                        help="Compare N result files (JSON or TSV) and "
                             "report cross-run sweet-spot statistics")
    args, extra = parser.parse_known_args()

    if args.report:
        return generate_report(args.report)

    if args.compare:
        return compare_runs(args.compare)

    # --format lmbench: lmbench cross-validation path
    if args.format == "lmbench":
        if not args.file:
            print("ERROR: --format lmbench requires --file", file=sys.stderr)
            sys.exit(1)
        with open(args.file) as fh:
            text = fh.read()
        data = parse_lmbench_output(text)
        if not data["rows"]:
            print("ERROR: no data rows parsed (expected target_MHz header + TSV rows)",
                  file=sys.stderr)
            sys.exit(1)
        visualize_lmbench(data)
        if args.json:
            out = {
                "meta": data["meta"],
                "sweet_spot_mhz": data["sweet"],
                "data": data["rows"],
            }
            jpath = "lmbench_results.json"
            with open(jpath, "w") as f:
                json.dump(out, f, indent=2)
            print(f"  JSON saved to {jpath}")
        return 0

    # Default: memfreq_bench path (run C binary or parse its TSV)
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
            "per_freq_stats": data.get("per_freq_stats", {}),
            "sensitivity": data.get("sensitivity", {}),
            "plateau": data.get("plateau", []),
            "raw_samples": data.get("raw_samples", {}),
            "sweet_spot_ci": data.get("sweet_spot_ci", []),
            "data": data["rows"],
        }
        jpath = "memfreq_results.json"
        with open(jpath, "w") as f:
            json.dump(out, f, indent=2)
        print(f"  JSON saved to {jpath}")


if __name__ == "__main__":
    main()

