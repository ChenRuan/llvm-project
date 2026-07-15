//===-- EJitSharedCacheQueryBench.cpp - cache-hit query microbenchmark ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  Deterministic, single-thread microbenchmark + slot-depth diagnostics for the
//  cross-core SHARED taskpool cache-hit query path (spec §5.2 steps 0-1). It is
//  NOT a gtest binary: it has its own main(), links only the shared-taskpool
//  sources + EJitSharedPlatform, and prints a machine-readable table so an
//  external harness (perf stat, this file's own cycle counter) can attribute
//  instructions/cycles per hit.
//
//  What it measures, per the audit requirements:
//    * 0D / 1D / 2D owner-core cache hit
//    * hit slot depth 0, 1, 5, 15 (the linear 16-slot bucket scan)
//    * peer-core hit (code sharing on, execute-permission already memoized)
//    * a slot-depth histogram for a synthetic mixed workload
//
//  Slot depth is controlled deterministically: 0D identities whose funcIndex is
//  a multiple of kEJitSharedCacheBuckets (32) all hash to bucket 0 and fill its
//  slots 0,1,2,... in publish order (cachePublish uses first-empty), so the Nth
//  published collider lands at slot N.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <thread>
#include <vector>

using namespace llvm::ejit;

namespace {

// Deterministic, non-null "compiled code" pointer derived from funcIndex. Never
// executed — only cached/compared/returned.
void *codeFor(uint32_t funcIndex) {
  return reinterpret_cast<void *>(0x100000ull +
                                  static_cast<uintptr_t>(funcIndex) * 64u);
}

bool benchCompile(void * /*ctx*/, const EJitCompileRequest &req, void **outFn) {
  *outFn = codeFor(req.funcIndex);
  return true;
}

// 0D cache-hit lookup selector. When funcOnly is set (only meaningful in an
// EJIT_BENCH_FUNCINDEX_ONLY_LOOKUP build) it drives the BENCHMARK ONLY /
// UNSAFE FOR GENERAL USE funcIndex-only fast path; otherwise the standard 0D
// fixed-dimension cache-hit path. Both return the same CompileOrGetResult
// shape, so callers stay identical.
inline EJitSharedTaskPool::CompileOrGetResult
lookup0D(EJitSharedTaskPool &pool, uint32_t funcIndex, bool funcOnly) {
#ifdef EJIT_BENCH_FUNCINDEX_ONLY_LOOKUP
  if (funcOnly)
    return pool.tryCacheHitFuncIndexOnly(funcIndex);
#else
  (void)funcOnly;
#endif
  return pool.tryCacheHit0D(funcIndex);
}

// Owner-side resolver: hand back a fixed 2MiB pool range so peer preparation
// (4K/2M) and codeStart/codeSize metadata are exercised.
struct RangeCtx {
  uintptr_t poolBase = 0x40000000ull;
  uint64_t poolSize = 0x200000ull;
  uintptr_t codeStart = 0x40000000ull;
  uint64_t codeSize = 64;
  uint32_t poolId = 0;
};
bool benchCodeRange(void *ctx, const void *fnPtr, EJitCompiledCodeInfo *out) {
  auto *r = static_cast<RangeCtx *>(ctx);
  out->fnPtr = const_cast<void *>(fnPtr);
  out->codeStart = r->codeStart;
  out->codeSize = r->codeSize;
  out->poolBase = r->poolBase;
  out->poolSize = r->poolSize;
  out->poolId = r->poolId;
  return true;
}

// Peer execute-permission preparation always succeeds in this host harness.
bool benchPrepareCode(void * /*ctx*/, const void * /*fnPtr*/) { return true; }

uint64_t nowNs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
         static_cast<uint64_t>(ts.tv_nsec);
}

// Global sink so the compiler cannot elide the looked-up pointer.
volatile uintptr_t gSink = 0;

struct Bench {
  std::unique_ptr<EJitSharedTaskPoolState> state;
  EJitSharedTaskPool pool;
  RangeCtx range;

