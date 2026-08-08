#!/usr/bin/env bash
# Builds the diagnostic LD_PRELOAD interceptor (libds4-intercept.so, 64-bit
# and 32-bit) directly with gcc. Standalone -- not part of the daemon build
# (root Makefile), since this tool only ever runs manually against a game
# process for tracing, never as part of ds4-translator/ds4-ctl.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

mkdir -p "$BUILD_DIR"

gcc -O3 -Wall -fPIC -shared \
    -o "$BUILD_DIR/libds4-intercept.so" "$SCRIPT_DIR/intercept.c" \
    -ldl -lpthread

echo "Built: $BUILD_DIR/libds4-intercept.so"

gcc -m32 -O3 -Wall -fPIC -shared \
    -o "$BUILD_DIR/libds4-intercept32.so" "$SCRIPT_DIR/intercept.c" \
    -ldl -lpthread

echo "Built: $BUILD_DIR/libds4-intercept32.so"
