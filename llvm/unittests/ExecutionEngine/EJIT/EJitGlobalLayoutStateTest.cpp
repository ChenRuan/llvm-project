//===-- EJitGlobalLayoutStateTest.cpp - Production worker state tests
//-------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"
#include "gtest/gtest.h"
#include <memory>

using namespace llvm::ejit;
namespace {
struct Artifact {
  uint32_t function;
};
struct Harness {
  std::unique_ptr<EJitSharedTaskPoolState> state{new EJitSharedTaskPoolState{}};
  EJitSharedTaskPool pool;
  unsigned prepared = 0, linked = 0, dropped = 0, sealed = 0, compiled = 0;
  bool failPrepare = false, failLink = false, failSeal = false,
       badOrder = false, blockCodeReady = false;
  uintptr_t blockAbove = 0;
  Harness() {
    pool.bind(state.get());
    pool.setMode(EJitCompileMode::Async);
    pool.setCodeBatchCallbacks(
        [](void *p, const void *fn) {
          // Blocking codeReady parks a fresh compile owner-private instead of
          // publishing it, exactly like a real RW/NX batched near pool. Only
          // bodies at/above blockAbove are blocked: member bodies are
          // re-validated with codeReady AFTER the collective seal, so they
          // must stay reportable.
          auto &H = *static_cast<Harness *>(p);
          return !(H.blockCodeReady &&
                   reinterpret_cast<uintptr_t>(fn) >= H.blockAbove);
        },
        [](void *p) {
          auto &H = *static_cast<Harness *>(p);
          ++H.sealed;
          return !H.failSeal;
        },
        this);
    pool.setCodeRangeProvider(
        [](void *, const void *fn, EJitCompiledCodeInfo *info) {
          info->codeStart = reinterpret_cast<uintptr_t>(fn);
          info->codeSize = 32;
          info->poolBase = 0x100000;
          info->poolSize = 0x200000;
          info->poolKind = EJitCodePoolKind::Near;
          return true;
        },
        this);
    EXPECT_EQ(pool.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  }
  bool configure(std::vector<EJitCompileRequest> members,
                 EJitLayoutBudgets budgets = {}) {
    return pool.configureGlobalLayout(
        members, budgets,
        [](void *p, const EJitCompileRequest &r, void **out, uint64_t *bytes) {
          auto &H = *static_cast<Harness *>(p);
          if (H.failPrepare)
            return false;
          ++H.prepared;
          *out = new Artifact{stripReqTier(r.funcIndex)};
          *bytes = 64;
          return true;
        },
        [](void *p, void *a, void **out) {
          auto &H = *static_cast<Harness *>(p);
          if (H.failLink)
            return false;
          ++H.linked;
          *out = reinterpret_cast<void *>(0x100000u +
                                          (H.badOrder ? 1u : H.linked) * 64);
          return a != nullptr;
        },
        [](void *p, void *a) {
          ++static_cast<Harness *>(p)->dropped;
          delete static_cast<Artifact *>(a);
        },
        this);
  }
  void request(uint32_t function) {
    pool.compileOrGet(function, nullptr, 0, nullptr);
    pool.workerPollOnce();
  }
  ~Harness() { pool.ownerShutdown(); }
};
std::vector<EJitCompileRequest>
manifest(std::initializer_list<uint32_t> order) {
  std::vector<EJitCompileRequest> R;
  for (auto f : order) {
    EJitCompileRequest M{};
    M.funcIndex = f;
    R.push_back(M);
  }
  return R;
}
} // namespace

TEST(EJitGlobalLayoutState, AllReadyAcrossBatchesPreservesExplicitRank) {
  Harness H;
  ASSERT_TRUE(H.configure(manifest({2, 0, 1})));
  H.request(1);
  H.request(0);
  for (int I = 0; I < 3; ++I)
    H.pool.workerPollOnce();
  EXPECT_EQ(H.linked, 0u);
  EXPECT_EQ(H.sealed, 0u);
  EXPECT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Collecting);
  H.request(2);
  H.pool.workerPollOnce();
  EXPECT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Complete);
  EXPECT_EQ(H.state->globalLayoutPublished.loadAcquire(), 3u);
  auto a = H.pool.compileOrGet(2, nullptr, 0, nullptr);
  auto b = H.pool.compileOrGet(0, nullptr, 0, nullptr);
  auto c = H.pool.compileOrGet(1, nullptr, 0, nullptr);
  EXPECT_LT(a.fnPtr, b.fnPtr);
  EXPECT_LT(b.fnPtr, c.fnPtr);
  H.pool.releaseRead(a.bucketIndex);
  H.pool.releaseRead(b.bucketIndex);
  H.pool.releaseRead(c.bucketIndex);
  EXPECT_EQ(H.dropped, 3u);
}

TEST(EJitGlobalLayoutState, UntrackedLegacyReservationCannotBeReused) {
  Harness H;
  EXPECT_TRUE(H.pool.checkGlobalLayoutReservationHistory(false));
  EXPECT_FALSE(H.pool.checkGlobalLayoutReservationHistory(true));
}
TEST(EJitGlobalLayoutState, RetainedLayoutReservationCannotSwitchToLegacy) {
  Harness H;
  EXPECT_TRUE(H.pool.checkGlobalLayoutReservationHistory(true));
  EXPECT_EQ(H.pool.reserveGlobalLayoutCodeBytes(4096, true, 8192), 0u);
  EXPECT_FALSE(H.pool.checkGlobalLayoutReservationHistory(false));
}
TEST(EJitGlobalLayoutState, DuplicateManifestRejectedAtomically) {
  Harness H;
  EXPECT_FALSE(H.configure(manifest({1, 1})));
  EXPECT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Disabled);
  EXPECT_EQ(H.state->globalLayoutRequired.loadRelaxed(), 0u);
}
TEST(EJitGlobalLayoutState, BindingCollisionIsNotSilentlyAliased) {
  Harness H;
  auto M = manifest({1, 1});
  int A = 1, B = 2;
  M[0].boundCount = M[1].boundCount = 1;
  M[0].boundPointers[0] = {&A, sizeof(A), 0};
  M[1].boundPointers[0] = {&B, sizeof(B), 0};
  EXPECT_FALSE(H.configure(M));
}
TEST(EJitGlobalLayoutState, MissingTargetExhaustsBoundedCollection) {
  Harness H;
  EJitLayoutBudgets B;
  B.maxCollectionPolls = 4;
  ASSERT_TRUE(H.configure(manifest({0, 1}), B));
  H.request(1);
  for (int I = 0; I < 8; ++I)
    H.pool.workerPollOnce();
  EXPECT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Failed);
  EXPECT_EQ(H.linked, 0u);
  EXPECT_EQ(H.state->globalLayoutError.loadAcquire(),
            static_cast<uint32_t>(EJitGlobalLayoutError::MissingTarget));
  EXPECT_EQ(H.state->globalLayoutArtifactBytes.loadAcquire(), 0u);
}
TEST(EJitGlobalLayoutState, ObjectBudgetNeverAllocatesFinalCode) {
  Harness H;
  EJitLayoutBudgets B;
  B.maxArtifactBytes = 32;
  ASSERT_TRUE(H.configure(manifest({0}), B));
  H.request(0);
  EXPECT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Failed);
  EXPECT_EQ(H.linked, 0u);
  EXPECT_EQ(H.dropped, 1u);
}
TEST(EJitGlobalLayoutState, PreparationFailureNeverCompletes) {
  Harness H;
  H.failPrepare = true;
  ASSERT_TRUE(H.configure(manifest({0})));
  H.request(0);
  EXPECT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Failed);
  EXPECT_EQ(H.sealed, 0u);
}
TEST(EJitGlobalLayoutState, LinkFailureNeverPublishes) {
  Harness H;
  H.failLink = true;
  ASSERT_TRUE(H.configure(manifest({0})));
  H.request(0);
  H.pool.workerPollOnce();
  EXPECT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Failed);
  EXPECT_EQ(H.state->globalLayoutPublished.loadRelaxed(), 0u);
}
TEST(EJitGlobalLayoutState, ActualBodyOrderViolationFailsClosed) {
  Harness H;
  H.badOrder = true;
  ASSERT_TRUE(H.configure(manifest({0, 1})));
  H.request(0);
  H.request(1);
  H.pool.workerPollOnce();
  EXPECT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Failed);
  EXPECT_EQ(H.state->globalLayoutError.loadRelaxed(),
            static_cast<uint32_t>(EJitGlobalLayoutError::PhysicalOrder));
  EXPECT_EQ(H.state->globalLayoutPublished.loadRelaxed(), 0u);
}
TEST(EJitGlobalLayoutState, SealFailureRetainsIncompleteGeneration) {
  Harness H;
  H.failSeal = true;
  ASSERT_TRUE(H.configure(manifest({0})));
  H.request(0);
  H.pool.workerPollOnce();
  EXPECT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Failed);
  EXPECT_EQ(H.state->globalLayoutPublished.loadRelaxed(), 0u);
}
TEST(EJitGlobalLayoutState, UndeclaredTargetParksWithoutShrinkingManifest) {
  Harness H;
  ASSERT_TRUE(H.configure(manifest({0})));
  H.request(1);
  // An entirely undeclared funcIndex no longer fails the layout: it parks as
  // unlisted batch backlog for the collective append, and the manifest keeps
  // its exact member set.
  EXPECT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Collecting);
  EXPECT_EQ(H.state->globalLayoutRequired.loadRelaxed(), 1u);
  EXPECT_EQ(H.state->globalLayoutUnlistedReady.loadAcquire(), 1u);
  H.request(0);
  H.pool.workerPollOnce();
  EXPECT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Complete);
  EXPECT_EQ(H.state->globalLayoutUnlistedReady.loadAcquire(), 0u);
  // The parked request was consumed at the collective point. This harness
  // wires no compiler, so its runCompile failed and appended nothing — the
  // layout still completed with the manifest members.
  EXPECT_EQ(H.state->globalLayoutUnlistedAppended.loadAcquire(), 0u);
  EXPECT_EQ(H.state->globalLayoutPublished.loadAcquire(), 1u);
}

