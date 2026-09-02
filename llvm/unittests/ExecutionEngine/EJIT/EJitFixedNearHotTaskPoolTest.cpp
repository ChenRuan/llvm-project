//===-- EJitFixedNearHotTaskPoolTest.cpp - fixed near-hot tests --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitModuleLoader.h"
#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"
#include "gtest/gtest.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace llvm::ejit;

#if defined(_WIN32)
namespace llvm::ejit {
const std::string &EJitModuleLoader::getFuncNameByFuncIdx(uint32_t) const {
  static const std::string Empty;
  return Empty;
}
} // namespace llvm::ejit
#endif

namespace {

void *codeFor(uint32_t FuncIndex) {
  return reinterpret_cast<void *>(0x100000ull +
                                  static_cast<uintptr_t>(FuncIndex) * 64u);
}

EJitDimPair dim(uint32_t Type, uint32_t Instance) {
  return EJitDimPair{Type, Instance};
}

struct FixedNearEntry {
  void *fn = nullptr;
  uint32_t poolId = kEJitNearHotPublicPoolId;
  bool ready = false;
};

struct FixedNearPublishCtx {
  std::vector<FixedNearEntry> entries;
  uint32_t flushCalls = 0;
  bool failCell1 = false;
};

uint32_t fixedNearPoolForRequest(const EJitCompileRequest &Req) {
  if (Req.numDims == 0)
    return kEJitNearHotPublicPoolId;
  return Req.dims[0].instanceId < kEJitNearHotCellPoolCount
             ? Req.dims[0].instanceId
             : kEJitNearHotPublicPoolId;
}

bool mockFixedNearCompile(void *Ctx, const EJitCompileRequest &Req,
                          void **OutFn) {
  auto *C = static_cast<FixedNearPublishCtx *>(Ctx);
  void *Fn = reinterpret_cast<void *>(
      0x600000ull + static_cast<uintptr_t>(C->entries.size()) * 0x100u);
  C->entries.push_back({Fn, fixedNearPoolForRequest(Req), false});
  *OutFn = Fn;
  return true;
}

bool mockFixedNearReady(void *Ctx, const void *Fn) {
  auto *C = static_cast<FixedNearPublishCtx *>(Ctx);
  for (const FixedNearEntry &Entry : C->entries)
    if (Entry.fn == Fn)
      return Entry.ready;
  return false;
}

bool mockFixedNearRange(void *Ctx, const void *Fn, EJitCompiledCodeInfo *Out) {
  auto *C = static_cast<FixedNearPublishCtx *>(Ctx);
  for (const FixedNearEntry &Entry : C->entries) {
    if (Entry.fn != Fn)
      continue;
    Out->fnPtr = const_cast<void *>(Fn);
    Out->codeStart = reinterpret_cast<uintptr_t>(Fn);
    Out->codeSize = 64;
    Out->poolId = Entry.poolId;
    Out->poolKind = EJitCodePoolKind::Near;
    Out->poolBase =
        0x80000000ull + static_cast<uint64_t>(Entry.poolId) * 0x200000ull;
    Out->poolSize =
        Entry.poolId == kEJitNearHotPublicPoolId ? 0x400000ull : 0x200000ull;
    return true;
  }
  return false;
}

bool mockFixedNearLegacyFlush(void *) { return true; }

bool mockFixedNearFlushPool(void *Ctx, uint32_t PoolId) {
  auto *C = static_cast<FixedNearPublishCtx *>(Ctx);
  ++C->flushCalls;
  if (PoolId == 1 && C->failCell1)
    return false;
  for (FixedNearEntry &Entry : C->entries)
    if (Entry.poolId == PoolId)
      Entry.ready = true;
  return true;
}

class FixedNearTaskPoolTest : public ::testing::Test {
protected:
  void SetUp() override {
    EJitCoreId::resetForTest();
    ejitIcacheClearAll();
    State = std::make_unique<EJitSharedTaskPoolState>();
  }

  void TearDown() override {
    ejitIcacheClearAll();
    EJitCoreId::resetForTest();
  }

  EJitSharedCacheSlot *findReadySlot(uint32_t FuncIndex) {
    for (uint32_t Bucket = 0; Bucket < kEJitSharedCacheBuckets; ++Bucket)
      for (uint32_t SlotIndex = 0; SlotIndex < kEJitSharedCacheSlots;
           ++SlotIndex) {
        EJitSharedCacheSlot &Slot = State->buckets[Bucket].slots[SlotIndex];
        if (Slot.state.loadAcquire() ==
                static_cast<uint32_t>(EJitSharedSlotState::Ready) &&
            Slot.funcIndex == FuncIndex)
          return &Slot;
      }
    return nullptr;
  }

