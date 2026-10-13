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
  EXPECT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Failed);
  EXPECT_EQ(H.state->globalLayoutPublished.loadAcquire(), 1u);
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
  EXPECT_EQ(H.pool.globalLayoutState(), EJitGlobalLayoutState::Failed);
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
