//===-- EJitCodePoolMemoryManager.h - JITLink mem mgr over code pool ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  A JITLink memory manager that backs all JIT segment memory with
//  EJitCodePoolManager's 2MiB pools instead of mmap/mprotect.
//
//  Unlike InProcessMemoryManager, finalize() does not apply per-segment memory
//  protections. Instead it seals the whole backing 2MiB pool through
//  EJitCodePoolManager before running JITLink allocation finalizers. This keeps
//  execute-permission flips at the required 2MiB granularity, avoids the W^X
//  conflict that wrapping mprotect causes, and still makes code executable for
//  any finalization action that runs before lookup() returns.
//
//  v1 does not reclaim pool memory on deallocate (only dealloc actions run);
//  pool lifetime equals the engine lifetime. See EJIT_SRE_CODE_POOL.md.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITCODEPOOLMEMORYMANAGER_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITCODEPOOLMEMORYMANAGER_H

#include "llvm/ExecutionEngine/EJIT/EJitCodePool.h"
#include "llvm/ExecutionEngine/JITLink/JITLinkMemoryManager.h"

namespace llvm {
namespace ejit {

/// JITLinkMemoryManager that allocates JIT segments from EJitCodePoolManager
/// and never applies per-segment page protections itself. The referenced pool
/// manager must outlive this object.
class EJitCodePoolMemoryManager : public jitlink::JITLinkMemoryManager {
public:
  EJitCodePoolMemoryManager(EJitCodePoolManager &Pool, size_t PageSize);

  void allocate(const jitlink::JITLinkDylib *JD, jitlink::LinkGraph &G,
                OnAllocatedFunction OnAllocated) override;

  void deallocate(std::vector<FinalizedAlloc> Allocs,
                  OnDeallocatedFunction OnDeallocated) override;

  // Bring in the convenience / blocking overloads hidden by the overrides.
  using JITLinkMemoryManager::allocate;
  using JITLinkMemoryManager::deallocate;

  EJitCodePoolManager &getPool() { return Pool_; }

private:
  class InFlightAllocImpl;
  struct FinalizedInfo;

  EJitCodePoolManager &Pool_;
  size_t PageSize_;
};

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITCODEPOOLMEMORYMANAGER_H
