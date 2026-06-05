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
#include <memory>
#include <string>
#ifdef EJIT_LIGHT_BACKEND
#include <map>
#endif

namespace llvm {
namespace ejit {

class EJitOrcEngine;
class EJitLogger;

#ifdef EJIT_LIGHT_BACKEND
struct SpecializationContext;
namespace light {
class CodeAllocator;
} // namespace light
#endif

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

  EJitCompileDriver(const Config &config,
                    EJitCache &cache,
                    PeriodArrayRegistry &periodReg,
                    EJitRuntimeState &runtimeState,
                    EJitModuleLoader &loader,
                    EJitLogger *logger = nullptr);

  ~EJitCompileDriver();

  /// Get or compile a function for the given specialization.
  /// Returns nullptr if compilation cannot proceed (e.g., time window
  /// not active, no bitcode found, or compile failure).
  void *getOrCompile(const std::string &funcName,
                     const std::pair<std::string, uint8_t> *dims,
                     unsigned count);

  EJitCache &getCache() { return cache_; }
  EJitRuntimeState &getRuntimeState() { return runtimeState_; }

#ifndef EJIT_LIGHT_BACKEND_ONLY
  void setSyncEngine(std::unique_ptr<EJitOrcEngine> engine);
#endif

  /// Register a user-defined symbol for JIT resolution (bare-metal).
  void registerSymbol(const std::string &name, void *addr);

private:
  const Config &config_;
  EJitCache &cache_;
  PeriodArrayRegistry &periodReg_;
  EJitRuntimeState &runtimeState_;
  EJitModuleLoader &loader_;
#ifndef EJIT_FREESTANDING
  EJitLogger *logger_;
#endif

#ifndef EJIT_LIGHT_BACKEND_ONLY
  std::unique_ptr<EJitOrcEngine> syncEngine_;
#endif
  // Async compiler will be added in EJitAsyncCompiler phase

#ifdef EJIT_LIGHT_BACKEND
  /// Lazily-created executable-memory allocator for the light backend.
  /// Owns all code emitted by the light path; lives until the driver dies.
  std::unique_ptr<light::CodeAllocator> lightAlloc_;

  /// User symbols registered via registerSymbol(). The light path resolves
  /// global addresses locally (no ORC), so it keeps its own copy in addition
  /// to forwarding to the ORC engine.
  std::map<std::string, void *> lightUserSymbols_;

  /// Light backend path used by getOrCompile() when backendMode is Light or
  /// Auto. Returns the function pointer, or nullptr if compilation did not
  /// succeed (the bool out-param reports whether the failure was "unsupported"
  /// so Auto mode can fall back to ORC). Fully self-contained: it does NOT
  /// depend on the ORC engine being available.
  void *tryCompileLight(const std::string &funcName, uint32_t cacheKey,
                        const std::string &bitcode,
                        const SpecializationContext &ctx,
                        bool &unsupported);
#endif
};

} // namespace ejit
} // namespace llvm

#endif