TEST(EJitGlobalLayoutState, DeclaredFuncWithMismatchedIdentityStillFails) {
  Harness H;
  auto M = manifest({0});
  M[0].numDims = 1;
  M[0].dims[0] = {0, 0};
  ASSERT_TRUE(H.configure(M));
  H.pool.setInstanceEnabled(1, 0, true);
  EJitDimPair Other{1, 0};
  H.pool.compileOrGet(0, &Other, 1, nullptr);
  H.pool.workerPollOnce();
  // The manifest declares funcIndex 0; an identity it does not declare is a
  // deployment error, not an append candidate. The precision kill stays.
  EXPECT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Failed);
  EXPECT_EQ(H.state->globalLayoutError.loadAcquire(),
            static_cast<uint32_t>(EJitGlobalLayoutError::Manifest));
  EXPECT_EQ(H.state->globalLayoutRequired.loadRelaxed(), 1u);
}
TEST(EJitGlobalLayoutState,
     RetainedReservationIsBoundedAcrossOwnerGenerations) {
  Harness H;
  EXPECT_EQ(H.pool.reserveGlobalLayoutCodeBytes(64, true, 128), 0u);
  EXPECT_EQ(H.pool.reserveGlobalLayoutCodeBytes(64, true, 128), 64u);
  EXPECT_EQ(H.pool.reserveGlobalLayoutCodeBytes(1, true, 128), UINT64_MAX);
  H.pool.ownerShutdown();
  EXPECT_EQ(H.pool.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  EXPECT_EQ(H.pool.reserveGlobalLayoutCodeBytes(1, true, 128), UINT64_MAX);
}

TEST(EJitGlobalLayoutState, InvalidationDuringLinkCannotBecomeComplete) {
  Harness H;
  ASSERT_TRUE(H.configure(manifest({0, 1})));
  H.pool.setPublishCallback(
      [](void *ctx, const EJitCompileRequest &, bool published) {
        if (published)
          static_cast<Harness *>(ctx)->pool.setInstanceEnabled(0, 0, true);
      },
      &H);
  H.request(0);
  H.request(1);
  H.pool.workerPollOnce();
  EXPECT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Refreshing);
  EXPECT_EQ(H.state->globalLayoutPublished.loadAcquire(), 1u);
  H.pool.workerPollOnce();
  EXPECT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Collecting);
}

