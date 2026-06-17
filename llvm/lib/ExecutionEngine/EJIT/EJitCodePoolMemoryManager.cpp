//===-- EJitCodePoolMemoryManager.cpp - JITLink mem mgr over code pool ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitCodePoolMemoryManager.h"
#include "llvm/ExecutionEngine/JITLink/JITLink.h"
#include "llvm/ExecutionEngine/Orc/Shared/AllocationActions.h"
#include "llvm/Support/MathExtras.h"
#include <cstring>
#include <cstdint>

using namespace llvm;
using namespace llvm::ejit;
using namespace llvm::jitlink;

using orc::ExecutorAddr;
using WrapperFunctionCall = orc::shared::WrapperFunctionCall;

static void syncCodeForExecution(const void *Base, size_t Size) {
  if (!Base || Size == 0)
    return;

#if defined(__aarch64__)
  auto Begin = reinterpret_cast<uintptr_t>(Base);
  auto End = Begin + Size;

  uint64_t Ctr = 0;
  asm volatile("mrs %0, ctr_el0" : "=r"(Ctr));

  const uintptr_t DLine = uintptr_t{4} << ((Ctr >> 16) & 0xf);
  const uintptr_t ILine = uintptr_t{4} << (Ctr & 0xf);

  for (uintptr_t P = Begin & ~(DLine - 1); P < End; P += DLine)
    asm volatile("dc cvau, %0" : : "r"(P) : "memory");
  asm volatile("dsb sy" ::: "memory");

  for (uintptr_t P = Begin & ~(ILine - 1); P < End; P += ILine)
    asm volatile("ic ivau, %0" : : "r"(P) : "memory");
  asm volatile("dsb sy\nisb" ::: "memory");
#endif
}

/// Side record used as the FinalizedAlloc handle. Holds the dealloc actions and
/// the pool-backed base address. Pool memory itself is not freed in v1.
struct EJitCodePoolMemoryManager::FinalizedInfo {
  void *Base = nullptr;
  std::vector<WrapperFunctionCall> DeallocActions;
};

class EJitCodePoolMemoryManager::InFlightAllocImpl
    : public JITLinkMemoryManager::InFlightAlloc {
public:
  InFlightAllocImpl(EJitCodePoolManager &Pool, LinkGraph &G, BasicLayout BL,
                    void *Base, size_t Size)
      : Pool(&Pool), G(&G), BL(std::move(BL)), Base(Base), Size(Size) {}

  void finalize(OnFinalizedFunction OnFinalized) override {
    // The content has already been written into working memory, which (for an
    // in-process pool) is the executor memory. Seal the backing 2MiB pool
    // before running allocation finalizers: some JITLink actions may execute
    // target code before lookup() returns, so lookup-time sealing is too late.
    // The lookup path still performs an idempotent seal as a defensive fallback.
    if (Pool && Base) {
      if (auto Err = Pool->sealPoolContaining(Base)) {
        OnFinalized(std::move(Err));
        return;
      }
      syncCodeForExecution(Base, Size);
    }

    runFinalizeActions(
        G->allocActions(),
        [this, OnFinalized = std::move(OnFinalized)](
            Expected<std::vector<WrapperFunctionCall>> DeallocActions) mutable {
          if (!DeallocActions) {
            OnFinalized(DeallocActions.takeError());
            return;
          }
          auto *Info = new FinalizedInfo();
          Info->Base = Base;
          Info->DeallocActions = std::move(*DeallocActions);
#ifndef NDEBUG
          G = nullptr; // mark finalized
#endif
          OnFinalized(FinalizedAlloc(ExecutorAddr::fromPtr(Info)));
        });
  }

  void abandon(OnAbandonedFunction OnAbandoned) override {
    // v1 does not reclaim pool memory; just drop the in-flight state.
#ifndef NDEBUG
    G = nullptr;
#endif
    OnAbandoned(Error::success());
  }

private:
  EJitCodePoolManager *Pool;
  LinkGraph *G;
  BasicLayout BL;
  void *Base;
  size_t Size;
};

EJitCodePoolMemoryManager::EJitCodePoolMemoryManager(EJitCodePoolManager &Pool,
                                                     size_t PageSize)
    : Pool_(Pool), PageSize_(PageSize) {}

void EJitCodePoolMemoryManager::allocate(const JITLinkDylib *JD, LinkGraph &G,
                                         OnAllocatedFunction OnAllocated) {
  BasicLayout BL(G);

  auto SegsSizes = BL.getContiguousPageBasedLayoutSizes(PageSize_);
  if (!SegsSizes) {
    OnAllocated(SegsSizes.takeError());
    return;
  }

  uint64_t Total = SegsSizes->total();

  void *Slab = nullptr;
  if (Total > 0) {
    auto MemOrErr = Pool_.allocateCode(static_cast<size_t>(Total), PageSize_);
    if (!MemOrErr) {
      OnAllocated(MemOrErr.takeError());
      return;
    }
    Slab = *MemOrErr;
    // Zero-fill the whole slab up-front (covers zero-fill segments and any
    // inter-segment page padding).
    std::memset(Slab, 0, static_cast<size_t>(Total));
  }

  auto *SlabBytes = static_cast<char *>(Slab);
  auto NextStandardSegAddr = ExecutorAddr::fromPtr(SlabBytes);
  auto NextFinalizeSegAddr =
      ExecutorAddr::fromPtr(SlabBytes + SegsSizes->StandardSegs);

  for (auto &KV : BL.segments()) {
    auto &AG = KV.first;
    auto &Seg = KV.second;

    auto &SegAddr = (AG.getMemLifetime() == orc::MemLifetime::Standard)
                        ? NextStandardSegAddr
                        : NextFinalizeSegAddr;

    Seg.WorkingMem = SegAddr.toPtr<char *>();
    Seg.Addr = SegAddr;
    SegAddr += alignTo(Seg.ContentSize + Seg.ZeroFillSize, PageSize_);
  }

  if (auto Err = BL.apply()) {
    OnAllocated(std::move(Err));
    return;
  }

  OnAllocated(std::make_unique<InFlightAllocImpl>(Pool_, G, std::move(BL), Slab,
                                                  static_cast<size_t>(Total)));
}

void EJitCodePoolMemoryManager::deallocate(std::vector<FinalizedAlloc> Allocs,
                                           OnDeallocatedFunction OnDeallocated) {
  Error DeallocErr = Error::success();
  for (auto &Alloc : Allocs) {
    auto *Info = Alloc.release().toPtr<FinalizedInfo *>();
    // Run dealloc actions in reverse order. Pool memory is intentionally not
    // released in v1 (sealed/RX pages must not be recycled; see design doc).
    while (!Info->DeallocActions.empty()) {
      if (auto Err = Info->DeallocActions.back().runWithSPSRetErrorMerged())
        DeallocErr = joinErrors(std::move(DeallocErr), std::move(Err));
      Info->DeallocActions.pop_back();
    }
    delete Info;
  }
  OnDeallocated(std::move(DeallocErr));
}
