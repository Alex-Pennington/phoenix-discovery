#!/bin/bash
# Build phoenix-discovery with MSYS2/UCRT64

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

echo "=== Building phoenix-discovery ==="

# Create build directory
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure
cmake -G "MSYS Makefiles" ..

# Build
make -j$(nproc)

echo ""
echo "=== Build complete ==="
echo "Test program: $BUILD_DIR/test_discovery.exe"
echo ""
echo "To test:"
echo "  Terminal 1: ./test_discovery server SDR1"
echo "  Terminal 2: ./test_discovery client WF1"
