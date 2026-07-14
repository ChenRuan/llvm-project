//===-- EJitHotPathBenchAbi.h - shared decls for the hot-path bench -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  Declarations shared between the benchmark driver (EJitHotPathBench.cpp) and
//  the ABI/wrapper translation unit (EJitHotPathBenchAbi.cpp). The two live in
//  SEPARATE translation units on purpose so the optimizer cannot inline the
//  C-ABI shim into the measurement loop or fold away the indirect call — the
//  compiled code path mirrors what a cross-TU AOT wrapper actually executes.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EJIT_HOTPATH_BENCH_ABI_H
#define LLVM_EJIT_HOTPATH_BENCH_ABI_H

#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"
#include <cstdint>

namespace ejitbench {

// Faithful mirror of the runtime's EJit black-box object indirection: the
// production C ABI resolves the pool via `gEJIT ? gEJIT->sharedTaskPool() :
// nullptr`, i.e. TWO dependent loads + two null checks. We reproduce that exact
// shape so the shim's measured cost matches production (the only thing we drop
// is the real gEJIT object graph, which is irrelevant to the hot path).
struct EJitStub {
  llvm::ejit::EJitSharedTaskPool *sharedPool = nullptr;
};

// Installed once by the driver before measuring; read by the shim TU.
extern EJitStub *gEJITStub;

// Status codes mirroring EJitError.h's ejit_status_t hot-path subset.
enum BenchStatus : uint32_t {
  BENCH_OK = 0,
  BENCH_PENDING = 1,
  BENCH_ERR_DISABLED = 2,
  BENCH_ERR_INSTANCE_DISABLED = 3,
  BENCH_ERR_QUEUE_FULL = 4,
  BENCH_ERR_INVALID_PARAM = 5,
  BENCH_ERR_COMPILE_FAILED = 6,
  BENCH_ERR_NOT_ACTIVE = 7,
};

//===-- Faithful same-ABI C shim (mirrors EJitRuntime.cpp) ----------------===//
// Byte-for-byte the same body as ejit_taskpool_compile_or_get{,_0d,_1d,_2d},
// except the pool is resolved from gEJITStub instead of gEJIT. Kept in a
// separate TU and NOT inlined, exactly like the real C ABI relative to the
// wrapper.
extern "C" {
uint32_t bench_cabi_0d(uint32_t funcIndex, void **outFn, uint32_t *outBucket);
uint32_t bench_cabi_1d(uint32_t funcIndex, uint32_t dim0, uint32_t inst0,
                       void **outFn, uint32_t *outBucket);
uint32_t bench_cabi_2d(uint32_t funcIndex, uint32_t dim0, uint32_t inst0,
                       uint32_t dim1, uint32_t inst1, void **outFn,
                       uint32_t *outBucket);
uint32_t bench_cabi_generic(uint32_t funcIndex,
                            const llvm::ejit::EJitDimPair *dims,
                            uint32_t numDims, void **outFn,
                            uint32_t *outBucket);
void bench_cabi_release_read(uint32_t bucketIndex);

// Experimental additive "trusted-wrapper" fast entry (optimization candidate):
// returns the fnPtr directly (nullptr on any non-hit) and, in a NO_RECLAIM
// build, needs no out-bucket / release at all. Never replaces the public ABI.
void *bench_cabi_hit_0d(uint32_t funcIndex, uint32_t *outBucket);
}

//===-- Wrapper end-to-end (mirrors EJitWrapperGen output) ----------------===//
// jit_entry -> jit_call (C ABI) -> jit_dispatch (indirect call + release) ->
// return, with a jit_fallback AOT body. Real indirect call through fnPtr.
extern "C" {
long bench_wrapper_0d_hit(long arg);      // resolves to a real JIT fn, calls it
long bench_wrapper_0d_fallback(long arg); // forced AOT fallback body
long bench_wrapper_0d_trusted(long arg);  // uses bench_cabi_hit_0d fast entry
long bench_aot_0d(long arg);              // the AOT body alone (direct)
long bench_call_jit_direct(long arg);     // direct call of the same JIT fn
}

// Addresses published into the cache as "compiled code" (real, executable).
void *benchJitFn0D();  // tiny leaf, magnifies fixed overhead
void *benchJitFnBig(); // larger body, dilutes fixed overhead
long benchJitRawBig(long arg);

} // namespace ejitbench

#endif
