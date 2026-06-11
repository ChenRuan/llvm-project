//===-- EJitCompileDriver.h - Compilation Scheduler -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITCOMPILEDRIVER_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITCOMPILEDRIVER_H

#include "llvm/ExecutionEngine/EJIT/EJitCache.h"
#include "llvm/ExecutionEngine/EJIT/EJitModuleLoader.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptions.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#ifndef EJIT_FREESTANDING
#include <atomic>
#endif
#include <memory>
#include <string>

namespace llvm {
namespace ejit {

class EJitOrcEngine;
class EJitLogger;

/// Unified entry point for sync and async compilation. Handles cache
/// lookup, time-window state verification, bitcode retrieval, and
/// compilation dispatch.
class EJitCompileDriver {
public:
  struct Result {
    void *funcPtr = nullptr;
    size_t compileTimeMs = 0;
    size_t codeSize = 0;
  };

  /// Observability counters for the compile path. Plain snapshot type returned
  /// by getCounters(). Lets callers and tests confirm the fast path (cache hit)
  /// does not re-enter the cold compile path, and which early-return reason
  /// fired on a miss. The following invariants hold:
  ///   calls          == cacheHits + coldMisses
  ///   coldMisses     == missingFunc + missingBitcode + windowInactive
  ///                     + engineMissing + compileAttempts
  ///   compileAttempts == compiled + compileFailed
  struct Counters {
    uint64_t calls = 0;           ///< total getOrCompile() calls
    uint64_t cacheHits = 0;       ///< fast path: returned a cached pointer
    uint64_t coldMisses = 0;      ///< entered the cold (decode/compile) path
    uint64_t missingFunc = 0;     ///< unknown funcIdx (no name registered)
    uint64_t missingBitcode = 0;  ///< name known but bitcode missing
    uint64_t windowInactive = 0;  ///< a required time window was not active
    uint64_t engineMissing = 0;   ///< sync engine not initialized
    uint64_t compileAttempts = 0; ///< reached the JIT load/lookup stage
    uint64_t compileFailed = 0;   ///< load or lookup failed
    uint64_t compiled = 0;        ///< produced and cached a function pointer
  };

  EJitCompileDriver(const Config &config,
                    EJitCache &cache,
                    EJitRuntimeState &runtimeState,
                    EJitModuleLoader &loader,
                    EJitLogger *logger = nullptr);

  ~EJitCompileDriver();

  /// Hot path: cache lookup on pre-computed uint64_t cacheKey.
  /// Cold path: decode cacheKey → load bitcode → JIT compile.
  /// Returns nullptr on miss that cannot be compiled (time window not
  /// active, no bitcode, or compile failure).
  void *getOrCompile(uint64_t cacheKey);

  EJitCache &getCache() { return cache_; }
  EJitRuntimeState &getRuntimeState() { return runtimeState_; }
  EJitModuleLoader &getLoader() { return loader_; }
  const Config &getConfig() { return config_; }
  EJitOrcEngine *getSyncEngine() { return syncEngine_.get(); }
#ifndef EJIT_FREESTANDING
  EJitLogger *getLogger() { return logger_; }
#else
  EJitLogger *getLogger() { return nullptr; }
#endif

  void setSyncEngine(std::unique_ptr<EJitOrcEngine> engine);
  void registerSymbol(const std::string &name, void *addr);

  /// Snapshot of the observability counters (thread-safe read).
  Counters getCounters() const;

private:
  const Config &config_;
  EJitCache &cache_;
  EJitRuntimeState &runtimeState_;
  EJitModuleLoader &loader_;
#ifndef EJIT_FREESTANDING
  EJitLogger *logger_;
#endif

  std::unique_ptr<EJitOrcEngine> syncEngine_;
  // Async compiler will be added in EJitAsyncCompiler phase

  // Observability counters. Host builds use relaxed atomics so a future async
  // worker can update them without a data race. Freestanding builds stay
  // single-threaded and avoid pulling in the atomic runtime for diagnostics-only
  // state.
  struct InternalCounters {
#ifndef EJIT_FREESTANDING
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> cacheHits{0};
    std::atomic<uint64_t> coldMisses{0};
    std::atomic<uint64_t> missingFunc{0};
    std::atomic<uint64_t> missingBitcode{0};
    std::atomic<uint64_t> windowInactive{0};
    std::atomic<uint64_t> engineMissing{0};
    std::atomic<uint64_t> compileAttempts{0};
    std::atomic<uint64_t> compileFailed{0};
    std::atomic<uint64_t> compiled{0};
#else
    uint64_t calls = 0;
    uint64_t cacheHits = 0;
    uint64_t coldMisses = 0;
    uint64_t missingFunc = 0;
    uint64_t missingBitcode = 0;
    uint64_t windowInactive = 0;
    uint64_t engineMissing = 0;
    uint64_t compileAttempts = 0;
    uint64_t compileFailed = 0;
    uint64_t compiled = 0;
#endif
  } counters_;
};

} // namespace ejit
} // namespace llvm

#endif
