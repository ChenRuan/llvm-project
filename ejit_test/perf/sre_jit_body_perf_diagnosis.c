/**
 * SRE EJIT body-only performance diagnosis.
 *
 * This test obtains each compiled function pointer directly from the taskpool
 * and holds its read token while timing. Wrapper dispatch, cache lookup and
 * release_read are therefore outside every reported interval.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"

extern int SRE_printf(const char *format, ...);
extern uint32_t SRE_TaskDelay(uint32_t ticks);
extern uint64_t SRE_CycleCountGet64(void);
extern void call_init_array_functions(void);
extern uint8_t g_ucLocalCoreID;

#ifndef PERF_INNER_ITERS
#define PERF_INNER_ITERS 512u
#endif
#ifndef PERF_CALLBACK_ITERS
#define PERF_CALLBACK_ITERS 64u
#endif
#ifndef PERF_WARMUP_CALLS
#define PERF_WARMUP_CALLS 256u
#endif
#ifndef PERF_BATCH_CALLS
#define PERF_BATCH_CALLS 2048u
#endif
#ifndef PERF_BATCHES
#define PERF_BATCHES 5u
#endif
#ifndef PERF_COMPILE_POLLS
#define PERF_COMPILE_POLLS 60000u
#endif
#ifndef PERF_COMPILE_WARMUP_DELAY_TICKS
#define PERF_COMPILE_WARMUP_DELAY_TICKS 40000u
#endif
#ifndef PERF_INSTANCE_ID
#define PERF_INSTANCE_ID 0u
#endif
#ifndef PERF_KEEP_WORKER_IDLE
#define PERF_KEEP_WORKER_IDLE 1
#endif

#define PERF_INSTANCES 32u
#define PERF_INVALID_INDEX 0xffffffffu

struct PerfRuntimeState {
  volatile uint32_t value;
  volatile uint32_t salt;
  uint8_t pad[56];
};

ejit_period_arr(perf) struct PerfRuntimeState g_srePerfState[PERF_INSTANCES];

static volatile uint64_t g_srePerfSink;

static uint32_t local_core_id(void) { return (uint32_t)g_ucLocalCoreID; }

typedef uint32_t (*PerfExternalFn)(uint32_t, uint32_t);

/*
 * These helpers remain in the original image. Page alignment deliberately
 * spreads the targets across several text pages so round-robin/random callback
 * modes can exercise I-cache, ITLB and indirect-branch target behavior.
 */