TEST(EJitGlobalLayoutState, StagedVersionInvalidationReleasesObjects) {
  Harness H;
  H.pool.setInstanceEnabled(0, 0, true);
  auto M = manifest({0, 1});
  for (auto &R : M) {
    R.numDims = 1;
    R.dims[0] = {0, 0};
    R.versions[0] = 1;
  }
  ASSERT_TRUE(H.configure(M));
  EJitDimPair D{0, 0};
  H.pool.compileOrGet(0, &D, 1, nullptr);
  H.pool.workerPollOnce();
  EXPECT_EQ(H.prepared, 1u);
  H.pool.setInstanceEnabled(0, 0, false);
  H.pool.workerPollOnce();
  EXPECT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Refreshing);
  H.pool.workerPollOnce();
  EXPECT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Collecting);
  EXPECT_EQ(H.dropped, 1u);
  EXPECT_EQ(H.linked, 0u);
}

TEST(EJitGlobalLayoutState, FullCellTrpOrderIsExplicitNotDimensionMajor) {
  Harness H;
  for (uint32_t D = 0; D < 2; ++D)
    for (uint32_t I = 0; I < 2; ++I)
      H.pool.setInstanceEnabled(D, I, true);
  auto M = manifest({1, 0, 1, 0});
  for (uint32_t I = 0; I < M.size(); ++I) {
    M[I].numDims = 2;
    M[I].dims[0] = {0, I < 2 ? 1u : 0u};
    M[I].dims[1] = {1, I % 2};
    M[I].versions[0] = M[I].versions[1] = 1;
  }
  ASSERT_TRUE(H.configure(M));
  for (auto I : {3u, 1u, 0u, 2u}) {
    H.pool.compileOrGet(M[I].funcIndex, M[I].dims, 2, nullptr);
    H.pool.workerPollOnce();
  }
  H.pool.workerPollOnce();
  ASSERT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Complete);
  uintptr_t Previous = 0;
  for (const auto &R : M) {
    auto Hit = H.pool.compileOrGet(R.funcIndex, R.dims, 2, nullptr);
    ASSERT_NE(Hit.fnPtr, nullptr);
    EXPECT_GT(reinterpret_cast<uintptr_t>(Hit.fnPtr), Previous);
    Previous = reinterpret_cast<uintptr_t>(Hit.fnPtr);
    H.pool.releaseRead(Hit.bucketIndex);
  }
}

TEST(EJitGlobalLayoutState, ExplicitStaleOrInactiveVersionsRejectImmediately) {
  Harness H;
  auto M = manifest({0});
  M[0].numDims = 1;
  M[0].dims[0] = {0, 0};
  M[0].layoutVersionPolicy = 1;
  EXPECT_FALSE(H.configure(M));
  H.pool.setInstanceEnabled(0, 0, true);
  EXPECT_FALSE(H.configure(M));
  M[0].versions[0] = 1;
  EXPECT_TRUE(H.configure(M));
}

#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
TEST(EJitGlobalLayoutState, PausedRejectedLeaseRetriesSameSealedBodies) {
  Harness H;
  H.pool.setPgoEnabled(true, 1, 4);
  auto M = manifest({0});
  M[0].funcIndex = encodeReqTier(0, kEJitTierPgoUse);
  H.pool.setCompiler(
      [](void *, const EJitCompileRequest &, void **out) {
        *out = reinterpret_cast<void *>(0x900000);
        return true;
      },
      nullptr);
  ASSERT_TRUE(H.configure(M));
  H.request(0);
  auto sample = H.pool.compileOrGet(0, nullptr, 0, nullptr);
  ASSERT_NE(sample.fnPtr, nullptr);
  H.pool.releaseRead(sample.bucketIndex);
  H.pool.workerPollOnce(); // emits owned object, still Collecting
  ASSERT_EQ(H.state->globalLayoutReady.loadAcquire(), 1u);
  H.pool.setSamplingLeaseHook(
      [](void *p) {
        auto &H = *static_cast<Harness *>(p);
        H.pool.workerPollOnce(); // publishes against the paused acquisition
        EXPECT_EQ(H.pool.globalLayoutState(),
                  EJitGlobalLayoutState::Publishing);
        EXPECT_EQ(H.state->globalLayoutPublished.loadAcquire(), 0u);
      },
      &H);
  auto rejected = H.pool.compileOrGet(0, nullptr, 0, nullptr);
  EXPECT_EQ(rejected.fnPtr, nullptr); // freeze never permits a genuine new T1
  H.pool.setSamplingLeaseHook(nullptr, nullptr);
  EXPECT_EQ(H.linked, 1u);
  EXPECT_EQ(H.sealed, 1u);
  H.pool.workerPollOnce();
  EXPECT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Complete);
  EXPECT_EQ(H.linked, 1u);
  EXPECT_EQ(H.sealed, 1u);
}
#endif

TEST(EJitGlobalLayoutState, TerminalQueuedTier1ReleasesAdmission) {
  Harness H;
  H.pool.setPgoEnabled(true, 8, 4);
  auto M = manifest({0});
  M[0].funcIndex = encodeReqTier(0, kEJitTierPgoUse);
  ASSERT_TRUE(H.configure(M));
  H.pool.compileOrGet(0, nullptr, 0, nullptr);
  ASSERT_EQ(H.state->pgoActiveFunctionCount.loadAcquire(), 1u);
  H.pool.setPgoEnabled(false, 0, 4);
  for (unsigned I = 0; I < 4; ++I)
    H.pool.workerPollOnce();
  EXPECT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Failed);
  EXPECT_EQ(H.state->pgoActiveFunctionCount.loadAcquire(), 0u);
}

TEST(EJitGlobalLayoutState, FailedBudgetNeedsNewEventOrExplicitRefresh) {
  Harness H;
  EJitLayoutBudgets B;
  B.maxCollectionPolls = 2;
  ASSERT_TRUE(H.configure(manifest({0}), B));
  for (unsigned I = 0; I < 10; ++I)
    H.pool.workerPollOnce();
  ASSERT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Failed);
  uint32_t gen = H.state->generation.loadAcquire();
  for (unsigned I = 0; I < 10; ++I)
    H.pool.workerPollOnce();
  EXPECT_EQ(H.state->generation.loadAcquire(), gen);
  EXPECT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Failed);
  ASSERT_TRUE(H.pool.requestGlobalLayoutRefresh(manifest({0})));
  H.pool.workerPollOnce();
  EXPECT_EQ(H.state->generation.loadAcquire(), gen + 1);
  H.request(0);
  H.pool.workerPollOnce();
  EXPECT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Complete);
}

TEST(EJitGlobalLayoutState, FailedLayoutStillServesUnaffectedLegalCachedHits) {
  Harness H;
  ASSERT_TRUE(H.configure(manifest({0})));
  H.request(0);
  H.pool.workerPollOnce();
  ASSERT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Complete);
  auto old = H.pool.compileOrGet(0, nullptr, 0, nullptr);
  ASSERT_NE(old.fnPtr, nullptr);
  H.pool.releaseRead(old.bucketIndex);
  H.pool.setPgoEnabled(true, 8, 4); // Unsupported control change is diagnosed.
  ASSERT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Failed);
  auto legal = H.pool.compileOrGet(0, nullptr, 0, nullptr);
  EXPECT_EQ(legal.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_EQ(legal.fnPtr, old.fnPtr);
  H.pool.releaseRead(legal.bucketIndex);
  auto miss =
      H.pool.compileOrGet(1, nullptr, 0, reinterpret_cast<void *>(0x777));
  // A Failed layout no longer forces OffMode: the miss takes the ordinary
  // enqueue path (Tier-1 here because PGO was just enabled) and the function
  // compiles through the normal fallback flow.
  EXPECT_EQ(miss.status, EJitCompileOrGetStatus::EnqueuedPending);
  EXPECT_EQ(miss.fnPtr, reinterpret_cast<void *>(0x777));
}

