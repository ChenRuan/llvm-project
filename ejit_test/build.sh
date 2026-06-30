#!/usr/bin/env bash
# Build all EJIT integration tests.
# Usage: ./build.sh [--run] [--arch=x86|aarch64] [--build-dir=DIR] [--analyze-deps] [--lipo=FILE] [<test>...]
#   --run          Build and run all tests
#   --arch=<arch>  Target architecture (default: auto-detect from build dirs)
#   --build-dir=DIR Use a specific LLVM build dir instead of auto-detecting
#   --analyze-deps Build & show which LLVM .a files were actually linked
#   --lipo=<FILE>  Use a single lipo .o/.a (from lipo.py) instead of individual .a files
#   --host-sre-stubs / --no-host-sre-stubs
#                  Force-enable/disable Linux SRE platform stubs for local tests
#   test_name      Build only the named test (without .c extension)
#
# Requires a release build (static libs): ./build.sh release <x86|aarch64>
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

ARCH=""
BUILD_DIR_OVERRIDE=""
SELECTED=()
DO_RUN=false
ANALYZE_DEPS=false
LIPO_FILE=""
NO_STRIP=false
HOST_SRE_STUBS="auto"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --run)         DO_RUN=true ;;
    --analyze-deps) ANALYZE_DEPS=true ;;
    --arch=*)      ARCH="${1#--arch=}" ;;
    --arch)        ARCH="$2"; shift ;;
    --build-dir=*) BUILD_DIR_OVERRIDE="${1#--build-dir=}" ;;
    --build-dir)   BUILD_DIR_OVERRIDE="$2"; shift ;;
    --lipo=*)      LIPO_FILE="${1#--lipo=}" ;;
    --lipo)        LIPO_FILE="auto" ;;
    --no-strip)    NO_STRIP=true ;;
    --host-sre-stubs) HOST_SRE_STUBS="on" ;;
    --no-host-sre-stubs) HOST_SRE_STUBS="off" ;;
    *)             SELECTED+=("$1") ;;
  esac
  shift
done

# Auto-detect arch from available build dirs (prefer release, fallback to debug-static)
find_build_dir() {
  local arch="$1"
  for dir in \
    "${ROOT_DIR}/build_release_${arch}" \
    "${ROOT_DIR}/build_debug_${arch}_static"; do
    if [[ -d "${dir}" && -f "${dir}/lib/libLLVMEJIT.a" ]]; then
      echo "${dir}"; return 0
    fi
  done
  return 1
}

infer_arch_from_build_dir() {
  local dir="$1"
  local cache="${dir}/CMakeCache.txt"
  if [[ -f "${cache}" ]]; then
    if grep -Eq '^LLVM_TARGETS_TO_BUILD(:[^=]+)?=.*AArch64' "${cache}"; then
      echo "aarch64"; return 0
    fi
    if grep -Eq '^LLVM_TARGETS_TO_BUILD(:[^=]+)?=.*X86' "${cache}"; then
      echo "x86"; return 0
    fi
  fi
  return 1
}

