//===-- EJitGlobalLayout.cpp - Owner-side complete final layout
//------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"
#include <limits>

using namespace llvm::ejit;

namespace {
bool sameIdentity(const EJitCompileRequest &A, const EJitCompileRequest &B,
                  bool bindings = true) {
  if (stripReqTier(A.funcIndex) != stripReqTier(B.funcIndex) ||
      A.numDims != B.numDims || A.generation != B.generation)
    return false;
  // Dimension order is the registered entry's formal/legacy key order.
  // This identifies a member; it does not determine the manifest's rank.
  for (uint32_t I = 0; I < A.numDims; ++I)
    if (A.dims[I].dimType != B.dims[I].dimType ||
        A.dims[I].instanceId != B.dims[I].instanceId ||
        A.versions[I] != B.versions[I])
      return false;
  if (!bindings)
    return true;
  if (A.boundCount != B.boundCount)
    return false;
  for (uint32_t I = 0; I < A.boundCount; ++I) {
    bool found = false;
    for (uint32_t J = 0; J < B.boundCount; ++J)
      if (A.boundPointers[I].argIndex == B.boundPointers[J].argIndex &&
          A.boundPointers[I].rawPtr == B.boundPointers[J].rawPtr &&
          A.boundPointers[I].size == B.boundPointers[J].size)
        found = true;
    if (!found)
      return false;
  }
  return true;
}

EJitCompileRequest slotRequest(const EJitSharedCacheSlot &S) {
  EJitCompileRequest R{};
  R.funcIndex = S.funcIndex;
  R.numDims = S.numDims;
  R.generation = S.generation;
  for (uint32_t I = 0; I < R.numDims; ++I) {
    R.dims[I] = S.dims[I];
    R.versions[I] = S.versions[I];
  }
  return R;
}
} // namespace

bool EJitSharedTaskPool::configureGlobalLayout(
    const std::vector<EJitCompileRequest> &manifest,
    const EJitLayoutBudgets &budgets, PrepareArtifactCallback prepare,
    LinkArtifactCallback link, DropArtifactCallback drop, void *ctx,
    SamplingPreparedCallback sampled) {
  if (!state_ || manifest.empty() || !prepare || !link || !drop ||
      layoutConfigured_ ||
      kEJitSharedCacheBuckets * kEJitSharedCacheSlots >= 0x8000u ||
      budgets.maxMembers == 0 || manifest.size() > budgets.maxMembers ||
      manifest.size() > kEJitSharedQueueSlots ||
      budgets.maxCollectionPolls == 0 || budgets.maxArtifactBytes == 0)
    return false;
  const auto Init =
      static_cast<EJitSharedInitState>(state_->initState.loadAcquire());
  if (Init != EJitSharedInitState::Initializing &&
      !(Init == EJitSharedInitState::Ready &&
        state_->workerTaskId.loadAcquire() == 0))
    return false;
  std::vector<LayoutMember> members;
  std::vector<uint32_t> occupancy(kEJitSharedCacheBuckets, 0);
  for (size_t I = 0; I < manifest.size(); ++I) {
    EJitCompileRequest R = manifest[I];
    R.generation = state_->generation.loadAcquire();
    const uint32_t tier = decodeReqTier(R.funcIndex);
    if (stripReqTier(R.funcIndex) >= kEJitSharedMaxFuncIndex ||
        R.numDims > kEJitSharedMaxDims ||
        !validateBoundPtrDescriptors(R.boundPointers, R.boundCount) ||
        tier != (state_->pgoEnabled.loadAcquire() ? kEJitTierPgoUse
                                                  : kEJitTierBaseline))
      return false;
    for (uint32_t D = 0; D < R.numDims; ++D) {
      if (R.dims[D].dimType >= kEJitSharedDimTypes ||
          R.dims[D].instanceId >= kEJitSharedInstances)
        return false;
      for (uint32_t E = 0; E < D; ++E)
        if (R.dims[D].dimType == R.dims[E].dimType)
          return false;
    }
    for (const auto &M : members) {
      // The legacy dispatch key cannot represent different bindings/versions
      // of the same entry+dims concurrently. Reject instead of aliasing them.
      EJitCompileRequest Existing = M.req, Candidate = R;
      for (uint32_t D = 0; D < Existing.numDims; ++D)
        Existing.versions[D] = 0;
      for (uint32_t D = 0; D < Candidate.numDims; ++D)
        Candidate.versions[D] = 0;
      if (sameIdentity(Existing, Candidate, false))
        return false;
    }
    uint32_t bucket =
        hashIdentity(stripReqTier(R.funcIndex), R.dims, R.numDims) %
        kEJitSharedCacheBuckets;
    if (++occupancy[bucket] > kEJitSharedCacheSlots)
      return false;
    LayoutMember M;
    M.req = R;
    members.push_back(M);
    EJIT_DIAG("global layout manifest rank=%zu func=%u dims=%u tier=%u", I,
              stripReqTier(R.funcIndex), R.numDims, tier);
    for (uint32_t D = 0; D < R.numDims; ++D)
      EJIT_DIAG("global layout manifest rank=%zu dim=%u type=%u instance=%u "
                "version=%u",
                I, D, R.dims[D].dimType, R.dims[D].instanceId, R.versions[D]);
    for (uint32_t B = 0; B < R.boundCount; ++B)
      EJIT_DIAG(
          "global layout manifest rank=%zu boundArg=%u address=%p bytes=%u", I,
          R.boundPointers[B].argIndex, R.boundPointers[B].rawPtr,
          R.boundPointers[B].size);
  }
  layoutMembers_ = std::move(members);
  layoutBudgets_ = budgets;
  layoutPrepare_ = prepare;
  layoutLink_ = link;
  layoutDrop_ = drop;
  layoutCtx_ = ctx;
  layoutSampled_ = sampled;
  layoutConfigured_ = true;
  layoutPolls_ = 0;
  layoutSamplingThreshold_ = state_->tier2Threshold.loadAcquire();
  layoutSamplingMax_ = state_->pgoMaxActiveFunctions.loadAcquire();
  state_->globalLayoutRequired.storeRelaxed(layoutMembers_.size());
  state_->globalLayoutState.storeRelease(
      static_cast<uint32_t>(EJitGlobalLayoutState::Collecting));
  return true;
}

