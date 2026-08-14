//===-- EJitProfileMerge.cpp - in-memory PGO profile synthesis ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitProfileMerge.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/ProfileData/InstrProfWriter.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include <algorithm>
#include <cstdint>
#include <vector>

using namespace llvm;
using namespace llvm::ejit;

namespace {
// __llvm_profile_data field offsets (same LLVM build -> identical layout),
// mirroring InstrProfData.inc - see EJIT_ONLINE_PGO.md §5.3 for the counter
// path; NumValueSites[] (uint16 x [IPVK_Last+1]) follows NumCounters.
constexpr uintptr_t kFuncHashOff = 8;
constexpr uintptr_t kNameRefOff = 0;
constexpr uintptr_t kNumCountersOff = 48;
constexpr uintptr_t kNumValueSitesOff = 52;
// Sanity cap: a single function's edge counter array is never huge; a bogus
// offset read (layout drift) would typically yield a wild NumCounters.
constexpr uint32_t kMaxCountersPerFunc = 1u << 20;
// Bounds for a sane value-site inventory: far above any real function, far
// below the collector shard capacity multiplied to overflow.
constexpr uint32_t kMaxValueSitesPerKind = 1u << 16;

uint64_t loadU64(const void *p) {
  return __atomic_load_n(reinterpret_cast<const uint64_t *>(p),
                         __ATOMIC_RELAXED);
}
uint32_t loadU32(const void *p) {
  return __atomic_load_n(reinterpret_cast<const uint32_t *>(p),
                         __ATOMIC_RELAXED);
}
uint16_t loadU16(const void *p) {
  return __atomic_load_n(reinterpret_cast<const uint16_t *>(p),
                         __ATOMIC_RELAXED);
}
} // namespace

