//===-- EJitHotPathBench.cpp - EJIT post-compile hot-path benchmark -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  End-to-end microbenchmark for the EmbeddedJIT post-compile hot path (the
//  "wrapper cache hit" the business sees as AOT ~9us vs JIT wrapper ~15us). It
//  has its own main() and links only the shared-taskpool sources + platform +
//  the faithful C-ABI/wrapper TU (EJitHotPathBenchAbi.cpp), so it builds and
//  runs on the host without the full LLVM/ORC graph.
//
//  Three measurement tiers, kept strictly separate (the audit brief forbids
//  passing an internal tryCacheHit measurement off as a C-ABI or wrapper
//  number):
//
//    [internal]  tryCacheHit0D/1D/2D/3D/4D + releaseRead, owner & peer, slot
//                depth 0/1/5/15 — the cache-query cost in isolation.
//    [cabi]      the real same-ABI C shell bench_cabi_0d/1d/2d/generic — the
//                cost the wrapper pays to cross into the runtime.
//    [wrapper]   bench_wrapper_0d_* — full jit_entry -> C ABI -> indirect call
//                -> release -> return, plus AOT/direct baselines.
//
//  Method: warm up, then time B batches of K iterations each (B*K >= the
//  requested total, default 4,000,000). Report mean, min, p50, p95, p99 of the
//  per-batch ns/iter AND cycles/iter (rdtsc on x86), so a stable distribution
//  is shown without per-call timer cost inside the loop. Two calibration rows
//  (empty loop, timer/rdtsc self-cost) are printed so every number can be read
//  net of measurement overhead. A volatile sink defeats dead-code elimination.
//
//===----------------------------------------------------------------------===//

#include "EJitHotPathBenchAbi.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <vector>

using namespace llvm::ejit;
using namespace ejitbench;

namespace {

volatile uint64_t gSink = 0;

uint64_t nowNs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
         static_cast<uint64_t>(ts.tv_nsec);
}

// Free-running counter for a secondary per-op tick figure. On x86 this is the
// TSC; on aarch64 it is the fixed-frequency virtual counter CNTVCT_EL0 (the
// same source the VDSO clock_gettime uses), reported as "ticks" — authoritative
// instructions/cycles come from `perf stat` around the `single` mode below.
#if defined(__x86_64__)
static inline uint64_t rdcycle() {
  uint32_t lo, hi, aux;
  __asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
  return (static_cast<uint64_t>(hi) << 32) | lo;
}
#define HAVE_CYCLES 1
#elif defined(__aarch64__)
static inline uint64_t rdcycle() {
  uint64_t v;
  __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
  return v;
}
#define HAVE_CYCLES 1
#else
static inline uint64_t rdcycle() { return 0; }
#define HAVE_CYCLES 0
#endif

struct Stat {
  double meanNs = 0, mnNs = 0, p50Ns = 0, p95Ns = 0, p99Ns = 0;
  double meanCyc = 0, p50Cyc = 0, p99Cyc = 0;
};

double pct(std::vector<double> &v, double p) {
  if (v.empty())
    return 0;
  size_t i = static_cast<size_t>(p * (v.size() - 1));
  return v[i];
}

// Time `body(i)` over B batches of K iters. body must fold something into
// gSink.
template <class F> Stat measure(uint64_t totalIters, F &&body) {
  const uint64_t K = 4000;
  uint64_t B = totalIters / K;
  if (B < 250)
    B = 250;
  // Warmup.
  for (uint64_t i = 0; i < K; ++i)
    body(i);
  std::vector<double> ns(B), cyc(B);
  for (uint64_t b = 0; b < B; ++b) {
    uint64_t c0 = rdcycle();
    uint64_t t0 = nowNs();
    for (uint64_t i = 0; i < K; ++i)
      body(b * K + i);
    uint64_t t1 = nowNs();
    uint64_t c1 = rdcycle();
    ns[b] = double(t1 - t0) / double(K);
    cyc[b] = double(c1 - c0) / double(K);
  }
  std::sort(ns.begin(), ns.end());
  std::sort(cyc.begin(), cyc.end());
  Stat s;
  double sum = 0, sumc = 0;
  for (double x : ns)
    sum += x;
  for (double x : cyc)
    sumc += x;
  s.meanNs = sum / B;
  s.mnNs = ns.front();
  s.p50Ns = pct(ns, 0.50);
  s.p95Ns = pct(ns, 0.95);
  s.p99Ns = pct(ns, 0.99);
  s.meanCyc = sumc / B;
  s.p50Cyc = pct(cyc, 0.50);
  s.p99Cyc = pct(cyc, 0.99);
  return s;
}

void row(const char *tier, const char *name, const Stat &s) {
  std::printf("%-9s %-26s mean=%7.2fns p50=%7.2f p95=%7.2f p99=%7.2f min=%7.2f "
              "| cyc mean=%7.1f p50=%7.1f p99=%7.1f\n",
              tier, name, s.meanNs, s.p50Ns, s.p95Ns, s.p99Ns, s.mnNs,
              s.meanCyc, s.p50Cyc, s.p99Cyc);
}

//===-- Pool bring-up -----------------------------------------------------===//
struct RangeCtx {
  uintptr_t poolBase = 0x40000000ull;
  uint64_t poolSize = 0x200000ull;
  uintptr_t codeStart = 0x40000000ull;
  uint64_t codeSize = 64;
  uint32_t poolId = 0;
};

// The compiler callback publishes the REAL executable JIT function address, so
// the wrapper's indirect call actually runs it.
bool benchCompile(void * /*ctx*/, const EJitCompileRequest & /*req*/,
                  void **outFn) {
  *outFn = benchJitFn0D();
  return true;
}
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
bool benchPrepareCode(void * /*ctx*/, const void * /*fnPtr*/) { return true; }

struct Bench {
  std::unique_ptr<EJitSharedTaskPoolState> state;
  EJitSharedTaskPool pool;
  RangeCtx range;
  EJitStub stub;

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
    stub.sharedPool = &pool;
    gEJITStub = &stub;
  }
  void publish0D(uint32_t fi) {
    EJitCoreId::setCurrentForTest(0);
    pool.compileOrGet(fi, nullptr, 0, benchJitFn0D());
    pool.pollOne();
  }
  void publish1D(uint32_t fi, uint32_t d0, uint32_t i0) {
    EJitCoreId::setCurrentForTest(0);
    pool.setInstanceEnabled(d0, i0, true);
    EJitDimPair dims[1] = {{d0, i0}};
    pool.compileOrGet(fi, dims, 1, benchJitFn0D());
    pool.pollOne();
  }
  void publish2D(uint32_t fi, uint32_t d0, uint32_t i0, uint32_t d1,
                 uint32_t i1) {
    EJitCoreId::setCurrentForTest(0);
    pool.setInstanceEnabled(d0, i0, true);
    pool.setInstanceEnabled(d1, i1, true);
    EJitDimPair dims[2] = {{d0, i0}, {d1, i1}};
    pool.compileOrGet(fi, dims, 2, benchJitFn0D());
    pool.pollOne();
  }
};

const uint32_t kB = kEJitSharedCacheBuckets;

//===-- Tier 1: internal cache-query --------------------------------------===//
// 0D depth sweep: publish depth+1 colliders into bucket 0, hammer the deepest.
void internal0D(uint64_t iters, uint32_t depth, bool peer) {
  Bench B;
  B.bringUpOwner(peer);
  uint32_t target = 0;
  for (uint32_t d = 0; d <= depth; ++d) {
    B.publish0D(d * kB);
    target = d * kB;
  }
  uint32_t core = 0;
  if (peer) {
    core = 1;
    EJitCoreId::setCurrentForTest(core);
    auto w = B.pool.tryCacheHit0D(target);
    if (w.hasReadToken)
      B.pool.releaseRead(w.bucketIndex);
  }
  EJitCoreId::setCurrentForTest(core);
  Stat s = measure(iters, [&](uint64_t) {
    auto r = B.pool.tryCacheHit0D(target);
    gSink += reinterpret_cast<uintptr_t>(r.fnPtr);
    if (r.hasReadToken)
      B.pool.releaseRead(r.bucketIndex);
  });
  char nm[64];
  std::snprintf(nm, sizeof nm, "%s0D slot%u", peer ? "peer" : "owner", depth);
  row("internal", nm, s);
}

void internal1D(uint64_t iters) {
  Bench B;
  B.bringUpOwner(false);
  B.publish1D(7, 0, 3);
  EJitCoreId::setCurrentForTest(0);
  Stat s = measure(iters, [&](uint64_t) {
    auto r = B.pool.tryCacheHit1D(7, 0, 3);
    gSink += reinterpret_cast<uintptr_t>(r.fnPtr);
    if (r.hasReadToken)
      B.pool.releaseRead(r.bucketIndex);
  });
  row("internal", "owner1D", s);
}

void internal2D(uint64_t iters) {
  Bench B;
  B.bringUpOwner(false);
  B.publish2D(9, 0, 1, 1, 2);
  EJitCoreId::setCurrentForTest(0);
  Stat s = measure(iters, [&](uint64_t) {
    auto r = B.pool.tryCacheHit2D(9, 0, 1, 1, 2);
    gSink += reinterpret_cast<uintptr_t>(r.fnPtr);
    if (r.hasReadToken)
      B.pool.releaseRead(r.bucketIndex);
  });
  row("internal", "owner2D", s);
}

//===-- Tier 2: C ABI -----------------------------------------------------===//
void cabi0D(uint64_t iters) {
  Bench B;
  B.bringUpOwner(false);
  B.publish0D(0);
  EJitCoreId::setCurrentForTest(0);
  Stat s = measure(iters, [&](uint64_t) {
    void *fn;
    uint32_t bk;
    uint32_t st = bench_cabi_0d(0, &fn, &bk);
    gSink += reinterpret_cast<uintptr_t>(fn) + st;
    if (st == BENCH_OK && bk < kB)
      bench_cabi_release_read(bk);
  });
  row("cabi", "cabi_0d", s);
}
void cabi1D(uint64_t iters) {
  Bench B;
  B.bringUpOwner(false);
  B.publish1D(7, 0, 3);
  EJitCoreId::setCurrentForTest(0);
  Stat s = measure(iters, [&](uint64_t) {
    void *fn;
    uint32_t bk;
    uint32_t st = bench_cabi_1d(7, 0, 3, &fn, &bk);
    gSink += reinterpret_cast<uintptr_t>(fn) + st;
    if (st == BENCH_OK && bk < kB)
      bench_cabi_release_read(bk);
  });
  row("cabi", "cabi_1d", s);
}
void cabi2D(uint64_t iters) {
  Bench B;
  B.bringUpOwner(false);
  B.publish2D(9, 0, 1, 1, 2);
  EJitCoreId::setCurrentForTest(0);
  Stat s = measure(iters, [&](uint64_t) {
    void *fn;
    uint32_t bk;
    uint32_t st = bench_cabi_2d(9, 0, 1, 1, 2, &fn, &bk);
    gSink += reinterpret_cast<uintptr_t>(fn) + st;
    if (st == BENCH_OK && bk < kB)
      bench_cabi_release_read(bk);
  });
  row("cabi", "cabi_2d", s);
}
void cabiGeneric3D(uint64_t iters) {
  Bench B;
  B.bringUpOwner(false);
  EJitCoreId::setCurrentForTest(0);
  B.pool.setInstanceEnabled(0, 1, true);
  B.pool.setInstanceEnabled(1, 2, true);
  B.pool.setInstanceEnabled(2, 3, true);
  EJitDimPair d3[3] = {{0, 1}, {1, 2}, {2, 3}};
  B.pool.compileOrGet(11, d3, 3, benchJitFn0D());
  B.pool.pollOne();
  Stat s = measure(iters, [&](uint64_t) {
    void *fn;
    uint32_t bk;
    uint32_t st = bench_cabi_generic(11, d3, 3, &fn, &bk);
    gSink += reinterpret_cast<uintptr_t>(fn) + st;
    if (st == BENCH_OK && bk < kB)
      bench_cabi_release_read(bk);
  });
  row("cabi", "cabi_generic3d", s);
}

//===-- Tier 3: wrapper end-to-end + baselines ----------------------------===//
void wrapperE2E(uint64_t iters) {
  Bench B;
  B.bringUpOwner(false);
  B.publish0D(0); // funcIndex 0 == kWrapperFuncIndex0D
  EJitCoreId::setCurrentForTest(0);

  // Calibration: empty loop and timer self-cost.
  Stat emptyS = measure(iters, [&](uint64_t i) { gSink += i; });
  row("baseline", "empty_loop", emptyS);

  // Direct AOT body (the "AOT ~9us" analogue: no wrapper, no cache).
  Stat aotS =
      measure(iters, [&](uint64_t i) { gSink += bench_aot_0d((long)i); });
  row("baseline", "direct_aot", aotS);

  // Direct call of the same JIT function through an opaque pointer (isolates
  // the indirect-call cost the wrapper's jit_dispatch pays).
  volatile auto fp = reinterpret_cast<long (*)(long)>(benchJitFn0D());
  Stat fpS = measure(iters, [&](uint64_t i) {
    auto f = fp;
    gSink += f((long)i);
  });
  row("baseline", "direct_jit_fnptr", fpS);

  // Wrapper: cache hit -> indirect JIT call -> release -> return.
  Stat whS = measure(
      iters, [&](uint64_t i) { gSink += bench_wrapper_0d_hit((long)i); });
  row("wrapper", "wrapper_hit", whS);

  // Wrapper: forced AOT fallback (miss -> AOT body).
  Stat wfS = measure(
      iters, [&](uint64_t i) { gSink += bench_wrapper_0d_fallback((long)i); });
  row("wrapper", "wrapper_fallback", wfS);

  // Wrapper via the trusted fast ABI (optimization candidate).
  Stat wtS = measure(
      iters, [&](uint64_t i) { gSink += bench_wrapper_0d_trusted((long)i); });
  row("wrapper", "wrapper_hit_trusted", wtS);
}

// Tight single-workload loops for `perf stat` attribution. No timing calls, no
// other tiers — run `perf stat -e instructions,cycles ./bench single <name> N`
// and subtract the `empty` run to get net instructions/cycles per op.
int runSingle(const char *name, uint64_t iters) {
  Bench B;
  B.bringUpOwner(false);
  B.publish0D(0);
  B.publish1D(7, 0, 3);
  B.publish2D(9, 0, 1, 1, 2);
  EJitCoreId::setCurrentForTest(0);
  uint64_t acc = 0;
  if (!std::strcmp(name, "empty")) {
    for (uint64_t i = 0; i < iters; ++i)
      acc += i;
  } else if (!std::strcmp(name, "internal0d")) {
    for (uint64_t i = 0; i < iters; ++i) {
      auto r = B.pool.tryCacheHit0D(0);
      acc += reinterpret_cast<uintptr_t>(r.fnPtr);
      if (r.hasReadToken)
        B.pool.releaseRead(r.bucketIndex);
    }
  } else if (!std::strcmp(name, "cabi0d")) {
    for (uint64_t i = 0; i < iters; ++i) {
      void *fn;
      uint32_t bk;
      uint32_t st = bench_cabi_0d(0, &fn, &bk);
      acc += reinterpret_cast<uintptr_t>(fn) + st;
      if (st == BENCH_OK && bk < kB)
        bench_cabi_release_read(bk);
    }
  } else if (!std::strcmp(name, "direct_fnptr")) {
    volatile auto fp = reinterpret_cast<long (*)(long)>(benchJitFn0D());
    for (uint64_t i = 0; i < iters; ++i) {
      auto f = fp;
      acc += f((long)i);
    }
  } else if (!std::strcmp(name, "wrapper_hit")) {
    for (uint64_t i = 0; i < iters; ++i)
      acc += bench_wrapper_0d_hit((long)i);
  } else if (!std::strcmp(name, "wrapper_trusted")) {
    for (uint64_t i = 0; i < iters; ++i)
      acc += bench_wrapper_0d_trusted((long)i);
  } else if (!std::strcmp(name, "wrapper_fallback")) {
    for (uint64_t i = 0; i < iters; ++i)
      acc += bench_wrapper_0d_fallback((long)i);
  } else {
    std::fprintf(stderr, "unknown single workload '%s'\n", name);
    return 2;
  }
  gSink += acc;
  std::fprintf(stderr, "single %s iters=%llu sink=%llu\n", name,
               (unsigned long long)iters, (unsigned long long)gSink);
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc >= 2 && !std::strcmp(argv[1], "single")) {
    const char *name = argc >= 3 ? argv[2] : "empty";
    uint64_t n = argc >= 4 ? strtoull(argv[3], nullptr, 10) : 50000000ull;
    return runSingle(name, n);
  }
  uint64_t iters = 4000000ull;
  if (argc >= 2)
    iters = strtoull(argv[1], nullptr, 10);

#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  const char *cfg = "NO_RECLAIM+SHARED_CODE_POINTERS (seqlock, no token)";
#else
  const char *cfg = "token (per-hit RMW read lock)";
#endif
#ifdef EJIT_STATS_ENABLE
  const char *stats = "STATS=ON";
#else
  const char *stats = "STATS=OFF";
#endif
  std::printf("# EJIT hot-path bench  config=%s  %s  iters=%llu  buckets=%u  "
              "cycles=%d\n",
              cfg, stats, (unsigned long long)iters, kB, HAVE_CYCLES);
  std::printf("# tier      name                       ns distribution "
              "                       | cycles\n");

  // Tier 1: internal cache-query, owner slot depth sweep + peer + 1D/2D.
  for (uint32_t d : {0u, 1u, 5u, 15u})
    internal0D(iters, d, /*peer=*/false);
  for (uint32_t d : {0u, 15u})
    internal0D(iters, d, /*peer=*/true);
  internal1D(iters);
  internal2D(iters);

  // Tier 2: C ABI.
  cabi0D(iters);
  cabi1D(iters);
  cabi2D(iters);
  cabiGeneric3D(iters);

  // Tier 3: wrapper e2e + baselines.
  wrapperE2E(iters);

  std::printf("# sink=%llu\n", (unsigned long long)gSink);
  return 0;
}
