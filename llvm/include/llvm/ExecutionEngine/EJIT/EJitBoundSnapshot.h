//===-- EJitBoundSnapshot.h - Owned bound-pointer snapshots --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITBOUNDSNAPSHOT_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITBOUNDSNAPSHOT_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#ifndef EJIT_FREESTANDING
#include <cstdlib>
#endif

namespace llvm {
namespace ejit {

/// A caller-owned input used only while building an owned snapshot.
struct EJitBoundPtrInput {
  const void *data = nullptr;
  uint32_t size = 0;
  uint32_t argIndex = 0;
};

/// A non-owning view used by the compiler and specialization pass. The view is
/// valid only while the compile callback is running.
struct EJitBoundPointerView {
  const uint8_t *data = nullptr;
  uint32_t size = 0;
  uint32_t argIndex = 0;
};

/// Fixed header of an owned snapshot. Entries and bytes follow in one
/// allocation. Offsets are relative to the byte area, never raw caller
/// pointers, so each entry has an independent copy and lifetime.
struct EJitBoundSnapshotEntry {
  uint32_t argIndex;
  uint32_t size;
  uint64_t offset;
};

using EJitBoundSnapshotAllocFn = void *(*)(void *ctx, size_t size);
using EJitBoundSnapshotFreeFn = void (*)(void *ctx, void *ptr);

/// Allocation hooks for snapshots that cross the producer/worker boundary.
/// The pair must come from an allocator that is valid in the process address
/// space, returns suitably aligned storage, and may be freed by the worker
/// core. The callbacks and ctx must remain valid until all queued snapshots
/// have been drained. Freestanding builds have no default allocator; the
/// platform must inject both callbacks before use.
struct EJitBoundSnapshotAllocator {
  EJitBoundSnapshotAllocFn alloc = nullptr;
  EJitBoundSnapshotFreeFn free = nullptr;
  void *ctx = nullptr;
};

#ifndef EJIT_FREESTANDING
inline void *ejitDefaultBoundSnapshotAlloc(void *, size_t size) {
  return std::malloc(size);
}

inline void ejitDefaultBoundSnapshotFree(void *, void *ptr) { std::free(ptr); }
#endif

inline EJitBoundSnapshotAllocator getDefaultEJitBoundSnapshotAllocator() {
#ifndef EJIT_FREESTANDING
  return {&ejitDefaultBoundSnapshotAlloc, &ejitDefaultBoundSnapshotFree,
          nullptr};
#else
  return {};
#endif
}

struct EJitBoundSnapshot {
  uint32_t entryCount;
  uint32_t reserved;
  uint64_t totalBytes;
  EJitBoundSnapshotFreeFn freeFn;
  void *freeCtx;
};

constexpr uint32_t kEJitBoundSnapshotMagic = 0x42534E50u; // "BSNP"
constexpr uint32_t kEJitMaxBoundPointers = 8;

inline bool ejitBoundSnapshotLayout(uint32_t entryCount, uint64_t totalBytes,
                                    size_t &entriesOffset, size_t &dataOffset,
                                    size_t &allocationSize) {
  constexpr size_t HeaderSize = sizeof(EJitBoundSnapshot);
  if constexpr (sizeof(size_t) < sizeof(uint64_t)) {
    const uint64_t maxEntries =
        (std::numeric_limits<size_t>::max() - HeaderSize) /
        sizeof(EJitBoundSnapshotEntry);
    if (static_cast<uint64_t>(entryCount) > maxEntries)
      return false;
  }
  entriesOffset = HeaderSize;
  dataOffset = entriesOffset +
               static_cast<size_t>(entryCount) * sizeof(EJitBoundSnapshotEntry);
  if (totalBytes > std::numeric_limits<size_t>::max() - dataOffset)
    return false;
  allocationSize = dataOffset + static_cast<size_t>(totalBytes);
  return true;
}

/// Allocate one owned snapshot and copy every input exactly once. A null
/// pointer with a non-zero size, arithmetic overflow, or allocation failure is
/// rejected without truncation.
inline EJitBoundSnapshot *
createEJitBoundSnapshot(const EJitBoundPtrInput *inputs, uint32_t inputCount,
                        EJitBoundSnapshotAllocator allocator =
                            getDefaultEJitBoundSnapshotAllocator()) {
  if (inputCount == 0 || inputCount > kEJitMaxBoundPointers || !inputs)
    return nullptr;
  if (!allocator.alloc || !allocator.free)
    return nullptr;

  uint64_t totalBytes = 0;
  for (uint32_t i = 0; i < inputCount; ++i) {
    // One formal argument must map to one snapshot. Rejecting duplicates keeps
    // malformed metadata/API input from making propagation order-dependent.
    for (uint32_t j = 0; j < i; ++j)
      if (inputs[j].argIndex == inputs[i].argIndex)
        return nullptr;
    if (inputs[i].size != 0 && !inputs[i].data)
      return nullptr;
    if (inputs[i].size > std::numeric_limits<uint64_t>::max() - totalBytes)
      return nullptr;
    totalBytes += inputs[i].size;
  }

  size_t entriesOffset = 0;
  size_t dataOffset = 0;
  size_t allocationSize = 0;
  if (!ejitBoundSnapshotLayout(inputCount, totalBytes, entriesOffset,
                               dataOffset, allocationSize))
    return nullptr;

  auto *snapshot = static_cast<EJitBoundSnapshot *>(
      allocator.alloc(allocator.ctx, allocationSize));
  if (!snapshot)
    return nullptr;
  snapshot->entryCount = inputCount;
  snapshot->reserved = kEJitBoundSnapshotMagic;
  snapshot->totalBytes = totalBytes;
  snapshot->freeFn = allocator.free;
  snapshot->freeCtx = allocator.ctx;

  auto *entries = reinterpret_cast<EJitBoundSnapshotEntry *>(
      reinterpret_cast<uint8_t *>(snapshot) + entriesOffset);
  auto *bytes = reinterpret_cast<uint8_t *>(snapshot) + dataOffset;
  uint64_t offset = 0;
  for (uint32_t i = 0; i < inputCount; ++i) {
    entries[i] = {inputs[i].argIndex, inputs[i].size, offset};
    if (inputs[i].size)
      std::memcpy(bytes + static_cast<size_t>(offset), inputs[i].data,
                  inputs[i].size);
    offset += inputs[i].size;
  }
  return snapshot;
}

inline void destroyEJitBoundSnapshot(EJitBoundSnapshot *snapshot) {
  if (!snapshot || snapshot->reserved != kEJitBoundSnapshotMagic ||
      !snapshot->freeFn)
    return;
  snapshot->freeFn(snapshot->freeCtx, snapshot);
}

inline bool isValidEJitBoundSnapshot(const EJitBoundSnapshot *snapshot) {
  if (!snapshot || snapshot->reserved != kEJitBoundSnapshotMagic ||
      snapshot->entryCount == 0 || snapshot->entryCount > kEJitMaxBoundPointers)
    return false;
  size_t entriesOffset = 0;
  size_t dataOffset = 0;
  size_t allocationSize = 0;
  if (!ejitBoundSnapshotLayout(snapshot->entryCount, snapshot->totalBytes,
                               entriesOffset, dataOffset, allocationSize))
    return false;
  (void)dataOffset;
  (void)allocationSize;
  const auto *entries = reinterpret_cast<const EJitBoundSnapshotEntry *>(
      reinterpret_cast<const uint8_t *>(snapshot) + entriesOffset);
  uint64_t previousEnd = 0;
  for (uint32_t i = 0; i < snapshot->entryCount; ++i) {
    const auto &entry = entries[i];
    if (entry.offset < previousEnd || entry.offset > snapshot->totalBytes ||
        entry.size > snapshot->totalBytes - entry.offset)
      return false;
    previousEnd = entry.offset + entry.size;
  }
  return previousEnd <= snapshot->totalBytes;
}

inline const EJitBoundSnapshotEntry *
getEJitBoundSnapshotEntries(const EJitBoundSnapshot *snapshot) {
  return reinterpret_cast<const EJitBoundSnapshotEntry *>(
      reinterpret_cast<const uint8_t *>(snapshot) + sizeof(EJitBoundSnapshot));
}

inline const uint8_t *
getEJitBoundSnapshotBytes(const EJitBoundSnapshot *snapshot) {
  return reinterpret_cast<const uint8_t *>(
      getEJitBoundSnapshotEntries(snapshot) + snapshot->entryCount);
}

inline EJitBoundSnapshot *getEJitBoundSnapshot(uintptr_t pointer) {
  return reinterpret_cast<EJitBoundSnapshot *>(pointer);
}

inline void destroyEJitBoundSnapshot(uintptr_t pointer) {
  destroyEJitBoundSnapshot(getEJitBoundSnapshot(pointer));
}

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITBOUNDSNAPSHOT_H
