#!/usr/bin/env bash
# Build the round-7 light-codegen global_snapshot driver.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
LLVM_ROOT="$(cd "$HERE/../.." && pwd)"
LIB="$LLVM_ROOT/build-host/lib"
INC="$LLVM_ROOT/llvm/include"
BUILD_INC="$LLVM_ROOT/build-host/include"
OUT_DIR="$HERE/out"
mkdir -p "$OUT_DIR"

CXX="${CXX:-/usr/bin/c++}"

# NOTE: -no-pie is critical. With -pie the loader would place g_cfg at an
# address requiring up to 4 MOVZ/MOVK halfwords; we already support that
# but -no-pie also keeps the .text+addresses deterministic for inspection.
$CXX -static -no-pie -O2 -s -std=c++17 \
  -DNDEBUG \
  -I"$INC" -I"$BUILD_INC" \
  -Wl,-Map="$OUT_DIR/light_global_snapshot.map" \
  -Wl,--gc-sections \
  "$HERE/light_aarch64.cpp" "$HERE/light_global_snapshot.cpp" \
  -o "$OUT_DIR/light_global_snapshot" \
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

echo "[build ok] $OUT_DIR/light_global_snapshot"
size "$OUT_DIR/light_global_snapshot"
