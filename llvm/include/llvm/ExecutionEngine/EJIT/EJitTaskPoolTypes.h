//===-- EJitTaskPoolTypes.h - Shared taskpool protocol types -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITTASKPOOLTYPES_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITTASKPOOLTYPES_H

#include "llvm/ExecutionEngine/EJIT/EJitBoundPtr.h"
#include <cstdint>

namespace llvm {
namespace ejit {

//===----------------------------------------------------------------------===//
// EJitCompileRequest
//
// Fixed-layout compile request. Only uint32_t / uint64_t / uintptr_t fields,
// no bitfields, no constructors/destructors, no STL — so it is trivially
// copyable through a platform queue and has identical field semantics on
// little- and big-endian targets (fields are accessed by type, never parsed
// byte-by-byte). It must NOT be serialized across hosts of differing
// pointer width.
//===----------------------------------------------------------------------===//
struct EJitDimPair {
  uint32_t dimType;
  uint32_t instanceId;
};

constexpr uint32_t kEJitMaxRequestDims = 4u;

struct EJitCompileRequest {
  uint32_t funcIndex;
  uint32_t numDims;
  EJitDimPair dims[kEJitMaxRequestDims];
  uint32_t versions[kEJitMaxRequestDims];
  // Nonzero only for the experimental version-reuse request lifecycle. This
  // token identifies one logical attempt across its Tier-1 queue item, the
  // later Tier-2 queue item, delayed publication, cancellation and callbacks.
  // It is never reused, so an old callback cannot settle a newer attempt for
  // the same function/generation.
  uint64_t attemptToken;
  uintptr_t fallbackPtr;
  // Shared-taskpool owner generation captured at enqueue time. A worker drops a
  // request whose generation no longer equals the shared state's generation
  // (owner re-init), so a stale request can never pollute a new generation's
  // cache. Endianness: a plain
  // fixed-width scalar accessed by value, never byte-parsed.
  uint32_t generation;
  // Borrowed bound objects. The queue copies only these descriptors; it never
  // owns, frees, or dereferences the pointed-to bytes. The default protocol
  // ends the borrow when the compile callback returns. The opt-in request-
  // attempt protocol can retain the descriptors across Tier-1/Tier-2, and
  // explicitly acknowledges the final compiler read before releasing them.
  uint32_t boundCount;
  EJitBoundPtrDescriptor boundPointers[kEJitMaxBoundPointers];
  // Frozen observed Tier-1 dispatch metadata (v21, experimental sharing
  // contract). Captured from the published Tier-1 slot under the bucket lock
  // when this Tier-2 request is built, and re-captured identically from the
  // same frozen slot on a queue-full retry. All zero when the observation is
  // unavailable (legacy/tokenless mode, non-shared pool, or a request that was
  // not armed by a Tier-1 slot). t1QuotaEnd is the timestamp of the final
  // allowed dispatch; 0 means unknown, never the compile time. These fields do
  // not affect request routing.
  uint64_t t1DispatchCount;
  uint64_t t1QuotaEnd;
  uint64_t t1DispatchLimit;
};

// Size is stable per pointer width and independent of pointee size. Some
// 32-bit ABIs align uint64_t to 8 bytes and therefore add tail padding.
// v21 added the three 64-bit observed-dispatch fields (208 -> 232 on 64-bit).
static_assert(
    sizeof(EJitCompileRequest) ==
        (sizeof(uintptr_t) == 8 ? 232u
                                : (alignof(uint64_t) == 8 ? 200u : 196u)),
    "EJitCompileRequest size must stay fixed and payload-independent");
static_assert(alignof(EJitCompileRequest) <= 8,
              "EJitCompileRequest alignment must stay <= 8 bytes");
static_assert(sizeof(uintptr_t) == 4 || sizeof(uintptr_t) == 8,
              "EJitCompileRequest assumes a 32- or 64-bit pointer width");

//===----------------------------------------------------------------------===//
// Compile-tier encoding for the queue (EJIT_ONLINE_PGO.md §6.3).
//
// CompileTier (EJitOrcEngine.h) is {Baseline=0, Instrumented=1, PGOUse=2}. It
// is transported through the queue in the TOP 2 BITS of funcIndex, which is
// <2^30 in practice. This needs NO layout change and NO queue-ABI bump.
//
// CRITICAL: cacheKey is built from the STRIPPED funcIndex (stripReqTier), so
// Tier-1 and Tier-2 of the same (funcIndex, dims) share one cacheKey - that is
// how Tier-2 publish overwrites the Tier-1 slot (§2/§7.1). The tier is decoded
// separately and carried on SpecializationContext, never in cacheKey.
//===----------------------------------------------------------------------===//
constexpr uint32_t kEJitTierShift = 30;
constexpr uint32_t kEJitTierMask = 0x3u << kEJitTierShift;

/// CompileTier values (mirror CompileTier in EJitOrcEngine.h) for code that
/// must not include EJitOrcEngine.h (e.g. the taskpool).
constexpr uint32_t kEJitTierBaseline = 0;
constexpr uint32_t kEJitTierInstrumented = 1;
constexpr uint32_t kEJitTierPgoUse = 2;

/// Encode a tier (CompileTier value) into funcIndex's top 2 bits.
inline uint32_t encodeReqTier(uint32_t funcIndex, uint32_t tier) {
  return (funcIndex & ~kEJitTierMask) | ((tier & 0x3u) << kEJitTierShift);
}
/// Decode the tier carried in funcIndex's top 2 bits.
inline uint32_t decodeReqTier(uint32_t funcIndex) {
  return (funcIndex >> kEJitTierShift) & 0x3u;
}
/// Strip the tier bits to recover the real funcIndex (for cacheKey).
inline uint32_t stripReqTier(uint32_t funcIndex) {
  return funcIndex & ~kEJitTierMask;
}

enum class EJitCompileMode : uint32_t {
  Off = 0,
  Async = 1,
};

enum class EJitCompileOrGetStatus : uint32_t {
  CacheHit = 0,
  OffMode,
  InstanceDisabled,
  EnqueuedPending,
  AlreadyPending,
  PgoAdmissionDeferred,
  QueueFullFallback,
  CompileFailed,
  InvalidParam,
};

enum class EJitDedupResult : uint32_t {
  Claimed = 0,      ///< Caller now owns the in-flight bit; must clear() later.
  AlreadyPending,   ///< Same funcIndex already in flight.
  InvalidFuncIndex, ///< funcIndex >= kCapacity: rejected, not folded.
};

enum class EJitPublishStatus : uint32_t {
  Published = 0,   ///< Entry written/overwritten with the request's versions.
  VersionMismatch, ///< A version changed before/at publish: nothing written.
  InvalidParam,    ///< Null fn or numDims > 4.
  Failed,          ///< No slot available.
};

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITTASKPOOLTYPES_H