std::string ejit::synthesizeProfileBuffer(ArrayRef<PgoCounterRef> counters,
                                          ArrayRef<PgoValueSite> valueSites) {
  InstrProfWriter Writer;
  // P0 finding: manual addRecord does not propagate the IR-level flag
  // (llvm-profdata merge normally reads it from the raw profile header). Set
  // it explicitly so PGOInstrumentationUse accepts this as an IR profile
  // (else "Not an IR level instrumentation profile").
  if (auto E = Writer.mergeProfileKind(InstrProfKind::IRInstrumentation))
    consumeError(std::move(E));

  // Index the aggregated value sites by (FuncHash, kind) so the record loop
  // below emits them in strict site-index order (addValueData requires sites
  // to be added 0, 1, 2, ...).
  DenseMap<uint64_t, SmallVector<const PgoValueSite *, 8>> siteIndex;
  for (const PgoValueSite &S : valueSites)
    siteIndex[S.pgoNameHash].push_back(&S);

  // Runtime layout of __llvm_profile_data, mirroring InstrProfData.inc (same
  // LLVM build -> identical layout). Field offsets on 64-bit:
  //   0  NameRef      (uint64_t)
  //   8  FuncHash     (uint64_t)
  //  16  CounterPtr   (uintptr_t) -- NOT used: EJIT resolves the counter array
  //                                  address via ORC lookup (profcAddr), which
  //                                  is absolute; the struct's CounterPtr may
  //                                  be relative/biased.
  //  24  BitmapPtr    32 FunctionPointer    40 Values
  //  48  NumCounters  (uint32_t)  52 NumValueSites[3] (uint16 each)
  // EJIT_ONLINE_PGO.md §5.3.
  static_assert(sizeof(uintptr_t) == 8,
                "EJIT PGO runtime profile-data offsets assume 64-bit");

  for (const PgoCounterRef &C : counters) {
    if (!C.profdAddr || !C.profcAddr || !C.pgoName)
      continue;
    const auto *Data = reinterpret_cast<const uint8_t *>(C.profdAddr);
    uint64_t FuncHash = loadU64(Data + kFuncHashOff);
    uint64_t NameRef = loadU64(Data + kNameRefOff);
    uint32_t NumCounters = loadU32(Data + kNumCountersOff);
    if (NumCounters == 0 || NumCounters > kMaxCountersPerFunc)
      continue;
    const auto *CounterArray = reinterpret_cast<const uint64_t *>(C.profcAddr);
    // The __profc_* counters are being updated concurrently by shared Tier-1
    // machine code with `atomicrmw add` (§5). Read each counter with a RELAXED
    // atomic load so this synthesis never tears a value another core is mid-way
    // updating (a plain copy is a data race). Typed uint64_t scalar loads keep
    // this endian-safe on aarch64_be (no byte-wise counter parsing).
    std::vector<uint64_t> Counts;
    Counts.reserve(NumCounters);
    for (uint32_t i = 0; i < NumCounters; ++i)
      Counts.push_back(loadU64(&CounterArray[i]));
    NamedInstrProfRecord Rec(C.pgoName, FuncHash, std::move(Counts));

    // Value profile: emit each official kind's sites in strict index order.
    // NumValueSites comes from the SAME __profd_ struct the Tier-1 lowering
    // wrote, so the profile's site count always matches what Tier-2's
    // ValueProfileCollector will enumerate (else PGOUse skips the kind with
    // an "Inconsistent number of value sites" warning).
    const uint32_t nsIC = loadU16(Data + kNumValueSitesOff + 0);
    const uint32_t nsMem = loadU16(Data + kNumValueSitesOff + 2);
    auto it = siteIndex.find(NameRef);
    const SmallVector<const PgoValueSite *, 8> *sites =
        it == siteIndex.end() ? nullptr : &it->second;
    for (uint32_t kind : {static_cast<uint32_t>(IPVK_IndirectCallTarget),
                          static_cast<uint32_t>(IPVK_MemOPSize)}) {
      const uint32_t ns =
          kind == static_cast<uint32_t>(IPVK_IndirectCallTarget) ? nsIC : nsMem;
      if (ns == 0 || ns > kMaxValueSitesPerKind)
        continue;
      Rec.reserveSites(kind, ns);
      for (uint32_t s = 0; s < ns; ++s) {
        SmallVector<InstrProfValueData, 8> vd;
        if (sites) {
          for (const PgoValueSite *S : *sites)
            if (S->valueKind == kind && S->siteIndex == s) {
              vd.append(S->values.begin(), S->values.end());
              break;
            }
        }
        Rec.addValueData(kind, s, vd, nullptr);
      }
    }

    Writer.addRecord(std::move(Rec), 1, [](Error) {});
  }

  auto Buf = Writer.writeBuffer();
  if (!Buf)
    return {};
  return std::string(Buf->getBuffer());
}

#ifdef EJIT_SRE_PGO_VALUE_PROFILE

bool ejit::readValueSiteInventory(ArrayRef<PgoCounterRef> counters,
                                  SmallVectorImpl<PgoValueFunction> &funcs) {
  for (const PgoCounterRef &C : counters) {
    if (!C.profdAddr)
      continue;
    const auto *Data = reinterpret_cast<const uint8_t *>(C.profdAddr);
    const uint32_t nsIC = loadU16(Data + kNumValueSitesOff + 0);
    const uint32_t nsMem = loadU16(Data + kNumValueSitesOff + 2);
    if (nsIC > kMaxValueSitesPerKind || nsMem > kMaxValueSitesPerKind)
      return false;
    PgoValueFunction F;
    F.funcHash = loadU64(Data + kFuncHashOff); // CFG hash
    F.pgoNameHash = loadU64(Data + 0);         // NameRef: IR-PGO-name hash
    F.numIcSites = nsIC;
    F.numMemSites = nsMem;
    funcs.push_back(F);
  }
  return true;
}