  std::unique_ptr<EJitSharedTaskPoolState> State;
};

TEST_F(FixedNearTaskPoolTest, WaitsForQuiescenceAndPublishesByPool) {
  FixedNearPublishCtx Ctx;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(State.get());
  Owner.setCompiler(&mockFixedNearCompile, &Ctx);
  Owner.setCodeRangeProvider(&mockFixedNearRange, &Ctx);
  Owner.setCodeBatchCallbacks(&mockFixedNearReady, &mockFixedNearLegacyFlush,
                              &Ctx);
  Owner.setCodeBatchPoolFlushCallback(&mockFixedNearFlushPool, &Ctx);
  Owner.setMode(EJitCompileMode::Async);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  Owner.setInstanceEnabled(0, 0, true);
  Owner.setInstanceEnabled(0, 1, true);

  const EJitDimPair Cell0[1] = {dim(0, 0)};
  const EJitDimPair Cell1[1] = {dim(0, 1)};
  EXPECT_EQ(Owner.compileOrGet(201, Cell0, 1, codeFor(201)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  EXPECT_EQ(Owner.compileOrGet(202, Cell1, 1, codeFor(202)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  EXPECT_EQ(Owner.compileOrGet(203, nullptr, 0, codeFor(203)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_EQ(Owner.pollBudget(3), 3u);
  ASSERT_EQ(Owner.pendingPublishCount(), 3u);
  ASSERT_EQ(Ctx.flushCalls, 0u);

  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Idle);
  EXPECT_EQ(Ctx.flushCalls, 0u);
  EXPECT_EQ(Owner.pendingPublishCount(), 3u);

  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Consumed);
  EXPECT_EQ(Ctx.flushCalls, 3u);
  EXPECT_EQ(Owner.pendingPublishCount(), 0u);
  ASSERT_EQ(Ctx.entries.size(), 3u);
  EXPECT_EQ(Ctx.entries[0].poolId, 0u);
  EXPECT_EQ(Ctx.entries[1].poolId, 1u);
  EXPECT_EQ(Ctx.entries[2].poolId, kEJitNearHotPublicPoolId);
  for (const FixedNearEntry &Entry : Ctx.entries)
    EXPECT_TRUE(Entry.ready);

  auto H0 = Owner.compileOrGet(201, Cell0, 1, codeFor(201));
  auto H1 = Owner.compileOrGet(202, Cell1, 1, codeFor(202));
  auto HP = Owner.compileOrGet(203, nullptr, 0, codeFor(203));
  EXPECT_EQ(H0.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_EQ(H1.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_EQ(HP.status, EJitCompileOrGetStatus::CacheHit);
  if (H0.hasReadToken)
    Owner.releaseRead(H0.bucketIndex);
  if (H1.hasReadToken)
    Owner.releaseRead(H1.bucketIndex);
  if (HP.hasReadToken)
    Owner.releaseRead(HP.bucketIndex);
}

TEST_F(FixedNearTaskPoolTest, FailureDoesNotBlockOtherPools) {
  FixedNearPublishCtx Ctx;
  Ctx.failCell1 = true;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(State.get());
  Owner.setCompiler(&mockFixedNearCompile, &Ctx);
  Owner.setCodeRangeProvider(&mockFixedNearRange, &Ctx);
  Owner.setCodeBatchCallbacks(&mockFixedNearReady, &mockFixedNearLegacyFlush,
                              &Ctx);
  Owner.setCodeBatchPoolFlushCallback(&mockFixedNearFlushPool, &Ctx);
  Owner.setMode(EJitCompileMode::Async);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  Owner.setInstanceEnabled(0, 0, true);
  Owner.setInstanceEnabled(0, 1, true);

  const EJitDimPair Cell0[1] = {dim(0, 0)};
  const EJitDimPair Cell1[1] = {dim(0, 1)};
  EXPECT_EQ(Owner.compileOrGet(301, Cell0, 1, codeFor(301)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  EXPECT_EQ(Owner.compileOrGet(302, Cell1, 1, codeFor(302)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  EXPECT_EQ(Owner.compileOrGet(303, nullptr, 0, codeFor(303)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_EQ(Owner.pollBudget(3), 3u);
  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Idle);
  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Consumed);

  ASSERT_EQ(Ctx.entries.size(), 3u);
  EXPECT_TRUE(Ctx.entries[0].ready);
  EXPECT_FALSE(Ctx.entries[1].ready);
  EXPECT_TRUE(Ctx.entries[2].ready);
  EXPECT_NE(findReadySlot(301), nullptr);
  EXPECT_EQ(findReadySlot(302), nullptr);
  EXPECT_NE(findReadySlot(303), nullptr);
  EXPECT_EQ(Owner.pendingPublishCount(), 0u);
}

TEST_F(FixedNearTaskPoolTest, MissingPoolFlushFallsBackCleanly) {
  FixedNearPublishCtx Ctx;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(State.get());
  Owner.setCompiler(&mockFixedNearCompile, &Ctx);
  Owner.setCodeRangeProvider(&mockFixedNearRange, &Ctx);
  Owner.setCodeBatchCallbacks(&mockFixedNearReady, &mockFixedNearLegacyFlush,
                              &Ctx);
  Owner.setMode(EJitCompileMode::Async);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  EXPECT_EQ(Owner.compileOrGet(304, nullptr, 0, codeFor(304)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_EQ(Owner.pollBudget(1), 1u);
  EXPECT_EQ(Owner.pendingPublishCount(), 0u);
  EXPECT_EQ(findReadySlot(304), nullptr);
}

} // namespace