  void bringUpOwner(bool codeSharing) {
    EJitCoreId::resetForTest();
    EJitCoreId::setCurrentForTest(0);
    state.reset(new EJitSharedTaskPoolState());
    pool.bind(state.get());
    pool.setCompiler(&benchCompile, nullptr);
    pool.setMode(EJitCompileMode::Async);
    pool.setCodeSharingEnabled(codeSharing);
    pool.setCodeRangeProvider(&benchCodeRange, &range);
    pool.setPrepareCodeCallback(&benchPrepareCode, nullptr);
    if (pool.init() != EJitSharedTaskPool::InitResult::BecameOwner) {
      std::fprintf(stderr, "owner election failed\n");
      std::exit(1);
    }
  }

  // Publish one 0D identity on the owner core (compile inline via Async enqueue
  // + pollOne).
  void publish0D(uint32_t funcIndex) {
    EJitCoreId::setCurrentForTest(0);
    pool.compileOrGet(funcIndex, nullptr, 0, codeFor(funcIndex));
    pool.pollOne();
  }
  void publish1D(uint32_t funcIndex, uint32_t d0, uint32_t i0) {
    EJitCoreId::setCurrentForTest(0);
    pool.setInstanceEnabled(d0, i0, true);
    EJitDimPair dims[1] = {{d0, i0}};
    pool.compileOrGet(funcIndex, dims, 1, codeFor(funcIndex));
    pool.pollOne();
  }
  void publish2D(uint32_t funcIndex, uint32_t d0, uint32_t i0, uint32_t d1,
                 uint32_t i1) {
    EJitCoreId::setCurrentForTest(0);
    pool.setInstanceEnabled(d0, i0, true);
    pool.setInstanceEnabled(d1, i1, true);
    EJitDimPair dims[2] = {{d0, i0}, {d1, i1}};
    pool.compileOrGet(funcIndex, dims, 2, codeFor(funcIndex));
    pool.pollOne();
  }
};

// --- 0D slot-depth sweep -----------------------------------------------------
// Publish (depth+1) 0D colliders into bucket 0, then hammer the deepest one.
uint64_t bench0D(uint32_t depth, uint64_t iters, bool peer, double *nsPerHit,
                 bool funcOnly = false) {
  Bench B;
  B.bringUpOwner(/*codeSharing=*/peer);
  const uint32_t kB = kEJitSharedCacheBuckets;
  uint32_t targetFunc = 0;
  for (uint32_t d = 0; d <= depth; ++d) {
    uint32_t fi = d * kB; // all hash to bucket 0
    B.publish0D(fi);
    targetFunc = fi; // deepest published so far
  }
  uint32_t core = 0;
  if (peer) {
    core = 1;
    EJitCoreId::setCurrentForTest(core);
    // Warm the memoized execute-permission bit for this peer core via the full
    // path (prepares), so the timed funcIndex-only / 0D path is a warm hit.
    auto w = B.pool.tryCacheHit0D(targetFunc);
    if (w.hasReadToken)
      B.pool.releaseRead(w.bucketIndex);
  }
  EJitCoreId::setCurrentForTest(core);
  uint64_t t0 = nowNs();
  uint64_t hits = 0;
  for (uint64_t i = 0; i < iters; ++i) {
    auto r = lookup0D(B.pool, targetFunc, funcOnly);
    gSink += reinterpret_cast<uintptr_t>(r.fnPtr);
    if (r.status == EJitCompileOrGetStatus::CacheHit)
      ++hits;
    if (r.hasReadToken)
      B.pool.releaseRead(r.bucketIndex);
  }
  uint64_t t1 = nowNs();
  *nsPerHit = double(t1 - t0) / double(iters);
  return hits;
}

