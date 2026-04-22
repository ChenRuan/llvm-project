#!/usr/bin/env bash
# Compile + link a single EasyJIT C API test statically and run it with
# EASYJIT_DUMP_IR set, so the post-specialization IR is dumped to disk.
#
# Usage:  dump_ir_test.sh <test_basename>   # e.g. partial_struct_binding
set -euo pipefail

TEST="${1:?test basename (without .c)}"
TRIM_ROOT="$(cd "$(dirname "$0")" && pwd)"
LLVM_ROOT="$(cd "$TRIM_ROOT/.." && pwd)"
EJ_ROOT="$LLVM_ROOT/easy-jit-llvm15"
LLVM_LIBDIR="$LLVM_ROOT/build-host/lib"
EJ_BUILD="$EJ_ROOT/build-host-easyjit"
OUT_DIR="$TRIM_ROOT/out"
DUMP_DIR="$TRIM_ROOT/ir_dumps"
mkdir -p "$OUT_DIR" "$DUMP_DIR"

CLANG="$LLVM_ROOT/build-host/bin/clang-15"
CXX="${CXX:-/usr/bin/c++}"
PASS="$EJ_BUILD/bin/EasyJitPass.so"
INC="$EJ_ROOT/include"
SRC="$EJ_ROOT/tests/c_api/${TEST}.c"
ARCHIVES_LIST="$TRIM_ROOT/archives_phase3.txt"

[[ -f "$SRC" ]] || { echo "no such test: $SRC" >&2; exit 2; }

OBJ="$OUT_DIR/${TEST}.easyjit.o"
echo "[compile] $OBJ"
"$CLANG" -O2 -Xclang -disable-O0-optnone -I"$INC" \
         -Xclang -fpass-plugin="$PASS" -c "$SRC" -o "$OBJ"

LLVM_ARCHIVES=""
while IFS= read -r a; do
  [[ -z "$a" || "${a:0:1}" == "#" ]] && continue
  LLVM_ARCHIVES+=" $LLVM_LIBDIR/$a"
done < "$ARCHIVES_LIST"

MIN_STDCPP="$EJ_ROOT/misc/minimal_libstdcpp/minimal_libstdcpp.o"
SUPCPP="/usr/lib/gcc/aarch64-linux-gnu/13/libsupc++.a"
LIBGCC="/usr/lib/gcc/aarch64-linux-gnu/13/libgcc.a"
LIBGCC_EH="/usr/lib/gcc/aarch64-linux-gnu/13/libgcc_eh.a"

STUB_OBJS=""
for s in ${EXTRA_STUBS:-asmparser_stub.o}; do
  f="$TRIM_ROOT/stubs/$s"
  [[ -f "$f" ]] && STUB_OBJS+=" $f"
done

OUT_BIN="$OUT_DIR/${TEST}.static"
echo "[link] $OUT_BIN"
$CXX -static -no-pie -O2 \
    -Wl,--gc-sections \
    -o "$OUT_BIN" \
    "$OBJ" \
    $STUB_OBJS \
    -Wl,--start-group \
    "$EJ_BUILD/bin/libEasyJitRuntime.a" \
    $LLVM_ARCHIVES \
    -Wl,--end-group \
    -nostdlib++ \
    "$MIN_STDCPP" \
    "$SUPCPP" "$LIBGCC_EH" "$LIBGCC" \
    -lm -lrt -ldl -lpthread -lc -lgcc -lgcc_eh

DUMP="$DUMP_DIR/${TEST}.spec.ll"
echo "[run with EASYJIT_DUMP_IR=$DUMP]"
EASYJIT_DUMP_IR="$DUMP" "$OUT_BIN" || echo "(test exit nonzero)"
echo "[dump written] $(wc -l <"$DUMP" 2>/dev/null || echo MISSING) lines  $DUMP"