#define DEFINE_EXTERNAL_STEP(N, CONSTANT)                                      \
  __attribute__((noinline, used, aligned(4096),                                \
                 section(".text.sre_perf_helper_" #N))) uint32_t               \
  sre_perf_external_step##N(uint32_t value, uint32_t salt) {                   \
    value ^= salt + (CONSTANT);                                                \
    value = (value << (5u + ((N) & 3u))) | (value >> (27u - ((N) & 3u)));      \
    return value * (1664525u + (N) * 2u) + 1013904223u + (N);                  \
  }

DEFINE_EXTERNAL_STEP(0, 0x9e3779b9u)
DEFINE_EXTERNAL_STEP(1, 0x7f4a7c15u)
DEFINE_EXTERNAL_STEP(2, 0x94d049bbu)
DEFINE_EXTERNAL_STEP(3, 0x2545f491u)
DEFINE_EXTERNAL_STEP(4, 0x85ebca6bu)
DEFINE_EXTERNAL_STEP(5, 0xc2b2ae35u)
DEFINE_EXTERNAL_STEP(6, 0x27d4eb2fu)
DEFINE_EXTERNAL_STEP(7, 0x165667b1u)

static PerfExternalFn const g_perfExternalSteps[8] = {
    sre_perf_external_step0, sre_perf_external_step1, sre_perf_external_step2,
    sre_perf_external_step3, sre_perf_external_step4, sre_perf_external_step5,
    sre_perf_external_step6, sre_perf_external_step7};

static inline uint32_t perf_mix(uint32_t value, uint32_t salt) {
  value ^= salt + 0x9e3779b9u;
  value = (value << 5) | (value >> 27);
  value *= 2246822519u;
  return value ^ (value >> 13);
}

#define DEFINE_COMPUTE_BODY(NAME, ATTR)                                        \
  ATTR __attribute__((noinline)) uint64_t NAME(                                \
      ejit_period_arr_ind(perf) uint8_t instance, uint32_t seed,               \
      uint32_t rounds) {                                                       \
    uint32_t x = seed ^ ((uint32_t)instance * 0x45d9f3bu);                     \
    uint64_t sum = 0;                                                          \
    for (uint32_t i = 0; i < rounds; ++i) {                                    \
      x = perf_mix(x, i + 17u);                                                \
      sum += (uint64_t)x * (uint64_t)(i + 3u);                                 \
    }                                                                          \
    return sum ^ x;                                                            \
  }

#define DEFINE_POINTER_BODY(NAME, ATTR)                                        \
  ATTR __attribute__((noinline)) uint64_t NAME(                                \
      ejit_period_arr_ind(perf) uint8_t instance, volatile uint32_t *valuePtr, \
      uint32_t seed, uint32_t rounds) {                                        \
    uint32_t x = seed + instance;                                              \
    uint64_t sum = 0;                                                          \
    for (uint32_t i = 0; i < rounds; ++i) {                                    \
      x = perf_mix(x, *valuePtr + i);                                          \
      sum += x;                                                                \
    }                                                                          \
    return sum;                                                                \
  }

#define DEFINE_GLOBAL_BODY(NAME, ATTR)                                         \
  ATTR __attribute__((noinline)) uint64_t NAME(                                \
      ejit_period_arr_ind(perf) uint8_t instance, uint32_t seed,               \
      uint32_t rounds) {                                                       \
    uint32_t x = seed + instance;                                              \
    uint64_t sum = 0;                                                          \
    for (uint32_t i = 0; i < rounds; ++i) {                                    \
      x = perf_mix(x, g_srePerfState[instance].value + i);                     \
      sum += x;                                                                \
    }                                                                          \
    return sum;                                                                \
  }

#define DEFINE_SNAPSHOT_BODY(NAME, ATTR)                                       \
  ATTR __attribute__((noinline)) uint64_t NAME(                                \
      ejit_period_arr_ind(perf) uint8_t instance, uint32_t seed,               \
      uint32_t rounds) {                                                       \
    uint32_t x = seed + instance;                                              \
    uint32_t value = g_srePerfState[instance].value;                           \
    uint64_t sum = 0;                                                          \
    for (uint32_t i = 0; i < rounds; ++i) {                                    \
      x = perf_mix(x, value + i);                                              \
      sum += x;                                                                \
    }                                                                          \
    return sum;                                                                \
  }

#define DEFINE_CALLBACK_BODY(NAME, ATTR)                                       \
  ATTR __attribute__((noinline)) uint64_t NAME(                                \
      ejit_period_arr_ind(perf) uint8_t instance,                              \
      PerfExternalFn const *externalSteps, uint32_t targetCount,               \
      uint32_t pattern, uint32_t seed, uint32_t rounds) {                      \
    uint32_t x = seed + instance;                                              \
    uint32_t selector = seed | 1u;                                             \
    uint32_t mask = targetCount - 1u;                                          \
    uint64_t sum = 0;                                                          \
    for (uint32_t i = 0; i < rounds; ++i) {                                    \
      uint32_t target = 0;                                                     \
      if (pattern == 1u) {                                                     \
        target = i & mask;                                                     \
      } else if (pattern == 2u) {                                              \
        selector ^= selector << 13;                                            \
        selector ^= selector >> 17;                                            \
        selector ^= selector << 5;                                             \
        target = selector & mask;                                              \
      }                                                                        \
      x = externalSteps[target](x, i + 31u);                                   \
      sum += x;                                                                \
    }                                                                          \
    return sum;                                                                \
  }

DEFINE_COMPUTE_BODY(sre_perf_compute_aot, )
DEFINE_COMPUTE_BODY(sre_perf_compute_jit, ejit_entry)
DEFINE_POINTER_BODY(sre_perf_pointer_aot, )
DEFINE_POINTER_BODY(sre_perf_pointer_jit, ejit_entry)
DEFINE_GLOBAL_BODY(sre_perf_global_aot, )
DEFINE_GLOBAL_BODY(sre_perf_global_jit, ejit_entry)
DEFINE_SNAPSHOT_BODY(sre_perf_snapshot_aot, )
DEFINE_SNAPSHOT_BODY(sre_perf_snapshot_jit, ejit_entry)
DEFINE_CALLBACK_BODY(sre_perf_callback_aot, )
DEFINE_CALLBACK_BODY(sre_perf_callback_jit, ejit_entry)

typedef uint64_t (*PerfBasicFn)(uint8_t, uint32_t, uint32_t);
typedef uint64_t (*PerfPointerFn)(uint8_t, volatile uint32_t *, uint32_t,
                                  uint32_t);
typedef uint64_t (*PerfCallbackFn)(uint8_t, PerfExternalFn const *, uint32_t,
                                   uint32_t, uint32_t, uint32_t);

struct PerfDirectFn {
  void *ptr;
  uint32_t bucket;
};

struct PerfResult {
  uint64_t first;
  uint64_t bestAvg;
  uint64_t meanAvg;
  uint64_t checksum;
};

static uint64_t abs_distance(const void *a, const void *b) {
  uintptr_t aa = (uintptr_t)a;
  uintptr_t bb = (uintptr_t)b;
  return aa >= bb ? (uint64_t)(aa - bb) : (uint64_t)(bb - aa);
}

static struct PerfDirectFn acquire_direct(uint32_t funcIndex, uint32_t dimType,
                                          uint32_t instance) {
  struct PerfDirectFn result = {0, 0};
  for (uint32_t poll = 0; poll < PERF_COMPILE_POLLS; ++poll) {
    ejit_status_t rc = ejit_taskpool_compile_or_get_1d(
        funcIndex, dimType, instance, &result.ptr, &result.bucket);
    if (rc == EJIT_OK && result.ptr)
      return result;
    if (rc < 0 && rc != EJIT_ERR_NOT_ACTIVE) {
      SRE_printf("[BODY-PERF] acquire failed func=%u rc=%d poll=%u\n",
                 funcIndex, (int)rc, poll);
      result.ptr = NULL;
      return result;
    }
    if ((poll & 0x3fu) == 0)
      (void)SRE_TaskDelay(1);
  }
  SRE_printf("[BODY-PERF] acquire timeout func=%u\n", funcIndex);
  result.ptr = NULL;
  return result;
}

static __attribute__((noinline)) struct PerfResult
measure_basic(PerfBasicFn fn, uint8_t instance, uint32_t rounds) {
  struct PerfResult r = {0, UINT64_MAX, 0, 0};
  PerfBasicFn volatile call = fn;
  uint64_t begin = SRE_CycleCountGet64();
  r.checksum ^= call(instance, 0x12345678u, rounds);
  r.first = SRE_CycleCountGet64() - begin;

  for (uint32_t i = 0; i < PERF_WARMUP_CALLS; ++i)
    r.checksum ^= call(instance, 0x12345678u + i, rounds);

  for (uint32_t batch = 0; batch < PERF_BATCHES; ++batch) {
    begin = SRE_CycleCountGet64();
    for (uint32_t i = 0; i < PERF_BATCH_CALLS; ++i)
      r.checksum ^= call(instance, 0x12345678u + i, rounds);
    uint64_t avg = (SRE_CycleCountGet64() - begin) / PERF_BATCH_CALLS;
    r.meanAvg += avg;
    if (avg < r.bestAvg)
      r.bestAvg = avg;
  }
  r.meanAvg /= PERF_BATCHES;
  g_srePerfSink ^= r.checksum;
  return r;
}

static __attribute__((noinline)) struct PerfResult
measure_pointer(PerfPointerFn fn, uint8_t instance, volatile uint32_t *valuePtr,
                uint32_t rounds) {
  struct PerfResult r = {0, UINT64_MAX, 0, 0};
  PerfPointerFn volatile call = fn;
  uint64_t begin = SRE_CycleCountGet64();
  r.checksum ^= call(instance, valuePtr, 0x12345678u, rounds);
  r.first = SRE_CycleCountGet64() - begin;

  for (uint32_t i = 0; i < PERF_WARMUP_CALLS; ++i)
    r.checksum ^= call(instance, valuePtr, 0x12345678u + i, rounds);

  for (uint32_t batch = 0; batch < PERF_BATCHES; ++batch) {
    begin = SRE_CycleCountGet64();
    for (uint32_t i = 0; i < PERF_BATCH_CALLS; ++i)
      r.checksum ^= call(instance, valuePtr, 0x12345678u + i, rounds);
    uint64_t avg = (SRE_CycleCountGet64() - begin) / PERF_BATCH_CALLS;
    r.meanAvg += avg;
    if (avg < r.bestAvg)
      r.bestAvg = avg;
  }
  r.meanAvg /= PERF_BATCHES;
  g_srePerfSink ^= r.checksum;
  return r;
}

static __attribute__((noinline)) struct PerfResult
measure_callback(PerfCallbackFn fn, uint8_t instance,
                 PerfExternalFn const *externalSteps, uint32_t targetCount,
                 uint32_t pattern, uint32_t rounds) {
  struct PerfResult r = {0, UINT64_MAX, 0, 0};
  PerfCallbackFn volatile call = fn;
  uint64_t begin = SRE_CycleCountGet64();
  r.checksum ^=
      call(instance, externalSteps, targetCount, pattern, 0x12345678u, rounds);
  r.first = SRE_CycleCountGet64() - begin;

  for (uint32_t i = 0; i < PERF_WARMUP_CALLS; ++i)
    r.checksum ^= call(instance, externalSteps, targetCount, pattern,
                       0x12345678u + i, rounds);

  for (uint32_t batch = 0; batch < PERF_BATCHES; ++batch) {
    begin = SRE_CycleCountGet64();
    for (uint32_t i = 0; i < PERF_BATCH_CALLS; ++i)
      r.checksum ^= call(instance, externalSteps, targetCount, pattern,
                         0x12345678u + i, rounds);
    uint64_t avg = (SRE_CycleCountGet64() - begin) / PERF_BATCH_CALLS;
    r.meanAvg += avg;
    if (avg < r.bestAvg)
      r.bestAvg = avg;
  }
  r.meanAvg /= PERF_BATCHES;
  g_srePerfSink ^= r.checksum;
  return r;
}

static void print_helper_layout(void) {
  uintptr_t low = UINTPTR_MAX;
  uintptr_t high = 0;
  for (uint32_t i = 0; i < 8; ++i) {
    uintptr_t address = (uintptr_t)g_perfExternalSteps[i];
    uint64_t previousGap =
        i ? abs_distance((const void *)g_perfExternalSteps[i - 1],
                         (const void *)g_perfExternalSteps[i])
          : 0;
    if (address < low)
      low = address;
    if (address > high)
      high = address;
    SRE_printf("[BODY-PERF] helper[%u]=%p page=0x%llx prev_gap=%llu\n", i,
               (void *)(uintptr_t)g_perfExternalSteps[i],
               (unsigned long long)(address >> 12),
               (unsigned long long)previousGap);
  }
  SRE_printf("[BODY-PERF] helper layout span=%llu bytes pages=%llu\n",
             (unsigned long long)(high - low),
             (unsigned long long)((high - low) / 4096u + 1u));
}

static void print_pair(const char *name, const void *aotPtr,
                       const struct PerfDirectFn *jit, struct PerfResult aot,
                       struct PerfResult jitted) {
  int64_t delta = (int64_t)jitted.bestAvg - (int64_t)aot.bestAvg;
  int64_t pct10 = aot.bestAvg ? (delta * 1000) / (int64_t)aot.bestAvg : 0;
  SRE_printf("[BODY-PERF] %-9s AOT first=%llu best=%llu mean=%llu sum=%llx\n",
             name, (unsigned long long)aot.first,
             (unsigned long long)aot.bestAvg, (unsigned long long)aot.meanAvg,
             (unsigned long long)aot.checksum);
  SRE_printf(
      "[BODY-PERF] %-9s JIT first=%llu best=%llu mean=%llu sum=%llx\n", name,
      (unsigned long long)jitted.first, (unsigned long long)jitted.bestAvg,
      (unsigned long long)jitted.meanAvg, (unsigned long long)jitted.checksum);
  SRE_printf("[BODY-PERF] %-9s delta_best=%lld.%01lld%% aot=%p jit=%p "
             "distance=%llu\n",
             name, (long long)(pct10 / 10),
             (long long)(pct10 < 0 ? -(pct10 % 10) : pct10 % 10), aotPtr,
             jit->ptr, (unsigned long long)abs_distance(aotPtr, jit->ptr));
  if (aot.checksum != jitted.checksum)
    SRE_printf("[BODY-PERF] ERROR %s result mismatch\n", name);
}

static void register_indices(uint32_t *dimType, uint32_t indexes[5]) {
  static const char *const names[5] = {
      "sre_perf_compute_jit", "sre_perf_pointer_jit", "sre_perf_global_jit",
      "sre_perf_snapshot_jit", "sre_perf_callback_jit"};
  *dimType = PERF_INVALID_INDEX;
  ejit_register_lifecycle("perf", dimType);
  for (uint32_t i = 0; i < 5; ++i) {
    indexes[i] = PERF_INVALID_INDEX;
    ejit_register_funcindex(names[i], &indexes[i]);
  }
}

/* Shell-compatible entry shared with the existing multi-core SRE demos. */
int test_ejit_period(uint8_t cellIdxArg, uint8_t trpIdxArg, uint8_t sliceIdxArg,
                     uint8_t carrierIdxArg) {
  (void)cellIdxArg;
  (void)trpIdxArg;
  (void)sliceIdxArg;
  (void)carrierIdxArg;

  const uint32_t coreId = local_core_id();
  uint32_t dimType;
  uint32_t indexes[5];
  uint8_t instance = (uint8_t)PERF_INSTANCE_ID;

  SRE_printf("[BODY-PERF][core=%u] begin instance=%u\n", coreId, instance);
  call_init_array_functions();
  register_indices(&dimType, indexes);

  ejit_config_t cfg = {0};
  cfg.compileMode = EJIT_COMPILE_ASYNC;
  cfg.optLevel = EJIT_OPT_L2;
  cfg.maxCodeMemory = 8u * 1024u * 1024u;
  cfg.maxDataMemory = 1u * 1024u * 1024u;
  cfg.maxCacheEntries = 256u;
  cfg.maxCacheSize = 8u * 1024u * 1024u;
  cfg.forceStaticRegistry = true;
  ejit_status_t initRc = ejit_init(&cfg);
  if (initRc != EJIT_OK) {
    SRE_printf("[BODY-PERF][core=%u] ejit_init failed rc=%d\n", coreId,
               (int)initRc);
    return -1;
  }

  uint32_t workerCore = ejit_taskpool_get_worker_core();
  SRE_printf(
      "[BODY-PERF][core=%u] workerCore=%u dimType=%u indexes=%u,%u,%u,%u,%u\n",
      coreId, workerCore, dimType, indexes[0], indexes[1], indexes[2],
      indexes[3], indexes[4]);

#if PERF_KEEP_WORKER_IDLE
  if (coreId == workerCore) {
    SRE_printf("[BODY-PERF][core=%u] worker kept idle for uncontaminated "
               "measurement\n",
               coreId);
    for (;;)
      (void)SRE_TaskDelay(1000);
  }
#endif

  if (dimType == PERF_INVALID_INDEX) {
    SRE_printf("[BODY-PERF][core=%u] invalid dimType\n", coreId);
    return -2;
  }
  for (uint32_t i = 0; i < 5; ++i) {
    if (indexes[i] == PERF_INVALID_INDEX) {
      SRE_printf("[BODY-PERF][core=%u] invalid funcIndex at %u\n", coreId, i);
      return -3;
    }
  }

  g_srePerfState[instance].value = 0x13579bdfu + instance;
  g_srePerfState[instance].salt = 0x2468ace0u;
  if (ejit_activate("perf", instance) != EJIT_OK) {
    SRE_printf("[BODY-PERF][core=%u] activate failed\n", coreId);
    return -4;
  }

  /* Trigger all five asynchronous compiles through their normal wrappers. */
  g_srePerfSink ^= sre_perf_compute_jit(instance, 1u, 2u);
  g_srePerfSink ^=
      sre_perf_pointer_jit(instance, &g_srePerfState[instance].value, 1u, 2u);
  g_srePerfSink ^= sre_perf_global_jit(instance, 1u, 2u);
  g_srePerfSink ^= sre_perf_snapshot_jit(instance, 1u, 2u);
  g_srePerfSink ^=
      sre_perf_callback_jit(instance, g_perfExternalSteps, 1u, 0u, 1u, 2u);

  SRE_printf("[BODY-PERF][core=%u] compile warmup delay=%u ticks\n", coreId,
             PERF_COMPILE_WARMUP_DELAY_TICKS);
  (void)SRE_TaskDelay(PERF_COMPILE_WARMUP_DELAY_TICKS);

  struct PerfDirectFn jit[5];
  for (uint32_t i = 0; i < 5; ++i) {
    jit[i] = acquire_direct(indexes[i], dimType, instance);
    if (!jit[i].ptr)
      return -5;
  }

  SRE_printf("[BODY-PERF][core=%u] direct fn pointers ready; lookup excluded\n",
             coreId);
  print_helper_layout();
  SRE_printf("[BODY-PERF] callback JIT=%p helper0_distance=%llu\n", jit[4].ptr,
             (unsigned long long)abs_distance(
                 jit[4].ptr, (const void *)g_perfExternalSteps[0]));

  struct PerfResult aot;
  struct PerfResult jitted;

  aot = measure_basic(sre_perf_compute_aot, instance, PERF_INNER_ITERS);
  jitted = measure_basic((PerfBasicFn)jit[0].ptr, instance, PERF_INNER_ITERS);
  print_pair("compute", (const void *)&sre_perf_compute_aot, &jit[0], aot,
             jitted);

  aot = measure_pointer(sre_perf_pointer_aot, instance,
                        &g_srePerfState[instance].value, PERF_INNER_ITERS);
  jitted = measure_pointer((PerfPointerFn)jit[1].ptr, instance,
                           &g_srePerfState[instance].value, PERF_INNER_ITERS);
  print_pair("pointer", (const void *)&sre_perf_pointer_aot, &jit[1], aot,
             jitted);

  aot = measure_basic(sre_perf_global_aot, instance, PERF_INNER_ITERS);
  jitted = measure_basic((PerfBasicFn)jit[2].ptr, instance, PERF_INNER_ITERS);
  print_pair("global", (const void *)&sre_perf_global_aot, &jit[2], aot,
             jitted);

  aot = measure_basic(sre_perf_snapshot_aot, instance, PERF_INNER_ITERS);
  jitted = measure_basic((PerfBasicFn)jit[3].ptr, instance, PERF_INNER_ITERS);
  print_pair("snapshot", (const void *)&sre_perf_snapshot_aot, &jit[3], aot,
             jitted);

  aot = measure_callback(sre_perf_callback_aot, instance, g_perfExternalSteps,
                         1u, 0u, PERF_CALLBACK_ITERS);
  jitted = measure_callback((PerfCallbackFn)jit[4].ptr, instance,
                            g_perfExternalSteps, 1u, 0u, PERF_CALLBACK_ITERS);
  print_pair("callback1", (const void *)&sre_perf_callback_aot, &jit[4], aot,
             jitted);

  aot = measure_callback(sre_perf_callback_aot, instance, g_perfExternalSteps,
                         4u, 1u, PERF_CALLBACK_ITERS);
  jitted = measure_callback((PerfCallbackFn)jit[4].ptr, instance,
                            g_perfExternalSteps, 4u, 1u, PERF_CALLBACK_ITERS);
  print_pair("callback4", (const void *)&sre_perf_callback_aot, &jit[4], aot,
             jitted);

  aot = measure_callback(sre_perf_callback_aot, instance, g_perfExternalSteps,
                         8u, 1u, PERF_CALLBACK_ITERS);
  jitted = measure_callback((PerfCallbackFn)jit[4].ptr, instance,
                            g_perfExternalSteps, 8u, 1u, PERF_CALLBACK_ITERS);
  print_pair("callback8", (const void *)&sre_perf_callback_aot, &jit[4], aot,
             jitted);

  aot = measure_callback(sre_perf_callback_aot, instance, g_perfExternalSteps,
                         8u, 2u, PERF_CALLBACK_ITERS);
  jitted = measure_callback((PerfCallbackFn)jit[4].ptr, instance,
                            g_perfExternalSteps, 8u, 2u, PERF_CALLBACK_ITERS);
  print_pair("random8", (const void *)&sre_perf_callback_aot, &jit[4], aot,
             jitted);

  SRE_printf("[BODY-PERF][core=%u] data=%p helper0=%p sink=%llx done\n", coreId,
             (void *)&g_srePerfState[instance],
             (void *)(uintptr_t)g_perfExternalSteps[0],
             (unsigned long long)g_srePerfSink);

  for (uint32_t i = 0; i < 5; ++i)
    ejit_taskpool_release_read(jit[i].bucket);

  /* Shared-taskpool peers must not tear down the owner while other cores run.
   */
  for (;;)
    (void)SRE_TaskDelay(1000);
}
