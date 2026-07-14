//===-- EJitIdentityHashTest.cpp - Unit tests for the identity hash ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  Tests the compile-identity hash (EJitSreQueue.h). An identity is
//  (funcIndex, numDims, dims[]) and callers take the low bits of the hash as a
//  cache bucket index, so every field must reach those low bits.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitSreQueue.h"
#include "gtest/gtest.h"
#include <set>

using namespace llvm::ejit;

namespace {

constexpr uint32_t kBuckets = 32;

uint64_t hash(uint32_t funcIndex, const EJitDimPair *dims, uint32_t numDims) {
  uint64_t key = ejitHashSeed(funcIndex, numDims);
  for (uint32_t i = 0; i < numDims; ++i)
    key = ejitHashDim(key, dims[i].dimType, dims[i].instanceId);
  return ejitFinalize64(key);
}

uint64_t hash1D(uint32_t funcIndex, uint32_t dimType, uint32_t instanceId) {
  const EJitDimPair d[1] = {{dimType, instanceId}};
  return hash(funcIndex, d, 1);
}

// Every identity here has funcIndex ^ instanceId == 3, which collapsed onto one
// key when funcIndex was folded in raw.
TEST(EJitIdentityHash, FuncIndexDoesNotCancelAgainstInstanceId) {
  std::set<uint64_t> keys = {hash1D(1, 0, 2), hash1D(2, 0, 1), hash1D(3, 0, 0),
                             hash1D(4, 0, 7)};
  EXPECT_EQ(keys.size(), 4u);
}

// dimType is folded into the high half, so without the avalanche it never
// reaches the low bits used as the bucket index.
TEST(EJitIdentityHash, DimTypeReachesTheBucketIndex) {
  std::set<uint32_t> buckets;
  for (uint32_t dt = 0; dt < 8; ++dt)
    buckets.insert(static_cast<uint32_t>(hash1D(1, dt, 2) % kBuckets));
  EXPECT_GT(buckets.size(), 1u);
}

TEST(EJitIdentityHash, NumDimsIsPartOfTheIdentity) {
  const EJitDimPair d1[1] = {{0, 5}};
  const EJitDimPair d2[2] = {{0, 5}, {1, 6}};
  EXPECT_NE(hash(7, nullptr, 0), hash(7, d1, 1));
  EXPECT_NE(hash(7, d1, 1), hash(7, d2, 2));
}

TEST(EJitIdentityHash, DimOrderMatters) {
  const EJitDimPair ab[2] = {{0, 1}, {1, 2}};
  const EJitDimPair ba[2] = {{1, 2}, {0, 1}};
  EXPECT_NE(hash(3, ab, 2), hash(3, ba, 2));
}

// Several entry functions over a couple of period types, many instances each:
// the shape the old hash collapsed. Every identity gets its own key.
TEST(EJitIdentityHash, DistinctKeysAndFullBucketCoverage) {
  std::set<uint64_t> keys;
  std::set<uint32_t> buckets;
  unsigned identities = 0;

  for (uint32_t funcIndex = 1; funcIndex <= 8; ++funcIndex) {
    for (uint32_t dimType = 0; dimType < 2; ++dimType) {
      for (uint32_t inst = 0; inst < 16; ++inst) {
        uint64_t k = hash1D(funcIndex, dimType, inst);
        keys.insert(k);
        buckets.insert(static_cast<uint32_t>(k % kBuckets));
        ++identities;
      }
    }
  }

  EXPECT_EQ(identities, 256u);
  EXPECT_EQ(keys.size(), identities);
  EXPECT_EQ(buckets.size(), kBuckets);
}

} // namespace
