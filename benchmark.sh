#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
BENCHMARK="$BUILD_DIR/tests/benchmark_pcache"

if [[ ! -x "$BENCHMARK" ]]; then
    echo "Building benchmark..."
    cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release > /dev/null
    cmake --build "$BUILD_DIR" --target benchmark_pcache
fi

echo "========================================"
echo " libpcache benchmarks  —  $(date '+%Y-%m-%d %H:%M:%S')"
echo "========================================"
echo ""

"$BENCHMARK"