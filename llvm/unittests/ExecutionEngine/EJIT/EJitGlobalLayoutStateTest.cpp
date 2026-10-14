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
  unsigned prepared = 0, linked = 0, dropped = 0, sealed = 0;
  bool failPrepare = false, failLink = false, failSeal = false,
       badOrder = false;
  Harness() {
    pool.bind(state.get());
    pool.setMode(EJitCompileMode::Async);
    pool.setCodeBatchCallbacks([](void *, const void *) { return true; },
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
TEST(EJitGlobalLayoutState, UndeclaredTargetDoesNotShrinkManifest) {
  Harness H;
  ASSERT_TRUE(H.configure(manifest({0})));
  H.request(1);
  EXPECT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Failed);
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
  EXPECT_EQ(miss.status, EJitCompileOrGetStatus::OffMode);
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
