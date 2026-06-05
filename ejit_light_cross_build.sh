#!/usr/bin/env bash
#===-- ejit_light_cross_build.sh -----------------------------------------===#
# Configure/build EmbeddedJIT with the optional AArch64 light backend using a
# user-provided cross Clang toolchain.
#
# Typical use:
#   ./ejit_light_cross_build.sh \
#     --clang /opt/aarch64/bin/clang \
#     --clangxx /opt/aarch64/bin/clang++ \
#     --target aarch64-linux-gnu \
#     --sysroot /opt/aarch64/sysroot
#
# Big-endian example:
#   ./ejit_light_cross_build.sh \
#     --clang /opt/aarch64_be/bin/clang \
#     --clangxx /opt/aarch64_be/bin/clang++ \
#     --target aarch64_be-linux-gnu \
#     --sysroot /opt/aarch64_be/sysroot \
#     --target-triple aarch64_be-unknown-linux-gnu
#===----------------------------------------------------------------------===#

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
LLVM_SRC="${ROOT_DIR}/llvm"

BUILD_TYPE="Release"
CROSS_BUILD_DIR="${ROOT_DIR}/build-ejit-light-cross"
NATIVE_BUILD_DIR="${ROOT_DIR}/build-ejit-native-tools"
CLANG_BIN=""
CLANGXX_BIN=""
TARGET_TRIPLE="aarch64-linux-gnu"
EJIT_TARGET_TRIPLE=""
SYSROOT=""
LLVM_TARGETS="AArch64"
ENABLE_PROJECTS="clang;lld"
BUILD_TARGETS="LLVMEJIT"
DO_CONFIGURE=true
DO_BUILD=true
DO_NATIVE_TOOLS=true
DO_LIPO=false
DO_LIPO_BUILD_DEPS=true
EJIT_BARE_METAL=OFF
EJIT_FREESTANDING=OFF
EJIT_LIGHT_ONLY=false
EXTRA_CFLAGS=""
EXTRA_CXXFLAGS=""
EXTRA_LDFLAGS=""
EXTRA_CMAKE_ARGS=()
LIPO_ARCH="aarch64"
LIPO_OUTPUT=""
LIPO_LD=""
LIPO_CXXFLAGS=""
LIPO_REF_LDFLAGS=""
LIPO_LDFLAGS=""
LIPO_LIBS="-lz -lpthread -ldl"
LIPO_DEP_TARGETS="clang lld LLVMEJIT LLVMCore LLVMSupport LLVMDemangle LLVMBinaryFormat LLVMBitReader LLVMBitstreamReader LLVMAnalysis LLVMScalarOpts LLVMInstCombine LLVMipo LLVMTransformUtils LLVMCodeGen LLVMCodeGenTypes LLVMTarget LLVMTargetParser LLVMSelectionDAG LLVMAsmPrinter LLVMMC LLVMObject LLVMProfileData LLVMExecutionEngine LLVMOrcJIT LLVMOrcShared LLVMJITLink LLVMRemarks LLVMOption LLVMMCDisassembler LLVMIRPrinter LLVMOrcTargetProcess LLVMRuntimeDyld LLVMBitWriter LLVMGlobalISel LLVMAArch64CodeGen LLVMAArch64Desc LLVMAArch64Info LLVMAArch64Utils"
LIPO_LIGHT_ONLY_DEP_TARGETS="LLVMEJIT LLVMCore LLVMSupport LLVMDemangle LLVMBinaryFormat LLVMBitReader LLVMBitstreamReader LLVMAnalysis LLVMInstCombine LLVMTransformUtils"

log() {
  printf '[ejit-cross] %s\n' "$*"
}

die() {
  printf '[ejit-cross][error] %s\n' "$*" >&2
  exit 1
}