TEST(EJitGlobalLayoutState, PeerInitSeedCannotChangeLiveOwnerSampling) {
  Harness H;
  ASSERT_TRUE(H.configure(manifest({0})));
  EJitSharedTaskPool peer;
  peer.bind(H.state.get());
  peer.configurePgoForInit(true, 8);
  EXPECT_EQ(H.state->pgoEnabled.loadAcquire(), 0u);
  EXPECT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Collecting);
  auto M = manifest({0});
  M[0].numDims = 1;
  M[0].dims[0] = {0, 0};
  M[0].layoutVersionPolicy = 1;
  EXPECT_FALSE(peer.validateGlobalLayoutAttachment(M));
  EXPECT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Collecting);
}

TEST(EJitGlobalLayoutState, UnlistedBaselineAppendedAfterManifestBodies) {
  Harness H;
  H.pool.setCompiler(
      [](void *p, const EJitCompileRequest &, void **out) {
        auto &H = *static_cast<Harness *>(p);
        *out = reinterpret_cast<void *>(0x200000u + H.compiled++ * 32u);
        return true;
      },
      &H);
  ASSERT_TRUE(H.configure(manifest({0})));
  H.request(2);
  H.request(1);
  // Unlisted Baseline misses park in the deferred batch backlog while the
  // manifest is still collecting; nothing compiles into the near window.
  EXPECT_EQ(H.state->globalLayoutUnlistedReady.loadAcquire(), 2u);
  EXPECT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Collecting);
  H.request(0); // the member stages its artifact
  H.blockCodeReady = true;
  H.blockAbove = 0x200000u;
  H.pool.workerPollOnce();
  ASSERT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Complete);
  EXPECT_EQ(H.state->globalLayoutUnlistedAppended.loadAcquire(), 2u);
  EXPECT_EQ(H.state->globalLayoutUnlistedReady.loadAcquire(), 0u);
  EXPECT_EQ(H.sealed, 1u); // one collective seal covered members and appends
  H.blockCodeReady = false;
  ASSERT_TRUE(H.pool.flushCodeBatch());
  auto r0 = H.pool.compileOrGet(0, nullptr, 0, nullptr);
  auto r1 = H.pool.compileOrGet(1, nullptr, 0, nullptr);
  auto r2 = H.pool.compileOrGet(2, nullptr, 0, nullptr);
  ASSERT_NE(r0.fnPtr, nullptr);
  ASSERT_NE(r1.fnPtr, nullptr);
  ASSERT_NE(r2.fnPtr, nullptr);
  // The batch backlog sorted by funcIndex: 1 before 2, both after the member.
  EXPECT_EQ(r1.fnPtr, reinterpret_cast<void *>(0x200000u));
  EXPECT_EQ(r2.fnPtr, reinterpret_cast<void *>(0x200020u));
  EXPECT_LT(r0.fnPtr, r1.fnPtr);
  EXPECT_LT(r1.fnPtr, r2.fnPtr);
  H.pool.releaseRead(r0.bucketIndex);
  H.pool.releaseRead(r1.bucketIndex);
  H.pool.releaseRead(r2.bucketIndex);
  // Post-Complete ordinary path: a fresh unlisted miss defers to the batch
  // backlog again and publishes through the ordinary flush, not a collective.
  H.request(3);
  EXPECT_EQ(H.state->globalLayoutUnlistedReady.loadAcquire(), 0u);
  ASSERT_TRUE(H.pool.flushCodeBatch());
  auto r3 = H.pool.compileOrGet(3, nullptr, 0, nullptr);
  ASSERT_NE(r3.fnPtr, nullptr);
  EXPECT_EQ(r3.fnPtr, reinterpret_cast<void *>(0x200040u));
  EXPECT_LT(r0.fnPtr, r3.fnPtr);
  H.pool.releaseRead(r3.bucketIndex);
}