size_t EJitSharedTaskPool::globalMemberFor(const EJitCompileRequest &R) const {
  for (size_t I = 0; I < layoutMembers_.size(); ++I)
    if (sameIdentity(layoutMembers_[I].req, R))
      return I;
  return layoutMembers_.size();
}

bool EJitSharedTaskPool::samplingDrained(const EJitCompileRequest &R) const {
  uint32_t bucket = hashIdentity(stripReqTier(R.funcIndex), R.dims, R.numDims) %
                    kEJitSharedCacheBuckets;
  for (const auto &S : state_->buckets[bucket].slots)
    if (S.state.loadAcquire() ==
            static_cast<uint32_t>(EJitSharedSlotState::Ready) &&
        S.tier.loadAcquire() == kEJitTierInstrumented &&
        sameIdentity(slotRequest(S), R, false))
      return S.hitCount.loadAcquire() >= state_->tier2Threshold.loadAcquire() &&
             S.samplingExecutions.loadAcquire() == 0 &&
             S.samplingReturns.loadAcquire() >=
                 state_->tier2Threshold.loadAcquire();
  return false;
}

void EJitSharedTaskPool::releaseSamplingAdmission(const EJitCompileRequest &R) {
  if (!samplingDrained(R))
    return;
  uint32_t expected = 0;
  while (!state_->pgoAdmissionLock.compareExchange(expected, 1))
    expected = 0;
  const uint32_t encoded = stripReqTier(R.funcIndex) + 1;
  for (uint32_t I = 0; I < kEJitSharedMaxConcurrentProfiles; ++I)
    if (state_->pgoActiveFunctions[I].loadRelaxed() == encoded) {
      state_->pgoActiveFunctions[I].storeRelease(0);
      state_->pgoProgressQuarters[I].storeRelaxed(0);
      state_->pgoActiveFunctionCount.fetchSub(1);
      EJIT_DIAG("global layout sampling drained func=%u; admission released "
                "before final allocation",
                encoded - 1);
      break;
    }
  state_->pgoAdmissionLock.storeRelease(0);
}

void EJitSharedTaskPool::clearGlobalArtifacts() {
  for (auto &M : layoutMembers_) {
    if (M.artifact && layoutDrop_)
      layoutDrop_(layoutCtx_, M.artifact);
    M.artifact = nullptr;
  }
  state_->globalLayoutArtifactBytes.storeRelaxed(0);
}

void EJitSharedTaskPool::invalidateMaterializedLayout(bool includeCollection) {
  uint32_t phase = state_->globalLayoutState.loadAcquire();
  while (phase >= static_cast<uint32_t>(includeCollection
                                            ? EJitGlobalLayoutState::Collecting
                                            : EJitGlobalLayoutState::Linking) &&
         phase <= static_cast<uint32_t>(EJitGlobalLayoutState::Complete)) {
    if (state_->globalLayoutState.compareExchange(
            phase, static_cast<uint32_t>(EJitGlobalLayoutState::Failed))) {
      state_->globalLayoutError.storeRelease(
          static_cast<uint32_t>(EJitGlobalLayoutError::StaleIdentity));
      return;
    }
  }
}

