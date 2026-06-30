#!/usr/bin/env python3
"""Measured CLI benchmark for matrixdb. Reproducible (seeded data).

  ./bench_cli.py [rows]        # default 2,000,000

Builds matrixdb (-O3), generates an N-row 2-column CSV, then reports:
  - LOAD throughput: wall-clock of a load-only session minus a startup baseline (CSV MB/s, rows/s).
  - QUERY latency: in-process .timing (µs) per query, warm = min over repeated runs.
Numbers are honest CLI-path measurements (parse + ingest + scan), not raw kernel bandwidth.
"""
import os, re, shutil, subprocess, sys, time, random

N = int(sys.argv[1]) if len(sys.argv) > 1 else 2_000_000
CSV = "/tmp/mdb_bench.csv"
BIN = "/tmp/mdb_bench"
HERE = os.path.dirname(os.path.abspath(__file__))

cxx = os.environ.get("CXX") or ("clang++" if shutil.which("clang++") else "g++")
print(f"building matrixdb (-O3, {cxx}) ...")
subprocess.run([cxx, "-std=c++20", "-O3", os.path.join(HERE, "matrixdb_cli.cpp"), "-o", BIN], check=True)

print(f"generating {N:,} rows -> {CSV} ...")
random.seed(42)
with open(CSV, "w") as f:
    f.write("amount,region\n")
    f.writelines(f"{random.randint(0, 1_000_000)},{random.randint(0, 9)}\n" for _ in range(N))
sz = os.path.getsize(CSV)


def wall(script, reps=3):
    """Min wall-clock over `reps` runs of a matrixdb session fed `script` (output discarded)."""
    best = float("inf")
    for _ in range(reps):
        t = time.perf_counter()
        subprocess.run([BIN], input=script.encode(), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        best = min(best, time.perf_counter() - t)
    return best


loadcmds = f".load {CSV} amount u32 col0 header\n.load {CSV} region u32 col1 header\n"
base = wall(".quit\n")                      # engine startup + teardown
load = wall(loadcmds + ".quit\n") - base    # two full CSV passes (one per column)

QUERIES = [
    ("SUM(amount)            full scan",   "SELECT SUM(amount)"),
    ("SUM WHERE amount>500k   filtered",   "SELECT SUM(amount) WHERE amount > 500000"),
    ("SUM GROUP BY region    10 groups",   "SELECT SUM(amount) GROUP BY region"),
    ("COUNT(DISTINCT region)",             "SELECT COUNT(DISTINCT region)"),
]
K = 6
qscript = ".timing on\n" + loadcmds + "".join((q + "\n") * K for _, q in QUERIES) + ".quit\n"
out = subprocess.run([BIN], input=qscript.encode(), capture_output=True).stdout.decode("utf-8", "replace")
us = [int(m) for m in re.findall(r"\((\d+) µs\)", out)]

print()
print(f"dataset      : {N:,} rows x 2 u32 columns, CSV {sz/1e6:.0f} MB")
print(f"startup      : {base*1000:.1f} ms")
print(f"load         : {load*1000:.0f} ms for 2 columns "
      f"-> {2*sz/1e6/load:,.0f} MB/s CSV, {2*N/load/1e6:.1f}M values/s")
print("query latency (warm = min of %d runs):" % K)
for i, (name, _) in enumerate(QUERIES):
    vals = us[i*K:(i+1)*K]
    if not vals:
        print(f"  {name:34s}: (no timing captured)"); continue
    warm = min(vals)
    rate = N / (warm/1e6) if warm else 0
    print(f"  {name:34s}: {warm/1000:7.2f} ms warm  [{rate/1e6:,.0f}M rows/s]")
