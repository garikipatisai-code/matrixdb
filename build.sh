#!/usr/bin/env bash
# Build the matrixdb CLI into ./matrixdb. One command, no cmake.
#   ./build.sh             # uses clang++ (falls back to g++)
#   CXX=g++ ./build.sh     # force a compiler
set -eu
cd "$(dirname "$0")"

if [ -n "${CXX:-}" ]; then :; elif command -v clang++ >/dev/null 2>&1; then CXX=clang++; else CXX=g++; fi

"$CXX" -std=c++20 -O3 matrixdb_cli.cpp -o matrixdb
echo "built ./matrixdb  (CXX=$CXX)  —  run it:  ./matrixdb   or   ./matrixdb -f examples/tour.sql"