void EJitSharedTaskPool::failGlobalLayout(EJitGlobalLayoutError error,
                                          uint32_t rank) {
  if (globalLayoutState() == EJitGlobalLayoutState::Failed &&
      layoutFailureRecorded_)
    return;
  layoutFailureRecorded_ = true;
  state_->globalLayoutError.storeRelaxed(static_cast<uint32_t>(error));
  state_->globalLayoutErrorRank.storeRelaxed(rank);
  state_->globalLayoutState.storeRelease(
      static_cast<uint32_t>(EJitGlobalLayoutState::Failed));
  EJIT_DIAG("global layout FAILED gen=%u rank=%u reason=%u ready=%u/%u "
            "published=%u artifactBytes=%llu; legal fallback retained",
            state_->generation.loadAcquire(), rank,
            static_cast<unsigned>(error),
            state_->globalLayoutReady.loadRelaxed(),
            state_->globalLayoutRequired.loadRelaxed(),
            state_->globalLayoutPublished.loadRelaxed(),
            static_cast<unsigned long long>(
                state_->globalLayoutArtifactBytes.loadRelaxed()));
  for (size_t I = 0; I < layoutMembers_.size(); ++I)
    EJIT_DIAG("global layout failure member rank=%zu published=%u body=%p", I,
              static_cast<unsigned>(layoutMembers_[I].published),
              layoutMembers_[I].fn);
  // Objects are not executable. Linked/published NO_RECLAIM bytes remain
  // charged to their pools and are NEVER freed or overwritten here.
  clearGlobalArtifacts();
  for (auto &M : layoutMembers_) {
    cacheDropPending(M.req, nullptr);
    dedupClear(M.req.funcIndex, M.req.generation);
  }
}

bool EJitSharedTaskPool::stageGlobalArtifact(const EJitCompileRequest &R) {
  size_t rank = globalMemberFor(R);
  if (rank == layoutMembers_.size()) {
    failGlobalLayout(EJitGlobalLayoutError::Manifest, rank);
    dedupClear(R.funcIndex, R.generation);
    return false;
  }
  auto &M = layoutMembers_[rank];
  if (M.ready) {
    dedupClear(R.funcIndex, R.generation);
    return true;
  }
  if (R.generation != state_->generation.loadAcquire() || !versionsCurrent(R)) {
    failGlobalLayout(EJitGlobalLayoutError::StaleIdentity, rank);
    return false;
  }
  if (decodeReqTier(R.funcIndex) == kEJitTierPgoUse && !samplingDrained(R))
    return false;
  void *artifact = nullptr;
  uint64_t bytes = 0;
  if (!layoutPrepare_(layoutCtx_, R, &artifact, &bytes) || !artifact) {
    if (artifact)
      layoutDrop_(layoutCtx_, artifact);
    failGlobalLayout(EJitGlobalLayoutError::Preparation, rank);
    return false;
  }
  uint64_t used = state_->globalLayoutArtifactBytes.loadRelaxed();
  if (R.generation != state_->generation.loadAcquire() || !versionsCurrent(R) ||
      bytes > layoutBudgets_.maxArtifactBytes - used) {
    layoutDrop_(layoutCtx_, artifact);
    failGlobalLayout(bytes > layoutBudgets_.maxArtifactBytes - used
                         ? EJitGlobalLayoutError::ArtifactBudget
                         : EJitGlobalLayoutError::StaleIdentity,
                     rank);
    return false;
  }
  M.artifact = artifact;
  M.bytes = bytes;
  M.ready = true;
  state_->globalLayoutArtifactBytes.storeRelaxed(used + bytes);
  state_->globalLayoutReady.fetchAdd(1);
  if (decodeReqTier(R.funcIndex) == kEJitTierPgoUse) {
    if (layoutSampled_)
      layoutSampled_(layoutCtx_, R);
    releaseSamplingAdmission(R);
    M.samplingReleased = true;
  } else
    cacheStageBatchRequest(R); // Complete-key marker, no executable pointer.
  dedupClear(R.funcIndex, R.generation);
  EJIT_DIAG("global layout ARTIFACT_READY rank=%zu func=%u bytes=%llu", rank,
            stripReqTier(R.funcIndex), static_cast<unsigned long long>(bytes));
  return true;
}

bool EJitSharedTaskPool::serviceGlobalLayout() {
  if (!layoutConfigured_)
    return false;
  if (globalLayoutState() == EJitGlobalLayoutState::Failed) {
    if (!layoutFailureRecorded_)
      failGlobalLayout(EJitGlobalLayoutError::StaleIdentity, 0);
    // Close and drain any remaining T1 rounds; never take fresh samples.
    for (auto &M : layoutMembers_) {
      if (M.samplingReleased)
        continue;
      bool DrainedRound = false;
      for (const auto &B : state_->buckets)
        for (const auto &S : B.slots)
          if (S.state.loadAcquire() ==
                  static_cast<uint32_t>(EJitSharedSlotState::Ready) &&
              S.tier.loadAcquire() == kEJitTierInstrumented &&
              sameIdentity(slotRequest(S), M.req, false) &&
              S.samplingExecutions.loadAcquire() == 0)
            DrainedRound = true;
      if (DrainedRound) {
        finishPgoFunction(stripReqTier(M.req.funcIndex), false,
                          "global-layout-failed-after-drain");
        if (publishFn_) {
          auto Abort = M.req;
          Abort.funcIndex = encodeReqTier(stripReqTier(Abort.funcIndex),
                                          kEJitTierInstrumented);
          publishFn_(publishCtx_, Abort, false);
        }
        M.samplingReleased = true;
      }
    }
    return false;
  }
  bool ExpectedPgo =
      decodeReqTier(layoutMembers_.front().req.funcIndex) == kEJitTierPgoUse;
  if (state_->mode.loadAcquire() !=
          static_cast<uint32_t>(EJitCompileMode::Async) ||
      (state_->pgoEnabled.loadAcquire() != 0) != ExpectedPgo ||
      (ExpectedPgo &&
       (state_->tier2Threshold.loadAcquire() != layoutSamplingThreshold_ ||
        state_->pgoMaxActiveFunctions.loadAcquire() != layoutSamplingMax_))) {
    failGlobalLayout(EJitGlobalLayoutError::StaleIdentity, 0);
    return true;
  }
  if (globalLayoutState() == EJitGlobalLayoutState::Complete) {
    for (size_t I = 0; I < layoutMembers_.size(); ++I)
      if (!versionsCurrent(layoutMembers_[I].req) ||
          layoutMembers_[I].req.generation !=
              state_->generation.loadAcquire()) {
        failGlobalLayout(EJitGlobalLayoutError::StaleIdentity, I);
        return true;
      }
    return false;
  }
  if (globalLayoutState() == EJitGlobalLayoutState::Collecting) {
    for (size_t I = 0; I < layoutMembers_.size(); ++I)
      if (layoutMembers_[I].ready && !versionsCurrent(layoutMembers_[I].req)) {
        failGlobalLayout(EJitGlobalLayoutError::StaleIdentity, I);
        return true;
      }
    if (++layoutPolls_ > layoutBudgets_.maxCollectionPolls) {
      size_t missing = 0;
      while (missing < layoutMembers_.size() && layoutMembers_[missing].ready)
        ++missing;
      failGlobalLayout(EJitGlobalLayoutError::MissingTarget, missing);
      return true;
    }
    for (size_t I = 0; I < layoutDraining_.size();) {
      auto R = layoutDraining_[I];
      if (!versionsCurrent(R) ||
          R.generation != state_->generation.loadAcquire()) {
        failGlobalLayout(EJitGlobalLayoutError::StaleIdentity,
                         globalMemberFor(R));
        return true;
      }
      if (!samplingDrained(R)) {
        ++I;
        continue;
      }
      layoutDraining_.erase(layoutDraining_.begin() + I);
      stageGlobalArtifact(R);
      if (globalLayoutState() == EJitGlobalLayoutState::Failed)
        return true;
    }
    if (state_->globalLayoutReady.loadAcquire() != layoutMembers_.size())
      return false;
    if (!transitionGlobalLayout(EJitGlobalLayoutState::Collecting,
                                EJitGlobalLayoutState::Linking)) {
      failGlobalLayout(EJitGlobalLayoutError::StaleIdentity, 0);
      return true;
    }
    uintptr_t previous = 0;
    for (size_t I = 0; I < layoutMembers_.size(); ++I) {
      auto &M = layoutMembers_[I];
      if (M.req.generation != state_->generation.loadAcquire() ||
          !versionsCurrent(M.req)) {
        failGlobalLayout(EJitGlobalLayoutError::StaleIdentity, I);
        return true;
      }
      if (!layoutLink_(layoutCtx_, M.artifact, &M.fn) || !M.fn) {
        failGlobalLayout(EJitGlobalLayoutError::Linking, I);
        return true;
      }
      uintptr_t current = reinterpret_cast<uintptr_t>(M.fn);
      if (I && current <= previous) {
        failGlobalLayout(EJitGlobalLayoutError::PhysicalOrder, I);
        return true;
      }
      previous = current;
      EJIT_DIAG("global layout LINKED rank=%zu func=%u body=%p", I,
                stripReqTier(M.req.funcIndex), M.fn);
    }
    if (!transitionGlobalLayout(EJitGlobalLayoutState::Linking,
                                EJitGlobalLayoutState::Linked)) {
      failGlobalLayout(EJitGlobalLayoutError::StaleIdentity, 0);
      return true;
    }
  }
  if (globalLayoutState() == EJitGlobalLayoutState::Linked ||
      globalLayoutState() == EJitGlobalLayoutState::Sealing) {
    if (globalLayoutState() == EJitGlobalLayoutState::Linked &&
        !transitionGlobalLayout(EJitGlobalLayoutState::Linked,
                                EJitGlobalLayoutState::Sealing)) {
      failGlobalLayout(EJitGlobalLayoutError::StaleIdentity, 0);
      return true;
    }
    if (!codeBatchFlushFn_ || !codeBatchFlushFn_(codeBatchCtx_)) {
      failGlobalLayout(EJitGlobalLayoutError::Seal, 0);
      return true;
    }
    for (size_t I = 0; I < layoutMembers_.size(); ++I) {
      auto &M = layoutMembers_[I];
      if (!codeReadyFn_ || !codeReadyFn_(codeBatchCtx_, M.fn) ||
          !codeRangeFn_ || !codeRangeFn_(codeRangeCtx_, M.fn, &M.range) ||
          !M.range.codeSize || M.range.poolKind != EJitCodePoolKind::Near ||
          reinterpret_cast<uintptr_t>(M.fn) < M.range.codeStart ||
          reinterpret_cast<uintptr_t>(M.fn) - M.range.codeStart >=
              M.range.codeSize) {
        failGlobalLayout(EJitGlobalLayoutError::Range, I);
        return true;
      }
    }
    clearGlobalArtifacts();
    if (!transitionGlobalLayout(EJitGlobalLayoutState::Sealing,
                                EJitGlobalLayoutState::Publishing)) {
      failGlobalLayout(EJitGlobalLayoutError::StaleIdentity, 0);
      return true;
    }
  }
  if (globalLayoutState() == EJitGlobalLayoutState::Publishing) {
    for (size_t I = 0; I < layoutMembers_.size(); ++I) {
      auto &M = layoutMembers_[I];
      if (M.published)
        continue;
      if (globalLayoutState() != EJitGlobalLayoutState::Publishing) {
        failGlobalLayout(EJitGlobalLayoutError::StaleIdentity, I);
        return true;
      }
      auto PS = cachePublish(M.req, M.fn, &M.range,
                             decodeReqTier(M.req.funcIndex) == kEJitTierPgoUse);
      if (PS != EJitPublishStatus::Published) {
        failGlobalLayout(PS == EJitPublishStatus::VersionMismatch
                             ? EJitGlobalLayoutError::StaleIdentity
                             : EJitGlobalLayoutError::Publish,
                         I);
        return true;
      }
      M.published = true;
      state_->globalLayoutPublished.fetchAdd(1);
      EJIT_STAT_INC(state_->counters.asyncCompiles);
      if (decodeReqTier(M.req.funcIndex) == kEJitTierPgoUse) {
        EJIT_STAT_INC(state_->counters.tier2Compiles);
        state_->pgoCompletedFunctions.fetchAdd(1);
      }
      if (publishFn_)
        publishFn_(publishCtx_, M.req, true);
      EJIT_DIAG(
          "global layout PUBLISHED rank=%zu func=%u body=%p range=0x%llx+%llu",
          I, stripReqTier(M.req.funcIndex), M.fn,
          static_cast<unsigned long long>(M.range.codeStart),
          static_cast<unsigned long long>(M.range.codeSize));
    }
    if (!transitionGlobalLayout(EJitGlobalLayoutState::Publishing,
                                EJitGlobalLayoutState::Complete)) {
      failGlobalLayout(EJitGlobalLayoutError::StaleIdentity, 0);
      return true;
    }
    publishCodePoolStats();
    EJIT_DIAG("global layout COMPLETE gen=%u members=%zu collectionPolls=%u",
              state_->generation.loadAcquire(), layoutMembers_.size(),
              layoutPolls_);
    return true;
  }
  return false;
}
