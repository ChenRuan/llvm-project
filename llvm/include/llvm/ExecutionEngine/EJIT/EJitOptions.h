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

/// Off  — no JIT compilation; the wrapper falls through to AOT on every call.
/// Sync — compile inline on the calling thread (blocking, for deterministic
///         environments or when background threads are not available).
/// Async — enqueue to a background worker; the first call falls through to
///         AOT and subsequent calls hit the cache.
enum class CompileMode { Off, Sync, Async };
enum class OptimizationLevel { L1 = 1, L2 = 2, L3 = 3 };

struct Config {
  CompileMode compileMode = CompileMode::Async;
  OptimizationLevel optLevel = OptimizationLevel::L2;
  size_t maxCodeMemory = 2 * 1024 * 1024;
  size_t maxDataMemory = 128 * 1024;
  size_t maxCacheEntries = 4096;
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
  /// Online PGO opt-in (EJIT_ONLINE_PGO.md). Off => the JIT pipeline is
  /// unchanged (Baseline only, no instrumentation, no Tier-2). On => Tier-1
  /// instrumentation + lazy Tier-2 PGOUse recompile. The footprint cost
  /// (~640 KB stripped runtime, P0-1) is incurred whenever the PGO component
  /// libs are linked, regardless of this flag; this flag only gates behavior.
  bool enablePgo = false;
#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
  /// Build-option-gated runtime sampling. This reuses the temporary
  /// instrumented tier but does not require profile-guided optimization.
  bool enableProfileAudit = true;
#else
  bool enableProfileAudit = false;
#endif
  /// PR230 shared-version code reuse (EJIT_VERSION_CODE_REUSE_SPEC.md §8.3),
  /// default OFF and opt-in only.
  ///
  /// When ON the JIT keeps the real cell/TRP call arguments in the IR and
  /// builds a read-only evaluation environment that is consulted only to fold
  /// authorized `!ejit.may_const` loads (§3.2). The per-(entry, cell, trp,
  /// versions) logical cache and lifecycle are unchanged; the wrapper and the
  /// cell/TRP argument ABI are unchanged.
  ///
  /// V1 supports Async + normal online PGO only. Use
  /// checkSharedSpecializationSupport() to reject the unsupported combinations
  /// before any queue/waiter/admission side effect exists.
  bool enableSharedSpecialization = false;
};

/// Result of validating a Config against the PR230 V1 support matrix (§8.3).
/// `supported` is false when shared specialization was requested in a
/// combination this version cannot run; `reason` then carries a stable,
/// human-readable explanation for the diagnostic log.
struct SharedSpecializationSupport {
  bool supported = true;
  const char *reason = "";
};

/// Validate \p C against the V1 shared-specialization support matrix:
///
///   OFF                        -> supported (nothing to check)
///   ON + Async + normal PGO    -> supported
///   ON + Sync                  -> rejected (no T1 progress source)
///   ON + PGO off               -> rejected (no group profile)
///   ON + audit-only PGO        -> rejected (diagnostic sampling is not a
///                                 normal PGO group lifecycle)
///
/// A rejected combination must fail initialization/compilation explicitly
/// rather than silently disabling PGO or creating a group that can never make
/// progress. Changing the policy requires a safe shutdown/drain and a fresh
/// initialization.
inline SharedSpecializationSupport
checkSharedSpecializationSupport(const Config &C) {
  if (!C.enableSharedSpecialization)
    return {true, ""};
  if (C.compileMode != CompileMode::Async)
    return {false, "shared specialization requires Async compile mode"};
  if (!C.enablePgo) {
    if (C.enableProfileAudit)
      return {false, "shared specialization rejects audit-only sampling "
                     "(enablePgo is required)"};
    return {false, "shared specialization requires online PGO (enablePgo)"};
  }
  return {true, ""};
}

} // namespace ejit
} // namespace llvm

#endif
