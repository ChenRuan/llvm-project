//===-- EJitDiag.h - EmbeddedJIT Diagnostic Logging ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Diagnostic logging for the EmbeddedJIT runtime, used for bring-up,
// field debugging, and production monitoring.  All output goes through
// the platform-provided SRE_printf() so it integrates with existing
// device log infrastructure.
//
// Compile with -DEJIT_DIAG_ENABLE to activate logging.  When not defined,
// all macros expand to nothing and incur zero runtime cost.
// Define EJIT_SRE_DIAG together with EJIT_DIAG_ENABLE to route diagnostics
// through the platform-provided SRE_printf(); otherwise diagnostics use
// std::printf.
//
// Runtime log levels (gEJitDiagLevel, default INFO):
//   OFF(0)    — no output
//   INFO(1)   — key events: init/shutdown, compile begin/OK/FAIL, cache
//               HIT/MISS, activation, errors, registration consume summary
//   VERBOSE(2)— per-item detail: each first-time registration, per-function
//               struct-field stats, per-call compile_or_get, taskpool requests
//   DEBUG(3)  — internals: idempotent registration skips, per-load replace
//               failures, staging internals, funcMeta caching
//
// The level mirrors enum ejit_log_level in EJitRuntime.h; raise it at runtime
// via ejit_set_log_level() to recover full detail without recompiling.  All
// EJIT_DIAG* call sites are on cold paths (registration, compile, diagnostics),
// so the single integer compare per call is negligible even on bare-metal.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITDIAG_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITDIAG_H

// Runtime log-level thresholds. Mirrored by enum ejit_log_level in
// EJitRuntime.h (the public C ABI contract) — keep the two in sync.
#define EJIT_LOG_LVL_OFF 0
#define EJIT_LOG_LVL_INFO 1
#define EJIT_LOG_LVL_VERBOSE 2
#define EJIT_LOG_LVL_DEBUG 3

// Process-wide runtime log level. Defined in EJitLogger.cpp (always compiled,
// hosted and freestanding). Default INFO. Set via ejit_set_log_level().
extern int gEJitDiagLevel;

#ifdef EJIT_DIAG_ENABLE

#ifdef EJIT_SRE_DIAG
// Provided by the platform.  Must be declared exactly as written so the
// linker can resolve it against the device firmware's libc / BSP.
extern "C" int SRE_printf(const char *fmt, ...);

// Keep diagnostics in a single call so the prefix and payload stay on one line.
#define EJIT_DIAG(fmt, ...)                                                  \
  do {                                                                       \
    if (gEJitDiagLevel >= EJIT_LOG_LVL_INFO)                                 \
      SRE_printf("[EJIT] %s:%d " fmt "\n", __func__, __LINE__,               \
                 ##__VA_ARGS__);                                             \
  } while (0)
#define EJIT_DIAG_VERBOSE(fmt, ...)                                          \
  do {                                                                       \
    if (gEJitDiagLevel >= EJIT_LOG_LVL_VERBOSE)                              \
      SRE_printf("[EJIT] %s:%d " fmt "\n", __func__, __LINE__,               \
                 ##__VA_ARGS__);                                             \
  } while (0)
#define EJIT_DIAG_DEBUG(fmt, ...)                                            \
  do {                                                                       \
    if (gEJitDiagLevel >= EJIT_LOG_LVL_DEBUG)                                \
      SRE_printf("[EJIT] %s:%d " fmt "\n", __func__, __LINE__,               \
                 ##__VA_ARGS__);                                             \
  } while (0)
#else
#include <cstdio>

#define EJIT_DIAG(fmt, ...)                                                  \
  do {                                                                       \
    if (gEJitDiagLevel >= EJIT_LOG_LVL_INFO)                                 \
      std::printf("[EJIT] %s:%d " fmt "\n", __func__, __LINE__,              \
                  ##__VA_ARGS__);                                            \
  } while (0)
#define EJIT_DIAG_VERBOSE(fmt, ...)                                          \
  do {                                                                       \
    if (gEJitDiagLevel >= EJIT_LOG_LVL_VERBOSE)                              \
      std::printf("[EJIT] %s:%d " fmt "\n", __func__, __LINE__,              \
                  ##__VA_ARGS__);                                            \
  } while (0)
#define EJIT_DIAG_DEBUG(fmt, ...)                                            \
  do {                                                                       \
    if (gEJitDiagLevel >= EJIT_LOG_LVL_DEBUG)                                \
      std::printf("[EJIT] %s:%d " fmt "\n", __func__, __LINE__,              \
                  ##__VA_ARGS__);                                            \
  } while (0)
#endif

#else // !EJIT_DIAG_ENABLE

// Expand to ((void)0) regardless of argument count by matching everything.
#define EJIT_DIAG(...) ((void)0)
#define EJIT_DIAG_VERBOSE(...) ((void)0)
#define EJIT_DIAG_DEBUG(...) ((void)0)

#endif // EJIT_DIAG_ENABLE

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITDIAG_H
