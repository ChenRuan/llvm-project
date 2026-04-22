#!/usr/bin/env bash
# Build the round-5 light-codegen PoC.
# Links only LLVMIRReader + LLVMBitReader + LLVMCore + LLVMBitstreamReader +
# LLVMRemarks + LLVMSupport + LLVMDemangle. No CodeGen/SelectionDAG/AArch64*/
# OrcJIT/MC/RuntimeDyld.
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
  -Wl,-Map="$OUT_DIR/light_add_int.map" \
  -Wl,--gc-sections \
  "$HERE/light_aarch64.cpp" "$HERE/light_add_int.cpp" \
  -o "$OUT_DIR/light_add_int" \
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
   \
  -lpthread -ldl -lm

echo "[build ok] $OUT_DIR/light_add_int"
size "$OUT_DIR/light_add_int"
file "$OUT_DIR/light_add_int"
