#!/usr/bin/env bash
# Build both production binaries: matrixdb (CLI) and matrixdbd (daemon). One command, no cmake.
#   ./build_all.sh             # uses clang++ (falls back to g++)
#   CXX=g++ ./build_all.sh     # force a compiler
set -eu
cd "$(dirname "$0")"

if [ -n "${CXX:-}" ]; then :; elif command -v clang++ >/dev/null 2>&1; then CXX=clang++; else CXX=g++; fi

# Extract and echo version
VERSION=$(grep 'const char\* MATRIXDB_VERSION' version.hpp | head -1 | grep -o '"[^"]*"' | tr -d '"')
echo "Building MatrixDB v$VERSION..."

# Build both binaries
"$CXX" -std=c++20 -O2 matrixdb_cli.cpp -o matrixdb
"$CXX" -std=c++20 -O2 matrixdbd.cpp -o matrixdbd

echo "Built matrixdb and matrixdbd (v$VERSION)"