usage() {
  cat <<'EOF'
Usage:
  ./ejit_light_cross_build.sh \
    --clang /opt/aarch64/bin/clang \
    --clangxx /opt/aarch64/bin/clang++ \
    --target aarch64-linux-gnu \
    --sysroot /opt/aarch64/sysroot

Big-endian example:
  ./ejit_light_cross_build.sh \
    --clang /opt/aarch64_be/bin/clang \
    --clangxx /opt/aarch64_be/bin/clang++ \
    --target aarch64_be-linux-gnu \
    --sysroot /opt/aarch64_be/sysroot \
    --target-triple aarch64_be-unknown-linux-gnu

Options:
  --clang <path>              Cross C compiler. Required unless found in PATH.
  --clangxx <path>            Cross C++ compiler. Required unless found in PATH.
  --target <triple>           Clang target triple. Default: aarch64-linux-gnu.
  --sysroot <path>            Target sysroot passed to CMake/Clang.
  --build-dir <path>          Cross build dir. Default: build-ejit-light-cross.
  --native-build-dir <path>   Native tblgen build dir. Default: build-ejit-native-tools.
  --build-type <type>         CMake build type. Default: Release.
  --llvm-targets <targets>    LLVM_TARGETS_TO_BUILD. Default: AArch64.
  --projects <list>           LLVM_ENABLE_PROJECTS. Default: clang;lld.
  --targets <list>            Ninja targets. Default: LLVMEJIT.
                               Example: --targets "LLVMEJIT check-ejit-light-backend".
  --extra-targets <list>      Append extra CMake/Ninja targets to build.
  --target-triple <triple>    EJIT_DEFAULT_TARGET_TRIPLE for runtime policy.
  --light-only                Build LLVMEJIT without ORC/JITLink fallback.
  --bare-metal                Enable EJIT_BARE_METAL=ON.
  --freestanding              Enable EJIT_FREESTANDING=ON.
  --extra-cflags <flags>      Append a raw string to CMAKE_C_FLAGS.
  --extra-cxxflags <flags>    Append a raw string to CMAKE_CXX_FLAGS.
  --extra-ldflags <flags>     Append a raw string to exe/shared/module linker flags.
  --extra-cmake-arg <arg>     Append one raw CMake argument.
  --lipo                      After build, produce a single relocatable ejit .o.
  --no-lipo-build-deps        Do not auto-build the LLVM component archives used by lipo.py.
  --lipo-output <path>        Final lipo output. Default: ejit_test/lipo/ejit_<target>.o.
  --lipo-arch <arch>          lipo.py arch key. Default: aarch64.
  --lipo-ld <path>            Linker for lipo.py. Default: build-dir/bin/ld.lld or PATH ld.lld.
  --lipo-cxxflags <flags>     Extra flags for lipo.py reference compile/link.
  --lipo-ref-ldflags <flags>  Extra flags for lipo.py clang++ reference link.
  --lipo-ldflags <flags>      Extra flags for lipo.py direct ld.lld -r steps.
  --lipo-libs <libs>          Reference link libraries. Default: "-lz -lpthread -ldl".
                               Pass "" for no reference libraries.
  --no-native-tools           Reuse existing tblgen tools; do not build them.
  -c, --configure-only        Configure only.
  -b, --build-only            Build only.
  -D<arg>                     Extra CMake definition, for example -DLLVM_ENABLE_ASSERTIONS=ON.
  -h, --help                  Show this help.

Notes:
  Cross-compiling LLVM needs native llvm-tblgen/clang-tblgen executables.
  This script builds them in --native-build-dir unless --no-native-tools is set.
EOF
}