if [[ -n "${BUILD_DIR_OVERRIDE}" ]]; then
  if [[ "${BUILD_DIR_OVERRIDE}" = /* ]]; then
    BUILD_DIR="${BUILD_DIR_OVERRIDE}"
  else
    BUILD_DIR="${ROOT_DIR}/${BUILD_DIR_OVERRIDE}"
  fi
  if [[ ! -d "${BUILD_DIR}" ]]; then
    echo "ERROR: build dir not found: ${BUILD_DIR}"
    exit 1
  fi
  if [[ -z "${ARCH}" ]]; then
    ARCH=$(infer_arch_from_build_dir "${BUILD_DIR}" || true)
    if [[ -z "${ARCH}" ]]; then
      echo "ERROR: cannot infer --arch from ${BUILD_DIR}/CMakeCache.txt"
      echo "  Pass --arch=x86 or --arch=aarch64 explicitly."
      exit 1
    fi
  fi
elif [[ -z "${ARCH}" ]]; then
  for a in x86 aarch64; do
    BUILD_DIR=$(find_build_dir "$a" || true)
    if [[ -n "${BUILD_DIR}" ]]; then ARCH="$a"; break; fi
  done
  if [[ -z "${ARCH}" ]]; then
    echo "ERROR: no build with static libs found."
    echo "  Run: ../build.sh release aarch64  (or release x86)"
    exit 1
  fi
else
  BUILD_DIR=$(find_build_dir "${ARCH}" || true)
  if [[ -z "${BUILD_DIR}" ]]; then
    echo "ERROR: no static-lib build for ${ARCH}."
    echo "  Run: ../build.sh release ${ARCH}"
    echo "    or ../build.sh debug ${ARCH} --static"
    exit 1
  fi
fi

# Everything from a single release build directory (static libs only)
CLANG="${BUILD_DIR}/bin/clang"
CXX="${CLANG}++"
BUILD_INCLUDE="${BUILD_DIR}/include"
LLVM_INCLUDE="${ROOT_DIR}/llvm/include"
INCLUDES="-I${LLVM_INCLUDE} -I${BUILD_INCLUDE}"

LD_LLD="${BUILD_DIR}/bin/ld.lld"
[[ -x "${LD_LLD}" ]] || { echo "ERROR: lld not found at ${LD_LLD}"; exit 1; }

EJIT_RUNTIME="${BUILD_DIR}/lib/libLLVMEJIT.a"
[[ -f "${EJIT_RUNTIME}" ]] || { echo "ERROR: libLLVMEJIT.a not found in ${BUILD_DIR}/lib/"; exit 1; }

cmake_bool_is_on() {
  local key="$1"
  local cache="${BUILD_DIR}/CMakeCache.txt"
  [[ -f "${cache}" ]] || return 1
  grep -Eq "^${key}(:[^=]+)?=(ON|TRUE|1)$" "${cache}"
}

# Match test wrapper generation to the selected LLVMEJIT runtime. Taskpool
# builds need the async wrapper ABI so funcIndex/lifecycle dimType fixups are
# emitted into the test object (__ejit_dimtype_*, ejit_register_lifecycle).
GLOBAL_COMPILE_FLAGS=""
if cmake_bool_is_on "EJIT_SRE_TASKPOOL" ||
   cmake_bool_is_on "EJIT_SRE_SHARED_TASKPOOL"; then
  GLOBAL_COMPILE_FLAGS="-mllvm -ejit-wrapper-async"
fi

USE_HOST_SRE_STUBS=false
if [[ "${HOST_SRE_STUBS}" == "on" ]]; then
  USE_HOST_SRE_STUBS=true
elif [[ "${HOST_SRE_STUBS}" == "auto" ]]; then
  if cmake_bool_is_on "EJIT_FREESTANDING" ||
     cmake_bool_is_on "EJIT_SRE_CODE_POOL"; then
    USE_HOST_SRE_STUBS=true
  fi
fi
HOST_SRE_STUB_SRC="${SCRIPT_DIR}/ejit_host_sre_stub.cpp"
if ${USE_HOST_SRE_STUBS} && [[ ! -f "${HOST_SRE_STUB_SRC}" ]]; then
  echo "ERROR: host SRE stub source not found: ${HOST_SRE_STUB_SRC}"
  exit 1
fi

# Minimal .a set verified by link-then-test on 2026-05-25.
# Core = EJIT's direct LINK_COMPONENTS + essential transitive deps
# (OrcJIT needs OrcTargetProcess/RuntimeDyld/BitWriter,
#  X86CodeGen needs GlobalISel/CFGuard/IRPrinter/Instrumentation,
#  CodeGen needs ObjCARCOpts/CGData,
#  AsmPrinter needs DebugInfoDWARF/CodeView/DWARFLowLevel/MCParser,
#  X86Desc needs MCDisassembler, JITLink needs Option, Core needs Remarks).
_set_min_libs() {
  local _l="${BUILD_DIR}/lib"
  # shared across all targets
  MIN_LIBS_COMMON="
    ${_l}/libLLVMCore.a ${_l}/libLLVMSupport.a ${_l}/libLLVMDemangle.a
    ${_l}/libLLVMBinaryFormat.a ${_l}/libLLVMBitReader.a ${_l}/libLLVMBitstreamReader.a
    ${_l}/libLLVMAnalysis.a ${_l}/libLLVMScalarOpts.a ${_l}/libLLVMInstCombine.a
    ${_l}/libLLVMipo.a ${_l}/libLLVMTransformUtils.a ${_l}/libLLVMCodeGen.a
    ${_l}/libLLVMCodeGenTypes.a ${_l}/libLLVMTarget.a ${_l}/libLLVMTargetParser.a
    ${_l}/libLLVMSelectionDAG.a ${_l}/libLLVMAsmPrinter.a ${_l}/libLLVMMC.a
    ${_l}/libLLVMObject.a ${_l}/libLLVMProfileData.a ${_l}/libLLVMExecutionEngine.a
    ${_l}/libLLVMOrcJIT.a ${_l}/libLLVMOrcShared.a ${_l}/libLLVMJITLink.a
    ${_l}/libLLVMRemarks.a ${_l}/libLLVMOption.a ${_l}/libLLVMMCDisassembler.a
    ${_l}/libLLVMIRPrinter.a ${_l}/libLLVMCFGuard.a
    ${_l}/libLLVMInstrumentation.a
    ${_l}/libLLVMDebugInfoCodeView.a ${_l}/libLLVMDebugInfoDWARF.a
    ${_l}/libLLVMDebugInfoDWARFLowLevel.a ${_l}/libLLVMMCParser.a
    ${_l}/libLLVMCGData.a ${_l}/libLLVMObjCARCOpts.a
    ${_l}/libLLVMGlobalISel.a
    ${_l}/libLLVMOrcTargetProcess.a ${_l}/libLLVMRuntimeDyld.a
    ${_l}/libLLVMBitWriter.a
  "
  case "$1" in
    x86)
      MIN_LIBS="${MIN_LIBS_COMMON}
        ${_l}/libLLVMX86CodeGen.a ${_l}/libLLVMX86Desc.a ${_l}/libLLVMX86Info.a"
      ;;
    aarch64)
      MIN_LIBS="${MIN_LIBS_COMMON}
        ${_l}/libLLVMAArch64CodeGen.a ${_l}/libLLVMAArch64Desc.a
        ${_l}/libLLVMAArch64Info.a ${_l}/libLLVMAArch64Utils.a"
      ;;
  esac
}

if [[ -n "${LIPO_FILE}" ]]; then
  # Lipo mode: use single pre-built .o/.a instead of individual .a files.
  # The lipo .a already bundles EJIT + all LLVM dependencies (see lipo.py).
  # If --lipo is passed without =FILE, auto-detect from build dir + arch.
  if [[ "${LIPO_FILE}" == "auto" ]]; then
    LIPO_FILE="${SCRIPT_DIR}/lipo/ejit.o"
  fi
  [[ -f "${LIPO_FILE}" ]] || { echo "ERROR: lipo file not found: ${LIPO_FILE}"; exit 1; }
  USE_LIPO=true
  LIPO_ABS=$(cd "$(dirname "${LIPO_FILE}")" && pwd)/$(basename "${LIPO_FILE}")
  echo "Lipo mode: ${LIPO_ABS}"
else
  USE_LIPO=false
  _set_min_libs "${ARCH}"
  OTHER_LIBS=$(echo "${MIN_LIBS}" | sed '/^$/d' | tr '\n' ' ')
fi
LINK_LIBS="-lz -lpthread -ldl"
STRIP_FLAG="-Wl,--strip-all"
if ${NO_STRIP}; then STRIP_FLAG=""; fi

OUTDIR="${SCRIPT_DIR}/out"
mkdir -p "${OUTDIR}"

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

ALL_TESTS=(
  ejit_attr_test
  ejit_complex_test
  ejit_config_api_test
  ejit_dump_test
  ejit_external_idx_test
  ejit_fold_loop_test
  ejit_jit_verify_test
  ejit_likely_test
  ejit_lifecycle_test
  ejit_multidim_test
  ejit_multiversion_test
  ejit_nested_struct_test
  ejit_opt_level_test
  ejit_manual_register_test
  ejit_multi_tu_test
  ejit_baremetal_link_test
  ejit_perf_bench
  ejit_ptr_period_test
  ejit_trace_test
)

# Per-test compile flags (e.g. for disabling global constructors)
declare -A COMPILE_FLAGS
COMPILE_FLAGS[ejit_manual_register_test]="-mllvm -enable-ejit-global-ctors=false"
# Bare-metal link test: no global ctors -> registration must use the static
# registry tables walked via linker-script-provided __start_/__stop_ symbols.
COMPILE_FLAGS[ejit_baremetal_link_test]="-mllvm -enable-ejit-global-ctors=false"

# Override the primary source file for a test (default: <name>.c). Lets a test
# reuse existing sources under a different build/link recipe without copying.
declare -A PRIMARY_SRC
PRIMARY_SRC[ejit_baremetal_link_test]="ejit_multi_tu_test.c"

# Custom linker script (-Wl,-T) for a test, relative to this dir.
declare -A LINKER_SCRIPT
LINKER_SCRIPT[ejit_baremetal_link_test]="ejit_baremetal.ld"

# Extra translation units linked alongside <test>.c (space-separated, with .c).
# Used by multi-TU tests that exercise cross-TU linking.
declare -A EXTRA_SRCS
EXTRA_SRCS[ejit_multi_tu_test]="ejit_multi_tu_test_b.c"
EXTRA_SRCS[ejit_baremetal_link_test]="ejit_multi_tu_test_b.c"

declare -A TEST_ARGS
TEST_ARGS[ejit_complex_test]="0 1 2 3"
TEST_ARGS[ejit_fold_loop_test]="0"
TEST_ARGS[ejit_opt_level_test]="L2"
TEST_ARGS[ejit_multiversion_test]="0 3 7"
TEST_ARGS[ejit_jit_verify_test]="0 1 5"
TEST_ARGS[ejit_likely_test]="0 1 7"
TEST_ARGS[ejit_external_idx_test]="3 1"
TEST_ARGS[ejit_lifecycle_test]="3 7 2"
TEST_ARGS[ejit_multidim_test]="0"
TEST_ARGS[ejit_nested_struct_test]="0"
TEST_ARGS[ejit_trace_test]="0"
TEST_ARGS[ejit_attr_test]="0"
TEST_ARGS[ejit_config_api_test]="0"
TEST_ARGS[ejit_perf_bench]="0 1"
TEST_ARGS[ejit_dump_test]="0 3"
TEST_ARGS[ejit_ptr_period_test]="0 1 3"
TEST_ARGS[ejit_multi_tu_test]="0 3"
TEST_ARGS[ejit_baremetal_link_test]="0 3"

if [[ ${#SELECTED[@]} -eq 0 ]]; then
  SELECTED=("${ALL_TESTS[@]}")
fi

# --- Build ---
build_one() {
  local name="$1"
  local src="${SCRIPT_DIR}/${PRIMARY_SRC[${name}]:-${name}.c}"
  local obj; obj=$(mktemp "${TMPDIR:-/tmp}/ejit_${name}_XXXXXX.o")
  local bin="${OUTDIR}/${name}"
  local map_file; map_file=$(mktemp "${TMPDIR:-/tmp}/ejit_${name}_XXXXXX.map")

  if [[ ! -f "${src}" ]]; then
    echo -e "${RED}  SKIP: ${src} not found${NC}"
    return 1
  fi

  echo "  Compiling $(basename "${src}") ..."
  local extra_flags="${COMPILE_FLAGS[${name}]:-}"
  "${CLANG}" -O2 ${INCLUDES} ${GLOBAL_COMPILE_FLAGS} ${extra_flags} -c "${src}" -o "${obj}" ||
    return 1

  # Compile any extra translation units (multi-TU tests) and link them all.
  local objs=("${obj}")
  if ${USE_HOST_SRE_STUBS}; then
    echo "  Compiling host SRE stubs ..."
    local sobj; sobj=$(mktemp "${TMPDIR:-/tmp}/ejit_${name}_sre_stub_XXXXXX.o")
    "${CXX}" -O2 ${INCLUDES} -c "${HOST_SRE_STUB_SRC}" -o "${sobj}" ||
      return 1
    objs+=("${sobj}")
  fi
  for extra in ${EXTRA_SRCS[${name}]:-}; do
    local esrc="${SCRIPT_DIR}/${extra}"
    if [[ ! -f "${esrc}" ]]; then
      echo -e "${RED}  SKIP: extra TU ${esrc} not found${NC}"
      return 1
    fi
    echo "  Compiling ${extra} ..."
    local eobj; eobj=$(mktemp "${TMPDIR:-/tmp}/ejit_${name}_tu_XXXXXX.o")
    "${CLANG}" -O2 ${INCLUDES} ${GLOBAL_COMPILE_FLAGS} ${extra_flags} -c "${esrc}" -o "${eobj}" ||
      return 1
    objs+=("${eobj}")
  done

  # Optional custom linker script (e.g. bare-metal __start_/__stop_ symbols).
  local lds_flag=""
  local lds="${LINKER_SCRIPT[${name}]:-}"
  if [[ -n "${lds}" ]]; then
    if [[ ! -f "${SCRIPT_DIR}/${lds}" ]]; then
      echo -e "${RED}  SKIP: linker script ${SCRIPT_DIR}/${lds} not found${NC}"
      return 1
    fi
    lds_flag="-Wl,-T,${SCRIPT_DIR}/${lds}"
  fi

  echo "  Linking ${name} (${ARCH}) ..."
  if ${USE_LIPO}; then
    "${CXX}" -fuse-ld="${LD_LLD}" \
      -Os -Wl,--gc-sections ${STRIP_FLAG} ${lds_flag} \
      "${LIPO_ABS}" \
      ${LINK_LIBS} \
      "${objs[@]}" -o "${bin}" ||
      return 1
  else
    "${CXX}" -fuse-ld="${LD_LLD}" \
      -Os -Wl,--gc-sections ${STRIP_FLAG} ${lds_flag} \
      -Wl,--whole-archive "${EJIT_RUNTIME}" -Wl,--no-whole-archive \
      ${OTHER_LIBS} \
      ${LINK_LIBS} \
      -Wl,-M \
      "${objs[@]}" -o "${bin}" > "${map_file}" 2>&1 ||
      return 1
  fi

  echo -e "  ${GREEN}OK${NC}: ${bin}"

  # Show dependency summary on request
  if ${ANALYZE_DEPS}; then
    if ${USE_LIPO}; then
      local lipo_sz=$(du -h "${LIPO_ABS}" | cut -f1)
      echo "         Lipo mode: 1 .a (${lipo_sz}) — ${LIPO_ABS}"
    else
      local deps
      deps=$(grep -oP "${BUILD_DIR}/lib/\K[^(]+\.a" "${map_file}" | sort -u)
      local dep_count
      dep_count=$(echo "${deps}" | grep -c '.' || true)
      echo "         ${dep_count} LLVM .a files linked:"
      echo "${deps}" | while IFS= read -r lib; do
        [[ -n "${lib}" ]] && echo "           ${lib}"
      done
    fi
  fi
}

# --- Run ---
run_one() {
  local name="$1"
  local bin="${OUTDIR}/${name}"
  local args="${TEST_ARGS[$name]:-}"

  if [[ ! -x "${bin}" ]]; then
    echo -e "${RED}  SKIP: ${bin} not found${NC}"
    return 1
  fi

  echo "  Running ${name} ${args} ..."
  local log; log=$(mktemp "${TMPDIR:-/tmp}/ejit_${name}_XXXXXX.log")
  if ${bin} ${args} > "${log}" 2>&1; then
    local nfails
    nfails=$(grep -c "FAIL" "${log}" 2>/dev/null || true)
    nfails=$(echo "${nfails}" | tr -d '[:space:]')
    if [[ -z "${nfails}" || "${nfails}" == "0" ]]; then
      echo -e "  ${GREEN}PASS${NC}"
    else
      echo -e "  ${RED}FAIL${NC} (${nfails} check failures)"
      grep "FAIL" "${log}" | head -5
    fi
  else
    echo -e "  ${RED}FAIL${NC} (exit code $?)"
    tail -10 "${log}"
  fi
}

echo "=== Building EJIT Integration Tests ==="
echo "Arch:    ${ARCH}"
echo "Build:   ${BUILD_DIR}"
echo "Output:  ${OUTDIR}"
echo "Tests:   ${SELECTED[*]}"
if [[ -n "${GLOBAL_COMPILE_FLAGS}" ]]; then
  echo "Flags:   ${GLOBAL_COMPILE_FLAGS}"
fi
if ${USE_HOST_SRE_STUBS}; then
  echo "Stubs:   host SRE platform"
fi
echo ""

BUILD_FAILED=0
for t in "${SELECTED[@]}"; do
  build_one "$t" || BUILD_FAILED=1
done
if [[ $BUILD_FAILED -ne 0 ]]; then
  echo -e "${RED}Build failed, stopping.${NC}"
  exit 1
fi

if ${DO_RUN}; then
  echo ""
  echo "=== Running Tests ==="
  for t in "${SELECTED[@]}"; do
    run_one "$t"
  done
fi

echo ""
echo "=== Done ==="
