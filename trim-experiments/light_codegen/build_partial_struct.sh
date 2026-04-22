#!/usr/bin/env bash
# Build the round-6 light-codegen partial_struct_binding driver.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
LLVM_ROOT="$(cd "$HERE/../.." && pwd)"
LIB="$LLVM_ROOT/build-host/lib"
INC="$LLVM_ROOT/llvm/include"
BUILD_INC="$LLVM_ROOT/build-host/include"
OUT_DIR="$HERE/out"
mkdir -p "$OUT_DIR"

CXX="${CXX:-/usr/bin/c++}"

$CXX -static -no-pie -O2 -s -std=c++17 \
  -DNDEBUG \
  -I"$INC" -I"$BUILD_INC" \
  -Wl,-Map="$OUT_DIR/light_partial_struct.map" \
  -Wl,--gc-sections \
  "$HERE/light_aarch64.cpp" "$HERE/light_partial_struct.cpp" \
  -o "$OUT_DIR/light_partial_struct" \
  -Wl,--start-group \
    "$LIB/libLLVMIRReader.a" \
    "$LIB/libLLVMAsmParser.a" \
    "$LIB/libLLVMBitReader.a" \
    "$LIB/libLLVMBitstreamReader.a" \
    "$LIB/libLLVMCore.a" \
    "$LIB/libLLVMRemarks.a" \
    "$LIB/libLLVMSupport.a" \
    "$LIB/libLLVMDemangle.a" \
    "$LIB/libLLVMBinaryFormat.a" \
  -Wl,--end-group \
  -lpthread -ldl -lm

echo "[build ok] $OUT_DIR/light_partial_struct"
size "$OUT_DIR/light_partial_struct"
