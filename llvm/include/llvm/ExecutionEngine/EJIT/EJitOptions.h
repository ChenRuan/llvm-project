//===-- EJitOptions.h - EmbeddedJIT Configuration -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITOPTIONS_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITOPTIONS_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace llvm {
namespace ejit {

enum class CompileMode { Sync, Async };
enum class OptimizationLevel { L1 = 1, L2 = 2, L3 = 3 };

/// Execution backend selection.
///   Orc   - default path: SPEC4/PASS7 specialization -> ORC/JITLink/LLJIT.
///   Light - optional AArch64 light backend: specialization -> light emitter
///           -> CodeAllocator -> function pointer (no ORC/JITLink/EPC).
///   Auto  - try Light first; on unsupported IR transparently fall back to Orc.
/// The Light backend is only available when LLVMEJIT was built with the
/// EJIT_ENABLE_LIGHT_BACKEND CMake option (macro EJIT_LIGHT_BACKEND). When the
/// macro is not defined, Light/Auto behave exactly like Orc.
enum class BackendMode { Orc, Light, Auto };

struct Config {
  CompileMode compileMode = CompileMode::Sync;
  /// Execution backend. Default keeps the proven ORC path as primary.
  BackendMode backendMode = BackendMode::Orc;
  OptimizationLevel optLevel = OptimizationLevel::L2;
  size_t maxCodeMemory = 384 * 1024;
  size_t maxDataMemory = 128 * 1024;
  size_t maxCacheEntries = 256;
  size_t maxCacheSize = 32 * 1024 * 1024;
  size_t maxSingleFuncSize = 512 * 1024;
  bool enableLogger = true;
  /// If true, skip the constructor-based registration path and use the
  /// static registry table (__ejit_registry_*[]).  For bare-metal where
  /// global constructors are unavailable, or for testing.
  bool forceStaticRegistry = false;
  /// If non-empty, dump JIT-optimized LLVM IR (.ll) to this directory.
  /// One file per specialization, named <funcName>_<cacheKey>.ll.
  std::string dumpJITDir;
};

} // namespace ejit
} // namespace llvm

#endif