need_value() {
  [[ $# -ge 2 ]] || die "missing value for $1"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --clang) need_value "$@"; CLANG_BIN="$2"; shift 2 ;;
    --clangxx) need_value "$@"; CLANGXX_BIN="$2"; shift 2 ;;
    --target) need_value "$@"; TARGET_TRIPLE="$2"; shift 2 ;;
    --sysroot) need_value "$@"; SYSROOT="$2"; shift 2 ;;
    --build-dir) need_value "$@"; CROSS_BUILD_DIR="$2"; shift 2 ;;
    --native-build-dir) need_value "$@"; NATIVE_BUILD_DIR="$2"; shift 2 ;;
    --build-type) need_value "$@"; BUILD_TYPE="$2"; shift 2 ;;
    --llvm-targets) need_value "$@"; LLVM_TARGETS="$2"; shift 2 ;;
    --projects) need_value "$@"; ENABLE_PROJECTS="$2"; shift 2 ;;
    --targets) need_value "$@"; BUILD_TARGETS="$2"; shift 2 ;;
    --extra-targets) need_value "$@"; BUILD_TARGETS="${BUILD_TARGETS} $2"; shift 2 ;;
    --target-triple) need_value "$@"; EJIT_TARGET_TRIPLE="$2"; shift 2 ;;
    --light-only) EJIT_LIGHT_ONLY=true; EXTRA_CMAKE_ARGS+=("-DEJIT_LIGHT_BACKEND_ONLY=ON"); shift ;;
    --bare-metal) EJIT_BARE_METAL=ON; shift ;;
    --freestanding) EJIT_FREESTANDING=ON; shift ;;
    --extra-cflags) need_value "$@"; EXTRA_CFLAGS="${EXTRA_CFLAGS} $2"; shift 2 ;;
    --extra-cxxflags) need_value "$@"; EXTRA_CXXFLAGS="${EXTRA_CXXFLAGS} $2"; shift 2 ;;
    --extra-ldflags) need_value "$@"; EXTRA_LDFLAGS="${EXTRA_LDFLAGS} $2"; shift 2 ;;
    --extra-cmake-arg)
      need_value "$@"
      EXTRA_CMAKE_ARGS+=("$2")
      if [[ "$2" == "-DEJIT_LIGHT_BACKEND_ONLY=ON" ]]; then
        EJIT_LIGHT_ONLY=true
      fi
      shift 2
      ;;
    --lipo) DO_LIPO=true; shift ;;
    --no-lipo-build-deps) DO_LIPO_BUILD_DEPS=false; shift ;;
    --lipo-output) need_value "$@"; LIPO_OUTPUT="$2"; shift 2 ;;
    --lipo-arch) need_value "$@"; LIPO_ARCH="$2"; shift 2 ;;
    --lipo-ld) need_value "$@"; LIPO_LD="$2"; shift 2 ;;
    --lipo-cxxflags) need_value "$@"; LIPO_CXXFLAGS="${LIPO_CXXFLAGS} $2"; shift 2 ;;
    --lipo-ref-ldflags) need_value "$@"; LIPO_REF_LDFLAGS="${LIPO_REF_LDFLAGS} $2"; shift 2 ;;
    --lipo-ldflags) need_value "$@"; LIPO_LDFLAGS="${LIPO_LDFLAGS} $2"; shift 2 ;;
    --lipo-libs) need_value "$@"; LIPO_LIBS="$2"; shift 2 ;;
    --no-native-tools) DO_NATIVE_TOOLS=false; shift ;;
    -c|--configure-only) DO_BUILD=false; shift ;;
    -b|--build-only) DO_CONFIGURE=false; shift ;;
    -D*)
      EXTRA_CMAKE_ARGS+=("$1")
      if [[ "$1" == "-DEJIT_LIGHT_BACKEND_ONLY=ON" ]]; then
        EJIT_LIGHT_ONLY=true
      fi
      shift
      ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done

if [[ -z "${CLANG_BIN}" ]]; then
  CLANG_BIN="$(command -v clang || true)"
fi
if [[ -z "${CLANGXX_BIN}" ]]; then
  CLANGXX_BIN="$(command -v clang++ || true)"
fi

[[ -n "${CLANG_BIN}" ]] || die "clang not found; pass --clang"
[[ -n "${CLANGXX_BIN}" ]] || die "clang++ not found; pass --clangxx"
[[ -x "${CLANG_BIN}" ]] || die "clang is not executable: ${CLANG_BIN}"
[[ -x "${CLANGXX_BIN}" ]] || die "clang++ is not executable: ${CLANGXX_BIN}"
if [[ -n "${SYSROOT}" && ! -d "${SYSROOT}" ]]; then
  die "sysroot does not exist: ${SYSROOT}"
fi

if ${DO_CONFIGURE} && ${DO_NATIVE_TOOLS}; then
  log "Configuring native tblgen tools: ${NATIVE_BUILD_DIR}"
  cmake -S "${LLVM_SRC}" -B "${NATIVE_BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_TARGETS_TO_BUILD=Native \
    -DLLVM_ENABLE_PROJECTS=clang \
    -DLLVM_ENABLE_ZLIB=OFF \
    -DLLVM_ENABLE_ZSTD=OFF

  log "Building native tblgen tools"
  cmake --build "${NATIVE_BUILD_DIR}" --target llvm-tblgen clang-tblgen -j
fi

LLVM_TBLGEN="${NATIVE_BUILD_DIR}/bin/llvm-tblgen"
CLANG_TBLGEN="${NATIVE_BUILD_DIR}/bin/clang-tblgen"
if ${DO_CONFIGURE}; then
  [[ -x "${LLVM_TBLGEN}" ]] || die "missing native llvm-tblgen: ${LLVM_TBLGEN}"
  [[ -x "${CLANG_TBLGEN}" ]] || die "missing native clang-tblgen: ${CLANG_TBLGEN}"
fi