TEST(EJitGlobalLayoutState, FailedLayoutReleasesParkedUnlistedForRetry) {
  Harness H;
  EJitLayoutBudgets B;
  B.maxCollectionPolls = 2;
  ASSERT_TRUE(H.configure(manifest({0}), B));
  H.request(1); // unlisted Baseline parks a staged batch marker
  EXPECT_EQ(H.state->globalLayoutUnlistedReady.loadAcquire(), 1u);
  for (unsigned I = 0; I < 10; ++I)
    H.pool.workerPollOnce();
  ASSERT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Failed);
  EXPECT_EQ(H.state->globalLayoutUnlistedReady.loadAcquire(), 0u);
  // The staged marker was dropped with the failure: a fresh miss re-enqueues
  // instead of sticking on AlreadyPending forever.
  auto miss =
      H.pool.compileOrGet(1, nullptr, 0, reinterpret_cast<void *>(0x777));
  EXPECT_EQ(miss.status, EJitCompileOrGetStatus::EnqueuedPending);
  EXPECT_EQ(miss.fnPtr, reinterpret_cast<void *>(0x777));
}

TEST(EJitGlobalLayoutState, SealFailureReleasesStagedAppendMarkers) {
  Harness H;
  H.failSeal = true;
  H.pool.setCompiler(
      [](void *p, const EJitCompileRequest &, void **out) {
        auto &H = *static_cast<Harness *>(p);
        *out = reinterpret_cast<void *>(0x200000u + H.compiled++ * 32u);
        return true;
      },
      &H);
  ASSERT_TRUE(H.configure(manifest({0})));
  H.request(1); // unlisted Baseline parks in the batch backlog
  H.request(0); // the member stages its artifact
  H.blockCodeReady = true;
  H.blockAbove = 0x200000u;
  H.pool.workerPollOnce(); // collective append stages, then the seal fails
  ASSERT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Failed);
  EXPECT_EQ(H.state->globalLayoutError.loadAcquire(),
            static_cast<uint32_t>(EJitGlobalLayoutError::Seal));
  H.blockCodeReady = false;
  // The collective-window staged marker (an overwrote-in-place Pending slot
  // carrying the appended body) was dropped with the failure: a fresh miss
  // re-enqueues instead of wedging on AlreadyPending.
  auto miss =
      H.pool.compileOrGet(1, nullptr, 0, reinterpret_cast<void *>(0x777));
  EXPECT_EQ(miss.status, EJitCompileOrGetStatus::EnqueuedPending);
  EXPECT_EQ(miss.fnPtr, reinterpret_cast<void *>(0x777));
}

TEST(EJitGlobalLayoutState, RefreshReleasesParkedUnlistedRequests) {
  Harness H;
  ASSERT_TRUE(H.configure(manifest({0})));
  H.request(1); // unlisted Baseline parks a staged batch marker
  EXPECT_EQ(H.state->globalLayoutUnlistedReady.loadAcquire(), 1u);
  ASSERT_TRUE(H.pool.requestGlobalLayoutRefresh(manifest({0})));
  H.pool.workerPollOnce();
  EXPECT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Collecting);
  EXPECT_EQ(H.state->globalLayoutUnlistedReady.loadAcquire(), 0u);
  // Markers were dropped before the generation bump, so the retired request
  // cannot wedge the next generation's misses.
  auto miss =
      H.pool.compileOrGet(1, nullptr, 0, reinterpret_cast<void *>(0x777));
  EXPECT_EQ(miss.status, EJitCompileOrGetStatus::EnqueuedPending);
  EXPECT_EQ(miss.fnPtr, reinterpret_cast<void *>(0x777));
}

