#!/usr/bin/env bash
# Build a fully-static add_int minimum using a user-specified list of LLVM
# archives, so we can iteratively drop/add archives and measure text size.
#
# Usage:
#   link_static_add_int.sh <output-name> <archives-file>
# Where archives-file is a file with one LLVM archive basename per line, e.g.:
#   libLLVMOrcJIT.a
#   libLLVMCore.a
#   ...
# The script always adds:
#   - libEasyJitRuntime.a
#   - minimal_libstdcpp.o + libsupc++.a + libgcc.a + libgcc_eh.a
#   - libc.a + libm.a + other fixed system archives
# and links -static, and writes <output-name> + <output-name>.map.
set -euo pipefail

OUT_NAME="${1:?output name}"
ARCHIVES_LIST="${2:?archives-file}"

TRIM_ROOT="$(cd "$(dirname "$0")" && pwd)"
LLVM_ROOT="$(cd "$TRIM_ROOT/.." && pwd)"
EJ_ROOT="$LLVM_ROOT/easy-jit-llvm15"
LLVM_LIBDIR="$LLVM_ROOT/build-host/lib"
EJ_BUILD="$EJ_ROOT/build-host-easyjit"
OUT_DIR="$TRIM_ROOT/out"
mkdir -p "$OUT_DIR"

CLANG="$LLVM_ROOT/build-host/bin/clang-15"
CXX="${CXX:-/usr/bin/c++}"
PASS="$EJ_BUILD/bin/EasyJitPass.so"
INC="$EJ_ROOT/include"
SRC="$EJ_ROOT/tests/c_api/add_int.c"

# --- Compile add_int.o with EasyJIT pass (idempotent, cache in out/) ---
OBJ="$OUT_DIR/add_int.easyjit.o"
if [[ ! -f "$OBJ" || "$SRC" -nt "$OBJ" || "$PASS" -nt "$OBJ" ]]; then
  echo "[compile] $OBJ"
  "$CLANG" -O2 -Xclang -disable-O0-optnone -I"$INC" \
           -Xclang -fpass-plugin="$PASS" -c "$SRC" -o "$OBJ"
fi

# --- Assemble the LLVM archive list ---
LLVM_ARCHIVES=""
while IFS= read -r a; do
  [[ -z "$a" || "${a:0:1}" == "#" ]] && continue
  LLVM_ARCHIVES+=" $LLVM_LIBDIR/$a"
done < "$ARCHIVES_LIST"

MIN_STDCPP="$EJ_ROOT/misc/minimal_libstdcpp/minimal_libstdcpp.o"
SUPCPP="/usr/lib/gcc/aarch64-linux-gnu/13/libsupc++.a"
LIBGCC="/usr/lib/gcc/aarch64-linux-gnu/13/libgcc.a"
LIBGCC_EH="/usr/lib/gcc/aarch64-linux-gnu/13/libgcc_eh.a"

# Optional stub objects. Any .o inside trim-experiments/stubs/ whose name
# starts with "stub_" is prepended to the link so it satisfies references
# before --start-group pulls them from the real archive.
STUB_OBJS=""
if [[ -d "$TRIM_ROOT/stubs" ]]; then
  # Rebuild stubs from .c sources on demand.
  for src in "$TRIM_ROOT"/stubs/*.c; do
    [[ -f "$src" ]] || continue
    obj="${src%.c}.o"
    if [[ ! -f "$obj" || "$src" -nt "$obj" ]]; then
      "${CLANG}" -O2 -c "$src" -o "$obj"
    fi
  done
  # Only include stubs requested via $EXTRA_STUBS (space-separated basenames).
  for s in ${EXTRA_STUBS:-}; do
    f="$TRIM_ROOT/stubs/$s"
    [[ -f "$f" ]] && STUB_OBJS+=" $f"
  done
fi

OUT_BIN="$OUT_DIR/${OUT_NAME}"
MAP="$OUT_BIN.map"

echo "[link] $OUT_BIN  (archives: $(wc -l <"$ARCHIVES_LIST"))"
set -x
$CXX -static -no-pie -O2 -s \
    -Wl,--gc-sections \
    -Wl,-Map="$MAP" \
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
{ set +x; } 2>/dev/null

echo "[size]"
size "$OUT_BIN"
echo "[file]"
file "$OUT_BIN"
echo "[run]"
"$OUT_BIN"
