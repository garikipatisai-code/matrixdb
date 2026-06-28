#!/usr/bin/env bash
# CI gate for MatrixDB (CPU): compile + run every CPU test and the pipeline oracle.
# Exit 0 only if ALL pass; non-zero (= number of failures) otherwise. One command to verify the build.
#
#   ./run_tests.sh            # uses clang++ (falls back to g++ if clang++ is absent)
#   CXX=g++ ./run_tests.sh    # force a compiler
#
# Auto-discovers test_*.cpp (so new tests are picked up) and skips test_migration_gpu.cpp (needs nvcc/GPU).
# Portable: plain -O3 (no -mcpu), works on Linux/macOS/Colab.
set -u
cd "$(dirname "$0")"

# Pick a compiler: $CXX, else clang++, else g++.
if [ -n "${CXX:-}" ]; then :; elif command -v clang++ >/dev/null 2>&1; then CXX=clang++; else CXX=g++; fi
TMP="${TMPDIR:-/tmp}"
# SAN=1 builds every test under ASan+UBSan (QA-3) to catch UB/OOB — slower; use for a thorough pass.
if [ "${SAN:-0}" = "1" ]; then FLAGS="-std=c++20 -O1 -g -fsanitize=address,undefined"; MODE=" [ASan+UBSan]";
else FLAGS="-std=c++20 -O2 -Wall -Wextra"; MODE=""; fi
pass=0; fail=0; failed=""

echo "== MatrixDB CI (CXX=$CXX)$MODE =="
for src in test_*.cpp; do
    [ "$src" = "test_migration_gpu.cpp" ] && continue        # GPU/Colab only (nvcc)
    name="${src%.cpp}"
    if "$CXX" $FLAGS "$src" -o "$TMP/mdb_$name" 2>"$TMP/mdb_$name.err" \
       && "$TMP/mdb_$name" >"$TMP/mdb_$name.out" 2>&1; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1)); failed="$failed $name"
        echo "FAIL: $name"; tail -3 "$TMP/mdb_$name.err" "$TMP/mdb_$name.out" 2>/dev/null
    fi
done

# Pipeline oracle: the running binary must report the known scan-sum.
if "$CXX" -std=c++20 -O3 main.cpp -o "$TMP/mdb_main" 2>"$TMP/mdb_main.err" \
   && "$TMP/mdb_main" 2>&1 | grep -q "Scan result sum: 83886070 (oracle 83886070)"; then
    oracle="OK"
else
    oracle="FAIL"; fail=$((fail + 1)); failed="$failed ORACLE"
fi

echo "-----------------------------------------"
echo "tests passed: $pass   oracle: $oracle"
if [ "$fail" -eq 0 ]; then
    echo "ALL GREEN ($pass tests + oracle)"
    exit 0
fi
echo "FAILURES ($fail):$failed"
exit "$fail"