uint64_t bench1D(uint32_t depth, uint64_t iters, double *nsPerHit,
                 bool funcOnly = false) {
  if (funcOnly && depth != 0) {
    std::fprintf(stderr,
                 "funcIndex-only 1D benchmark requires slotDepth=0: distinct "
                 "specializations deliberately poison the hint\n");
    std::exit(2);
  }
  Bench B;
  B.bringUpOwner(false);
  // 1D identities colliding into one bucket: vary funcIndex by multiples that
  // keep the same hash bucket is nontrivial, so instead vary instanceId and
  // accept the natural bucket; publish depth+1 entries and hammer the last.
  uint32_t targetFunc = 7;
  for (uint32_t d = 0; d <= depth; ++d) {
    B.publish1D(targetFunc, 0, d); // same funcIndex, distinct instanceId
  }
  // The deepest is instanceId=depth; ensure it is enabled and query it.
  EJitCoreId::setCurrentForTest(0);
  uint64_t t0 = nowNs();
  uint64_t hits = 0;
  for (uint64_t i = 0; i < iters; ++i) {
#ifdef EJIT_BENCH_FUNCINDEX_ONLY_LOOKUP
    auto r = funcOnly ? B.pool.tryFuncIndexOnlyDirect(targetFunc)
                      : B.pool.tryCacheHit1D(targetFunc, 0, depth);
#else
    (void)funcOnly;
    auto r = B.pool.tryCacheHit1D(targetFunc, 0, depth);
#endif
    gSink += reinterpret_cast<uintptr_t>(r.fnPtr);
    if (r.status == EJitCompileOrGetStatus::CacheHit)
      ++hits;
    if (r.hasReadToken)
      B.pool.releaseRead(r.bucketIndex);
  }
  uint64_t t1 = nowNs();
  *nsPerHit = double(t1 - t0) / double(iters);
  return hits;
}

uint64_t bench2D(uint64_t iters, double *nsPerHit, bool funcOnly = false) {
  Bench B;
  B.bringUpOwner(false);
  uint32_t targetFunc = 9;
  B.publish2D(targetFunc, 0, 1, 1, 2);
  EJitCoreId::setCurrentForTest(0);
  uint64_t t0 = nowNs();
  uint64_t hits = 0;
  for (uint64_t i = 0; i < iters; ++i) {
#ifdef EJIT_BENCH_FUNCINDEX_ONLY_LOOKUP
    auto r = funcOnly ? B.pool.tryFuncIndexOnlyDirect(targetFunc)
                      : B.pool.tryCacheHit2D(targetFunc, 0, 1, 1, 2);
#else
    (void)funcOnly;
    auto r = B.pool.tryCacheHit2D(targetFunc, 0, 1, 1, 2);
#endif
    gSink += reinterpret_cast<uintptr_t>(r.fnPtr);
    if (r.status == EJitCompileOrGetStatus::CacheHit)
      ++hits;
    if (r.hasReadToken)
      B.pool.releaseRead(r.bucketIndex);
  }
  uint64_t t1 = nowNs();
  *nsPerHit = double(t1 - t0) / double(iters);
  return hits;
}

struct alignas(64) ThreadResult {
  uint64_t elapsedNs = 0;
  uint64_t hits = 0;
  uintptr_t sink = 0;
};

struct alignas(64) StartGate {
  std::atomic<uint32_t> ready{0};
  std::atomic<bool> go{false};
};

// Real-thread contention test. In sameIdentity mode every thread reads the
// same slot in bucket 0, mirroring many SRE cores calling one hot
// specialization. In spread mode each thread reads a different 0D identity in
// a different bucket, isolating thread/runtime overhead from bucket-line
// contention. Each thread has its own sink/result cache line so the harness
// does not add an unrelated shared-write bottleneck.
void benchMultiThread0D(uint32_t threadCount, uint64_t iters, bool sameIdentity,
                        uint32_t depth, bool funcOnly = false) {
  if (threadCount == 0 || threadCount > 63 || depth >= kEJitSharedCacheSlots) {
    std::fprintf(stderr, "invalid threads/depth: threads=%u depth=%u\n",
                 threadCount, depth);
    std::exit(2);
  }
  if (!sameIdentity && threadCount >= kEJitSharedCacheBuckets) {
    std::fprintf(stderr, "spread mode needs fewer than %u threads\n",
                 kEJitSharedCacheBuckets);
    std::exit(2);
  }

  Bench B;
  B.bringUpOwner(/*codeSharing=*/true);
  std::vector<uint32_t> targetFuncs(threadCount);
  if (sameIdentity) {
    uint32_t target = 0;
    for (uint32_t d = 0; d <= depth; ++d) {
      target = d * kEJitSharedCacheBuckets;
      B.publish0D(target);
    }
    std::fill(targetFuncs.begin(), targetFuncs.end(), target);
  } else {
    for (uint32_t t = 0; t < threadCount; ++t) {
      targetFuncs[t] = t + 1u; // buckets 1..threadCount for <=31 threads.
      B.publish0D(targetFuncs[t]);
    }
  }

  StartGate gate;
  std::unique_ptr<ThreadResult[]> results(new ThreadResult[threadCount]);
  std::vector<std::thread> threads;
  threads.reserve(threadCount);

  for (uint32_t t = 0; t < threadCount; ++t) {
    threads.emplace_back([&, t] {
      // Core 0 owns the pool. Host workers model peer cores 1..threadCount.
      EJitCoreId::setCurrentForTest(t + 1u);
      const uint32_t target = targetFuncs[t];

      // Prepare/memoize this peer's execute permission before timing via the
      // full path (which prepares), so the timed loop is a warm hit.
      auto warm = B.pool.tryCacheHit0D(target);
      if (warm.hasReadToken)
        B.pool.releaseRead(warm.bucketIndex);
      bool warmOk = warm.status == EJitCompileOrGetStatus::CacheHit;

      gate.ready.fetch_add(1, std::memory_order_release);
      while (!gate.go.load(std::memory_order_acquire))
        std::this_thread::yield();
      if (!warmOk)
        return;

      uint64_t hits = 0;
      uintptr_t sink = 0;
      uint64_t begin = nowNs();
      for (uint64_t i = 0; i < iters; ++i) {
        auto r = lookup0D(B.pool, target, funcOnly);
        sink += reinterpret_cast<uintptr_t>(r.fnPtr);
        if (r.status == EJitCompileOrGetStatus::CacheHit)
          ++hits;
        if (r.hasReadToken)
          B.pool.releaseRead(r.bucketIndex);
      }
      results[t].elapsedNs = nowNs() - begin;
      results[t].hits = hits;
      results[t].sink = sink;
    });
  }

  while (gate.ready.load(std::memory_order_acquire) != threadCount)
    std::this_thread::yield();
  uint64_t wallBegin = nowNs();
  gate.go.store(true, std::memory_order_release);
  for (std::thread &T : threads)
    T.join();
  uint64_t wallNs = nowNs() - wallBegin;

  uint64_t totalHits = 0;
  uintptr_t sink = 0;
  std::vector<double> perThreadNs;
  perThreadNs.reserve(threadCount);
  for (uint32_t t = 0; t < threadCount; ++t) {
    totalHits += results[t].hits;
    sink ^= results[t].sink;
    perThreadNs.push_back(double(results[t].elapsedNs) / double(iters));
  }
  std::sort(perThreadNs.begin(), perThreadNs.end());
  double median = perThreadNs[threadCount / 2u];
  double throughputMHits =
      double(totalHits) * 1000.0 / static_cast<double>(wallNs);

#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  const char *discipline = "noreclaim-seqlock";
#else
  const char *discipline = "read-token";
#endif
  if (funcOnly)
    discipline = "funcindex-only";
  std::printf("mt0d mode=%s discipline=%s threads=%u depth=%u "
              "iters/thread=%llu min=%.3f median=%.3f max=%.3f ns/hit "
              "throughput=%.3f Mhit/s hits=%llu/%llu sink=%llu\n",
              sameIdentity ? "same" : "spread", discipline, threadCount,
              sameIdentity ? depth : 0u, (unsigned long long)iters,
              perThreadNs.front(), median, perThreadNs.back(), throughputMHits,
              (unsigned long long)totalHits,
              (unsigned long long)(iters * threadCount),
              (unsigned long long)sink);
}

void runTable(uint64_t iters) {
  std::printf("# config,dim,slotDepth,core,nsPerHit,hits/iters\n");
  double ns;
  uint64_t h;
  for (uint32_t depth : {0u, 1u, 5u, 15u}) {
    h = bench0D(depth, iters, /*peer=*/false, &ns);
    std::printf("owner0D,0,%u,0,%.3f,%llu/%llu\n", depth, ns,
                (unsigned long long)h, (unsigned long long)iters);
  }
  for (uint32_t depth : {0u, 1u, 5u, 15u}) {
    h = bench0D(depth, iters, /*peer=*/true, &ns);
    std::printf("peer0D,0,%u,1,%.3f,%llu/%llu\n", depth, ns,
                (unsigned long long)h, (unsigned long long)iters);
  }
  for (uint32_t depth : {0u, 1u, 5u, 15u}) {
    h = bench1D(depth, iters, &ns);
    std::printf("owner1D,1,%u,0,%.3f,%llu/%llu\n", depth, ns,
                (unsigned long long)h, (unsigned long long)iters);
  }
  h = bench2D(iters, &ns);
  std::printf("owner2D,2,0,0,%.3f,%llu/%llu\n", ns, (unsigned long long)h,
              (unsigned long long)iters);
}

} // namespace

// Usage:
//   bench                 -> full table, 20M iters each
//   bench <iters>         -> full table with given iters
//   bench single0d <d> <iters>   -> ONE tight loop (for perf stat attribution)
//   bench single0dpeer <d> <iters>
//   bench single1d <d> <iters>
//   bench single2d <iters>
//   bench mt0dsame <iters/thread> [threads=16] [slotDepth=0]
//   bench mt0dspread <iters/thread> [threads=16]
// BENCHMARK ONLY / UNSAFE FOR GENERAL USE (only in an
// EJIT_BENCH_FUNCINDEX_ONLY_LOOKUP build):
//   bench single0dfunconly <d> <iters>
//   bench single0dpeerfunconly <d> <iters>
//   bench single1dfunconly <iters>
//   bench single2dfunconly <iters>
//   bench mt0dsamefunconly <iters/thread> [threads=16] [slotDepth=0]
//   bench mt0dspreadfunconly <iters/thread> [threads=16]
int main(int argc, char **argv) {
  uint64_t iters = 20000000ull;
  if (argc >= 2 && std::strncmp(argv[1], "mt0d", 4) == 0) {
    iters = argc >= 3 ? strtoull(argv[2], nullptr, 10) : 1000000ull;
    uint32_t threadCount =
        argc >= 4 ? (uint32_t)strtoul(argv[3], nullptr, 10) : 16u;
    if (std::strcmp(argv[1], "mt0dsame") == 0) {
      uint32_t depth = argc >= 5 ? (uint32_t)strtoul(argv[4], nullptr, 10) : 0u;
      benchMultiThread0D(threadCount, iters, /*sameIdentity=*/true, depth);
    } else if (std::strcmp(argv[1], "mt0dspread") == 0) {
      benchMultiThread0D(threadCount, iters, /*sameIdentity=*/false, 0);
    } else if (std::strcmp(argv[1], "mt0dsamefunconly") == 0) {
#ifdef EJIT_BENCH_FUNCINDEX_ONLY_LOOKUP
      uint32_t depth = argc >= 5 ? (uint32_t)strtoul(argv[4], nullptr, 10) : 0u;
      benchMultiThread0D(threadCount, iters, /*sameIdentity=*/true, depth,
                         /*funcOnly=*/true);
#else
      std::fprintf(stderr, "mt0dsamefunconly needs "
                           "EJIT_BENCH_FUNCINDEX_ONLY_LOOKUP\n");
      return 2;
#endif
    } else if (std::strcmp(argv[1], "mt0dspreadfunconly") == 0) {
#ifdef EJIT_BENCH_FUNCINDEX_ONLY_LOOKUP
      benchMultiThread0D(threadCount, iters, /*sameIdentity=*/false, 0,
                         /*funcOnly=*/true);
#else
      std::fprintf(stderr, "mt0dspreadfunconly needs "
                           "EJIT_BENCH_FUNCINDEX_ONLY_LOOKUP\n");
      return 2;
#endif
    } else {
      std::fprintf(stderr, "unknown multi-thread mode %s\n", argv[1]);
      return 2;
    }
    return 0;
  }
  if (argc >= 2 && std::strncmp(argv[1], "single", 6) == 0) {
    double ns;
    uint64_t h = 0;
    const char *what = argv[1];
    if (std::strcmp(what, "single0d") == 0) {
      uint32_t d = argc >= 3 ? (uint32_t)strtoul(argv[2], nullptr, 10) : 0;
      iters = argc >= 4 ? strtoull(argv[3], nullptr, 10) : iters;
      h = bench0D(d, iters, false, &ns);
    } else if (std::strcmp(what, "single0dpeer") == 0) {
      uint32_t d = argc >= 3 ? (uint32_t)strtoul(argv[2], nullptr, 10) : 0;
      iters = argc >= 4 ? strtoull(argv[3], nullptr, 10) : iters;
      h = bench0D(d, iters, true, &ns);
    } else if (std::strcmp(what, "single1d") == 0) {
      uint32_t d = argc >= 3 ? (uint32_t)strtoul(argv[2], nullptr, 10) : 0;
      iters = argc >= 4 ? strtoull(argv[3], nullptr, 10) : iters;
      h = bench1D(d, iters, &ns);
    } else if (std::strcmp(what, "single2d") == 0) {
      iters = argc >= 3 ? strtoull(argv[2], nullptr, 10) : iters;
      h = bench2D(iters, &ns);
    } else if (std::strcmp(what, "single0dfunconly") == 0) {
#ifdef EJIT_BENCH_FUNCINDEX_ONLY_LOOKUP
      uint32_t d = argc >= 3 ? (uint32_t)strtoul(argv[2], nullptr, 10) : 0;
      iters = argc >= 4 ? strtoull(argv[3], nullptr, 10) : iters;
      h = bench0D(d, iters, /*peer=*/false, &ns, /*funcOnly=*/true);
#else
      std::fprintf(stderr, "single0dfunconly needs "
                           "EJIT_BENCH_FUNCINDEX_ONLY_LOOKUP\n");
      return 2;
#endif
    } else if (std::strcmp(what, "single0dpeerfunconly") == 0) {
#ifdef EJIT_BENCH_FUNCINDEX_ONLY_LOOKUP
      uint32_t d = argc >= 3 ? (uint32_t)strtoul(argv[2], nullptr, 10) : 0;
      iters = argc >= 4 ? strtoull(argv[3], nullptr, 10) : iters;
      h = bench0D(d, iters, /*peer=*/true, &ns, /*funcOnly=*/true);
#else
      std::fprintf(stderr, "single0dpeerfunconly needs "
                           "EJIT_BENCH_FUNCINDEX_ONLY_LOOKUP\n");
      return 2;
#endif
    } else if (std::strcmp(what, "single1dfunconly") == 0) {
#ifdef EJIT_BENCH_FUNCINDEX_ONLY_LOOKUP
      iters = argc >= 3 ? strtoull(argv[2], nullptr, 10) : iters;
      h = bench1D(/*depth=*/0, iters, &ns, /*funcOnly=*/true);
#else
      std::fprintf(stderr, "single1dfunconly needs "
                           "EJIT_BENCH_FUNCINDEX_ONLY_LOOKUP\n");
      return 2;
#endif
    } else if (std::strcmp(what, "single2dfunconly") == 0) {
#ifdef EJIT_BENCH_FUNCINDEX_ONLY_LOOKUP
      iters = argc >= 3 ? strtoull(argv[2], nullptr, 10) : iters;
      h = bench2D(iters, &ns, /*funcOnly=*/true);
#else
      std::fprintf(stderr, "single2dfunconly needs "
                           "EJIT_BENCH_FUNCINDEX_ONLY_LOOKUP\n");
      return 2;
#endif
    } else {
      std::fprintf(stderr, "unknown single mode %s\n", what);
      return 2;
    }
    std::printf("%s ns/hit=%.3f hits=%llu/%llu sink=%llu\n", what, ns,
                (unsigned long long)h, (unsigned long long)iters,
                (unsigned long long)gSink);
    return 0;
  }
  if (argc >= 2)
    iters = strtoull(argv[1], nullptr, 10);
  runTable(iters);
  std::printf("# sink=%llu\n", (unsigned long long)gSink);
  return 0;
}