#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
TEST(EJitGlobalLayoutState, UnlistedTier2ParksReleasesAdmissionAndAppends) {
  Harness H;
  H.pool.setPgoEnabled(true, 1, 4);
  H.pool.setCompiler(
      [](void *p, const EJitCompileRequest &, void **out) {
        auto &H = *static_cast<Harness *>(p);
        *out = reinterpret_cast<void *>(0x900000u + H.compiled++ * 32u);
        return true;
      },
      &H);
  auto M = manifest({0});
  M[0].funcIndex = encodeReqTier(0, kEJitTierPgoUse);
  ASSERT_TRUE(H.configure(M));
  // Unlisted func 1: Tier-1 compiles immediately (far pool, unordered), then
  // its armed Tier-2 parks for the collective append.
  H.request(1);
  auto s1 = H.pool.compileOrGet(1, nullptr, 0, nullptr);
  ASSERT_NE(s1.fnPtr, nullptr);
  H.pool.releaseRead(s1.bucketIndex);
  H.pool.workerPollOnce(); // pops the armed Tier-2 and parks it
  EXPECT_EQ(H.state->pgoActiveFunctionCount.loadAcquire(), 0u)
      << "parking must release the sampling admission immediately";
  EXPECT_EQ(H.state->globalLayoutUnlistedReady.loadAcquire(), 1u);
  // Member func 0: Tier-1 compiles, then its drained Tier-2 stages.
  H.request(0);
  auto s0 = H.pool.compileOrGet(0, nullptr, 0, nullptr);
  ASSERT_NE(s0.fnPtr, nullptr);
  H.pool.releaseRead(s0.bucketIndex);
  H.pool.workerPollOnce();
  ASSERT_EQ(H.state->globalLayoutReady.loadAcquire(), 1u);
  H.blockCodeReady = true;
  H.blockAbove = 0x800000u;
  H.pool.workerPollOnce(); // collective: parked Tier-2 compiles into the
                           // unsealed window; one seal; member publishes
  ASSERT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Complete);
  EXPECT_EQ(H.state->globalLayoutUnlistedAppended.loadAcquire(), 1u);
  H.blockCodeReady = false;
  ASSERT_TRUE(H.pool.flushCodeBatch());
  auto r1 = H.pool.compileOrGet(1, nullptr, 0, nullptr);
  auto r0 = H.pool.compileOrGet(0, nullptr, 0, nullptr);
  ASSERT_NE(r1.fnPtr, nullptr);
  ASSERT_NE(r0.fnPtr, nullptr);
  EXPECT_EQ(r1.fnPtr, reinterpret_cast<void *>(0x900040u)); // Tier-2 body won
  EXPECT_EQ(r0.fnPtr, reinterpret_cast<void *>(0x100040u));
  EXPECT_LT(r0.fnPtr, r1.fnPtr); // appended after the manifest body
  H.pool.releaseRead(r1.bucketIndex);
  H.pool.releaseRead(r0.bucketIndex);
}

TEST(EJitGlobalLayoutState, UnlistedTier2RearmsSupersedeTheirPark) {
  Harness H;
  H.pool.setPgoEnabled(true, 1, 4);
  H.pool.setCompiler(
      [](void *, const EJitCompileRequest &, void **out) {
        *out = reinterpret_cast<void *>(0x900000u);
        return true;
      },
      nullptr);
  auto M = manifest({0});
  M[0].funcIndex = encodeReqTier(0, kEJitTierPgoUse);
  ASSERT_TRUE(H.configure(M));
  H.request(1); // Tier-1 published; the manifest member is never requested
  auto s = H.pool.compileOrGet(1, nullptr, 0, nullptr);
  ASSERT_NE(s.fnPtr, nullptr);
  H.pool.releaseRead(s.bucketIndex);
  H.pool.workerPollOnce(); // first park
  // A saturated Tier-1 re-arms on every lookup: each duplicate must
  // supersede the earlier park of the same identity instead of filling the
  // bounded list, which would starve other functions' parks and storm the
  // queueFull counter.
  for (uint32_t I = 0; I < 64; ++I) {
    auto Rearm = H.pool.compileOrGet(1, nullptr, 0, nullptr);
    EXPECT_EQ(Rearm.status, EJitCompileOrGetStatus::AlreadyPending);
    H.pool.workerPollOnce();
  }
  EXPECT_EQ(H.state->globalLayoutUnlistedReady.loadAcquire(), 1u);
  EXPECT_EQ(H.state->counters.queueFull.loadRelaxed(), 0u);
  EXPECT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Collecting);
  // The function stays on its live Tier-1 and can keep arming.
  auto Rearm = H.pool.compileOrGet(1, nullptr, 0, nullptr);
  EXPECT_EQ(Rearm.status, EJitCompileOrGetStatus::AlreadyPending);
}
#endif