bool ejit::aggregateValueSamples(ArrayRef<EJitVpSiteSample> samples,
                                 ArrayRef<PgoValueFunction> funcs,
                                 ArrayRef<PgoValueTarget> targets,
                                 SmallVectorImpl<PgoValueSite> &icMemSites,
                                 SmallVectorImpl<PgoScalarSite> &scalarSites) {
  // Verified target table: runtime address -> IR-PGO-name MD5. A recorded
  // indirect-call value that is not a verified function address is DROPPED -
  // never written raw into the profile as if it were a hash
  // (EJIT_VALUE_PROFILE.md §5.3).
  DenseMap<uintptr_t, uint64_t> targetByAddr;
  for (const PgoValueTarget &T : targets)
    targetByAddr[T.addr] = T.md5Hash;

  // Accumulate snapshot samples per site key: value -> count, plus the
  // window total. The same site observed by several cores sums here.
  struct SiteAccum {
    uint64_t total = 0;
    DenseMap<uint64_t, uint64_t> counts;
  };
  DenseMap<uint64_t, SiteAccum> byKey;
  for (const EJitVpSiteSample &S : samples) {
    if (S.siteKey == 0)
      continue;
    SiteAccum &A = byKey[S.siteKey];
    for (uint32_t i = 0; i < kEJitVpK; ++i) {
      if (S.counts[i] == 0)
        continue;
      A.counts[S.values[i]] += S.counts[i];
    }
    A.total += S.total;
  }

  for (const PgoValueFunction &F : funcs) {
    if (F.numIcSites > kMaxValueSitesPerKind ||
        F.numMemSites > kMaxValueSitesPerKind ||
        F.numScalarSites > kMaxValueSitesPerKind)
      return false;

    // Official kinds: indirect-call targets + memop sizes.
    for (uint32_t kind : {static_cast<uint32_t>(kEJitVpIndirectCall),
                          static_cast<uint32_t>(kEJitVpMemOpSize)}) {
      const uint32_t ns =
          kind == kEJitVpIndirectCall ? F.numIcSites : F.numMemSites;
      for (uint32_t s = 0; s < ns; ++s) {
        auto it = byKey.find(ejitVpSiteKey(F.pgoNameHash, kind, s));
        if (it == byKey.end() || it->second.counts.empty())
          continue;
        PgoValueSite Site;
        Site.pgoNameHash = F.pgoNameHash;
        Site.valueKind = kind == kEJitVpIndirectCall ? IPVK_IndirectCallTarget
                                                     : IPVK_MemOPSize;
        Site.siteIndex = s;
        for (const auto &KV : it->second.counts) {
          uint64_t v = KV.first;
          if (kind == kEJitVpIndirectCall) {
            auto T = targetByAddr.find(static_cast<uintptr_t>(v));
            if (T == targetByAddr.end())
              continue; // unverified address: drop
            v = T->second;
          }
          Site.values.push_back({v, KV.second});
        }
        if (Site.values.empty())
          continue;
        llvm::sort(Site.values, [](const InstrProfValueData &A,
                                   const InstrProfValueData &B) {
          return A.Count > B.Count;
        });
        // Top-N bound: the official profile supports up to
        // INSTR_PROF_NUM_BUCKETS (16) per site; keep the best 8 per site.
        constexpr size_t kTopN = 8;
        if (Site.values.size() > kTopN)
          Site.values.resize(kTopN);
        icMemSites.push_back(std::move(Site));
      }
    }

    // Scalar/loop-bound sites: top-1 + total for the Tier-2 side table. The
    // dominance thresholds (min samples / min confidence) are the caller's
    // policy; here we only report the measured distribution. Keyed by the
    // IR-PGO-name hash (EJIT_VALUE_PROFILE.md §5.2).
    for (uint32_t s = 0; s < F.numScalarSites; ++s) {
      auto it = byKey.find(ejitVpSiteKey(F.pgoNameHash, kEJitVpScalar, s));
      if (it == byKey.end() || it->second.counts.empty())
        continue;
      uint64_t topV = 0, topC = 0;
      for (const auto &KV : it->second.counts)
        if (KV.second > topC) {
          topC = KV.second;
          topV = KV.first;
        }
      PgoScalarSite Sc;
      Sc.funcHash = F.pgoNameHash; // keyed by the IR-PGO-name hash (§5.2)
      Sc.siteIndex = s;
      Sc.topValue = topV;
      Sc.topCount = topC;
      Sc.total = it->second.total;
      scalarSites.push_back(Sc);
    }
  }
  return true;
}

#endif // EJIT_SRE_PGO_VALUE_PROFILE