if ${DO_CONFIGURE}; then
  CMAKE_ARGS=(
    -S "${LLVM_SRC}"
    -B "${CROSS_BUILD_DIR}"
    -G Ninja
    "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
    -DCMAKE_SYSTEM_NAME=Linux
    "-DCMAKE_C_COMPILER=${CLANG_BIN}"
    "-DCMAKE_CXX_COMPILER=${CLANGXX_BIN}"
    "-DCMAKE_C_COMPILER_TARGET=${TARGET_TRIPLE}"
    "-DCMAKE_CXX_COMPILER_TARGET=${TARGET_TRIPLE}"
    "-DLLVM_TARGETS_TO_BUILD=${LLVM_TARGETS}"
    "-DLLVM_ENABLE_PROJECTS=${ENABLE_PROJECTS}"
    -DBUILD_SHARED_LIBS=OFF
    "-DLLVM_NATIVE_TOOL_DIR=${NATIVE_BUILD_DIR}/bin"
    "-DLLVM_TABLEGEN=${LLVM_TBLGEN}"
    "-DLLVM_TABLEGEN_EXE=${LLVM_TBLGEN}"
    -DLLVM_TABLEGEN_TARGET=
    "-DCLANG_TABLEGEN=${CLANG_TBLGEN}"
    "-DCLANG_TABLEGEN_EXE=${CLANG_TBLGEN}"
    -DCLANG_TABLEGEN_TARGET=
    -DLLVM_ENABLE_ZLIB=OFF
    -DLLVM_ENABLE_ZSTD=OFF
    -DLLVM_INCLUDE_BENCHMARKS=OFF
    -DLLVM_BUILD_BENCHMARKS=OFF
    -DBENCHMARK_ENABLE_TESTING=OFF
    -DEJIT_ENABLE_LIGHT_BACKEND=ON
    "-DEJIT_BARE_METAL=${EJIT_BARE_METAL}"
    "-DEJIT_FREESTANDING=${EJIT_FREESTANDING}"
    "-DCMAKE_C_FLAGS=-ffunction-sections -fdata-sections${EXTRA_CFLAGS}"
    "-DCMAKE_CXX_FLAGS=-ffunction-sections -fdata-sections${EXTRA_CXXFLAGS}"
    -DCMAKE_C_FLAGS_RELEASE=-Os\ -DNDEBUG
    -DCMAKE_CXX_FLAGS_RELEASE=-Os\ -DNDEBUG
  )

  if [[ -n "${EXTRA_LDFLAGS}" ]]; then
    CMAKE_ARGS+=(
      "-DCMAKE_EXE_LINKER_FLAGS=${EXTRA_LDFLAGS}"
      "-DCMAKE_SHARED_LINKER_FLAGS=${EXTRA_LDFLAGS}"
      "-DCMAKE_MODULE_LINKER_FLAGS=${EXTRA_LDFLAGS}"
    )
  fi

  if [[ -n "${SYSROOT}" ]]; then
    CMAKE_ARGS+=("-DCMAKE_SYSROOT=${SYSROOT}")
  fi
  if [[ -n "${EJIT_TARGET_TRIPLE}" ]]; then
    CMAKE_ARGS+=("-DEJIT_DEFAULT_TARGET_TRIPLE=${EJIT_TARGET_TRIPLE}")
  fi
  if ((${#EXTRA_CMAKE_ARGS[@]})); then
    CMAKE_ARGS+=("${EXTRA_CMAKE_ARGS[@]}")
  fi

  log "Configuring cross EmbeddedJIT light backend: ${CROSS_BUILD_DIR}"
  printf '[ejit-cross] cmake'
  printf ' %q' "${CMAKE_ARGS[@]}"
  printf '\n'
  cmake "${CMAKE_ARGS[@]}"
fi

if ${DO_BUILD}; then
  if ${DO_LIPO} && ${DO_LIPO_BUILD_DEPS}; then
    if ${EJIT_LIGHT_ONLY}; then
      BUILD_TARGETS="${BUILD_TARGETS} ${LIPO_LIGHT_ONLY_DEP_TARGETS}"
    else
      BUILD_TARGETS="${BUILD_TARGETS} ${LIPO_DEP_TARGETS}"
    fi
  fi
  read -r -a RAW_TARGET_ARRAY <<< "${BUILD_TARGETS}"
  TARGET_ARRAY=()
  TARGET_SEEN=" "
  for target in "${RAW_TARGET_ARRAY[@]}"; do
    if [[ "${TARGET_SEEN}" != *" ${target} "* ]]; then
      TARGET_ARRAY+=("${target}")
      TARGET_SEEN="${TARGET_SEEN}${target} "
    fi
  done
  log "Building targets in ${CROSS_BUILD_DIR}: ${TARGET_ARRAY[*]}"
  cmake --build "${CROSS_BUILD_DIR}" --target "${TARGET_ARRAY[@]}" -j
fi

if ${DO_LIPO} && ${DO_BUILD}; then
  LIPO_DIR="${ROOT_DIR}/ejit_test/lipo"
  LIPO_SCRIPT="${LIPO_DIR}/lipo.py"
  [[ -f "${LIPO_SCRIPT}" ]] || die "missing lipo.py: ${LIPO_SCRIPT}"

  if [[ -z "${LIPO_OUTPUT}" ]]; then
    SAFE_TARGET="${TARGET_TRIPLE//[^A-Za-z0-9_]/_}"
    LIPO_OUTPUT="${LIPO_DIR}/ejit_${SAFE_TARGET}.o"
  fi

  if [[ -z "${LIPO_LD}" ]]; then
    if [[ -x "${CROSS_BUILD_DIR}/bin/ld.lld" ]]; then
      LIPO_LD="${CROSS_BUILD_DIR}/bin/ld.lld"
    else
      LIPO_LD="$(command -v ld.lld || true)"
    fi
  fi
  [[ -n "${LIPO_LD}" ]] || die "ld.lld not found; pass --lipo-ld"
  [[ -x "${LIPO_LD}" ]] || die "lipo linker is not executable: ${LIPO_LD}"

  LIPO_COMMON_CXXFLAGS="--target=${TARGET_TRIPLE}${LIPO_CXXFLAGS}${EXTRA_CXXFLAGS}"
  if [[ -n "${SYSROOT}" ]]; then
    LIPO_COMMON_CXXFLAGS="${LIPO_COMMON_CXXFLAGS} --sysroot=${SYSROOT}"
  fi
  LIPO_REF_LINK_FLAGS="${LIPO_REF_LDFLAGS}${EXTRA_LDFLAGS}"
  LIPO_LD_R_FLAGS="${LIPO_LDFLAGS}"

  LIPO_EXTRACT_A="${LIPO_DIR}/libejit_lipo_${LIPO_ARCH}.a"
  LIPO_GC_A="${LIPO_DIR}/libejit_lipo_${LIPO_ARCH}_gc.a"

  log "Running lipo extract -> ${LIPO_EXTRACT_A}"
  if ${EJIT_LIGHT_ONLY}; then
    python3 "${LIPO_SCRIPT}" extract \
      --arch="${LIPO_ARCH}" \
      --build-dir="${CROSS_BUILD_DIR}" \
      --cxx="${CLANGXX_BIN}" \
      --ld="${LIPO_LD}" \
      --cxxflags="${LIPO_COMMON_CXXFLAGS}" \
      --ldflags="${LIPO_REF_LINK_FLAGS}" \
      --libs="${LIPO_LIBS}" \
      --light-only \
      --output="${LIPO_EXTRACT_A}"
  else
    python3 "${LIPO_SCRIPT}" extract \
      --arch="${LIPO_ARCH}" \
      --build-dir="${CROSS_BUILD_DIR}" \
      --cxx="${CLANGXX_BIN}" \
      --ld="${LIPO_LD}" \
      --cxxflags="${LIPO_COMMON_CXXFLAGS}" \
      --ldflags="${LIPO_REF_LINK_FLAGS}" \
      --libs="${LIPO_LIBS}" \
      --output="${LIPO_EXTRACT_A}"
  fi

  log "Running lipo gc-merge -> ${LIPO_GC_A}"
  python3 "${LIPO_SCRIPT}" gc-merge \
    --input="${LIPO_EXTRACT_A}" \
    --build-dir="${CROSS_BUILD_DIR}" \
    --ld="${LIPO_LD}" \
    --ldflags="${LIPO_LD_R_FLAGS}" \
    --output="${LIPO_GC_A}"

  log "Running lipo merge -> ${LIPO_OUTPUT}"
  python3 "${LIPO_SCRIPT}" merge \
    --input="${LIPO_GC_A}" \
    --build-dir="${CROSS_BUILD_DIR}" \
    --ld="${LIPO_LD}" \
    --ldflags="${LIPO_LD_R_FLAGS}" \
    --output="${LIPO_OUTPUT}"

  log "Lipo output: ${LIPO_OUTPUT}"
fi

log "Done"
log "Build dir: ${CROSS_BUILD_DIR}"
