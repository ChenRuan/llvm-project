//===-- ejit_fixed_near_hot_sre_multicore_test.c - board smoke test -------===//
//
// Board flow after reset:
//   1. Core 6: run test_ejit_period once. It becomes the fixed worker and
//      compile owner, then returns after printing the ready message.
//   2. Core 16: run test_ejit_period once. It attaches to core 6, triggers one
//      version for cell[0..15] and one no-cell/public version, waits for the
//      queue-empty publish, and prints the pool diagnostics.
//   3. Core 6: run test_ejit_fixed_near_hot_print once. It validates all 17
//      pool envelopes and prints the published compiled list separately.
//
// This file is deliberately independent of business headers and does not use
// MFS. Do not call ejit_shutdown(): the shared worker must survive shell
// invocations.
//===----------------------------------------------------------------------===//

// Keep this board test self-contained: the harness supplies the public EJIT
// symbols below, so the file does not depend on project or business headers.
typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef unsigned long size_t;
typedef _Bool bool;

#define true 1
#define false 0

#define EJIT_PERIOD_CONST __attribute__((ejit_period_const))
#define EJIT_IN_PERIOD_ARRAY(x) __attribute__((ejit_in_period_array(#x)))
#define EJIT_DIM(x) __attribute__((ejit_dim(#x)))
#define EJIT_ENTRY __attribute__((ejit_entry))
#define ejit_may_const EJIT_PERIOD_CONST
#define ejit_period_arr(x) EJIT_IN_PERIOD_ARRAY(x)
#define ejit_period_arr_ind(x) EJIT_DIM(x)
#define ejit_entry EJIT_ENTRY

typedef enum {
  EJIT_OK = 0,
} ejit_status_t;

typedef enum {
  EJIT_COMPILE_SYNC = 0,
  EJIT_COMPILE_ASYNC = 1,
} ejit_compile_mode_t;

typedef enum {
  EJIT_OPT_L1 = 1,
  EJIT_OPT_L2 = 2,
  EJIT_OPT_L3 = 3,
} ejit_opt_level_t;

typedef struct {
  ejit_compile_mode_t compileMode;
  ejit_opt_level_t optLevel;
  size_t maxCodeMemory;
  size_t maxDataMemory;
  size_t maxCacheEntries;
  size_t maxCacheSize;
  bool enableLogger;
  bool forceStaticRegistry;
  const char *dumpJITDir;
} ejit_config_t;

typedef struct {
  uint64_t cacheHits;
  uint64_t asyncCompiles;
  uint64_t asyncEnqueues;
  uint64_t alreadyPending;
  uint64_t queueFull;
  uint64_t compileFailed;
  uint64_t publishFailed;
  uint64_t instanceDisabled;
  uint64_t instanceDisabledPreActivate;
  uint32_t readyEntries;
  uint32_t pendingEntries;
  uint32_t queueApproxSize;
  uint32_t reserved;
} ejit_taskpool_stats_t;

typedef struct {
  uint64_t poolCount;
  uint64_t sealedCount;
  uint64_t activeCount;
  uint64_t usedBytes;
  uint64_t reservedBytes;
  uint64_t wastedBytes;
  uint64_t sealInvocations;
  uint64_t splitInvocations;
  uint64_t finalizedRangeCount;
} ejit_code_pool_stats_t;

typedef struct {
  ejit_code_pool_stats_t total;
  ejit_code_pool_stats_t near;
  ejit_code_pool_stats_t nearHot[17];
  ejit_code_pool_stats_t far;
} ejit_code_pool_stats_v2_t;

typedef enum {
  EJIT_LOG_OFF = 0,
  EJIT_LOG_INFO = 1,
  EJIT_LOG_VERBOSE = 2,
  EJIT_LOG_DEBUG = 3,
} ejit_log_level_t;

extern ejit_status_t ejit_init(const ejit_config_t *config);
extern ejit_status_t ejit_init_pgo(const ejit_config_t *config);
extern ejit_status_t ejit_activate(const char *periodName, uint32_t cellIdx);
extern void ejit_clear_cache(void);
extern uint32_t ejit_taskpool_pending_count(void);
extern ejit_status_t ejit_taskpool_get_stats(ejit_taskpool_stats_t *out);
extern ejit_status_t
ejit_get_code_pool_stats_v2(ejit_code_pool_stats_v2_t *out);
extern void ejit_print_code_pool_stats(void);
extern void ejit_taskpool_print_compiled(void);
extern void ejit_taskpool_print_stats(void);
extern uint32_t ejit_taskpool_get_worker_core(void);
extern void ejit_set_log_level(ejit_log_level_t level);

extern void SRE_printf(const char *format, ...);
extern uint32_t SRE_TaskDelay(uint32_t tick);
extern void call_init_array_functions(void);
extern uint8_t g_ucLocalCoreID;

#ifndef EJIT_SHARED_SECTION_ATTR
#define EJIT_SHARED_SECTION_ATTR __attribute__((section(".mc_shared")))
#endif

#ifndef FIXED_NEAR_HOT_WORKER_CORE
#define FIXED_NEAR_HOT_WORKER_CORE 6u
#endif
#ifndef FIXED_NEAR_HOT_PRODUCER_CORE
#define FIXED_NEAR_HOT_PRODUCER_CORE 16u
#endif

#define FIXED_NEAR_HOT_CELL_COUNT 16u
#define FIXED_NEAR_HOT_POOL_COUNT 17u
#ifndef FIXED_NEAR_HOT_FUNCTION_COUNT
#define FIXED_NEAR_HOT_FUNCTION_COUNT 4u
#endif
#ifndef FIXED_NEAR_HOT_PROFILE_BATCH
#ifdef EJIT_SRE_PGO_MAX_CONCURRENT_PROFILES
#define FIXED_NEAR_HOT_PROFILE_BATCH EJIT_SRE_PGO_MAX_CONCURRENT_PROFILES
#else
#define FIXED_NEAR_HOT_PROFILE_BATCH 2u
#endif
#endif
#define FIXED_NEAR_HOT_T1_SAMPLES 64u
#define FIXED_NEAR_HOT_SPECIALIZATION_COUNT                                    \
  (FIXED_NEAR_HOT_FUNCTION_COUNT * FIXED_NEAR_HOT_CELL_COUNT)
#define FIXED_NEAR_HOT_IDENTITY_COUNT (FIXED_NEAR_HOT_SPECIALIZATION_COUNT + 1u)
#define FIXED_NEAR_HOT_BATCH_WIDTH                                            \
  (FIXED_NEAR_HOT_PROFILE_BATCH < FIXED_NEAR_HOT_FUNCTION_COUNT              \
       ? FIXED_NEAR_HOT_PROFILE_BATCH                                          \
       : FIXED_NEAR_HOT_FUNCTION_COUNT)
#define FIXED_NEAR_HOT_SPECIALIZATION_BATCH_COUNT                              \
  ((FIXED_NEAR_HOT_SPECIALIZATION_COUNT + FIXED_NEAR_HOT_BATCH_WIDTH - 1u) /  \
   FIXED_NEAR_HOT_BATCH_WIDTH)
#define FIXED_NEAR_HOT_BATCH_COUNT                                             \
  (FIXED_NEAR_HOT_SPECIALIZATION_BATCH_COUNT + 1u)
#define FIXED_NEAR_HOT_WAIT_ROUNDS 6000u
#define FIXED_NEAR_HOT_WAIT_TICKS 10u

#if FIXED_NEAR_HOT_FUNCTION_COUNT != 4u && FIXED_NEAR_HOT_FUNCTION_COUNT != 8u
#error "FIXED_NEAR_HOT_FUNCTION_COUNT must be 4 or 8"
#endif
#if FIXED_NEAR_HOT_PROFILE_BATCH < 1u
#error "FIXED_NEAR_HOT_PROFILE_BATCH must be at least 1"
#endif

enum FixedNearHotStage {
  FIXED_NEAR_HOT_RESET = 0,
  FIXED_NEAR_HOT_WORKER_READY = 1,
  FIXED_NEAR_HOT_PUBLISHED = 2,
  FIXED_NEAR_HOT_PRINTED = 3,
};

struct FixedNearHotConfig {
  ejit_may_const uint32_t multiplier;
  ejit_may_const uint32_t bias;
};

EJIT_SHARED_SECTION_ATTR ejit_period_arr(cell)
struct FixedNearHotConfig g_fixed_near_hot_config[FIXED_NEAR_HOT_CELL_COUNT];
EJIT_SHARED_SECTION_ATTR volatile uint32_t g_fixed_near_hot_stage;
EJIT_SHARED_SECTION_ATTR volatile uint64_t g_fixed_near_hot_sink;

#define FIXED_NEAR_HOT_BODY(TAG)                                               \
  const struct FixedNearHotConfig *cfg = &g_fixed_near_hot_config[cellIndex];  \
  uint32_t value = input ^ (TAG);                                              \
  for (uint32_t i = 0; i < 8u; ++i)                                            \
    value = (value * cfg->multiplier) ^ (value >> 3) ^ (i + (TAG));            \
  return value + cfg->bias + (TAG)

#define FIXED_NEAR_HOT_ENTRY(NAME, TAG)                                        \
  ejit_entry uint32_t NAME(ejit_period_arr_ind(cell) uint8_t cellIndex,        \
                           uint32_t input) {                                   \
    FIXED_NEAR_HOT_BODY(TAG);                                                  \
  }

FIXED_NEAR_HOT_ENTRY(fixed_near_hot_cell_0, 0x101u)
FIXED_NEAR_HOT_ENTRY(fixed_near_hot_cell_1, 0x202u)
FIXED_NEAR_HOT_ENTRY(fixed_near_hot_cell_2, 0x303u)
FIXED_NEAR_HOT_ENTRY(fixed_near_hot_cell_3, 0x404u)

#if FIXED_NEAR_HOT_FUNCTION_COUNT >= 8u
FIXED_NEAR_HOT_ENTRY(fixed_near_hot_cell_4, 0x505u)
FIXED_NEAR_HOT_ENTRY(fixed_near_hot_cell_5, 0x606u)
FIXED_NEAR_HOT_ENTRY(fixed_near_hot_cell_6, 0x707u)
FIXED_NEAR_HOT_ENTRY(fixed_near_hot_cell_7, 0x808u)
#endif

#undef FIXED_NEAR_HOT_ENTRY
#undef FIXED_NEAR_HOT_BODY

// No period dimension is intentional: this entry must route to public pool 16.
ejit_entry uint32_t fixed_near_hot_public(uint32_t input) {
  return input + 0x5a5au;
}

static uint32_t call_fixed_near_hot(uint32_t function, uint8_t cell,
                                    uint32_t input) {
  switch (function) {
  case 0:
    return fixed_near_hot_cell_0(cell, input);
  case 1:
    return fixed_near_hot_cell_1(cell, input);
  case 2:
    return fixed_near_hot_cell_2(cell, input);
  case 3:
    return fixed_near_hot_cell_3(cell, input);
#if FIXED_NEAR_HOT_FUNCTION_COUNT >= 8u
  case 4:
    return fixed_near_hot_cell_4(cell, input);
  case 5:
    return fixed_near_hot_cell_5(cell, input);
  case 6:
    return fixed_near_hot_cell_6(cell, input);
  case 7:
    return fixed_near_hot_cell_7(cell, input);
#endif
  default:
    return 0u;
  }
}

static uint32_t expected_fixed_near_hot(uint32_t function, uint8_t cell,
                                        uint32_t input) {
  static const uint32_t tags[8] = {0x101u, 0x202u, 0x303u, 0x404u,
                                   0x505u, 0x606u, 0x707u, 0x808u};
  const struct FixedNearHotConfig *cfg = &g_fixed_near_hot_config[cell];
  uint32_t value = input ^ tags[function];
  for (uint32_t i = 0; i < 8u; ++i)
    value = (value * cfg->multiplier) ^ (value >> 3) ^ (i + tags[function]);
  return value + cfg->bias + tags[function];
}

static uint32_t call_fixed_near_hot_identity(uint32_t identity,
                                             uint32_t input) {
  if (identity == FIXED_NEAR_HOT_SPECIALIZATION_COUNT)
    return fixed_near_hot_public(input);
  // Keep each batch cell-major. The PGO admission gate is per function, so a
  // batch must never contain two cells of the same function.
  const uint8_t cell = (uint8_t)(identity / FIXED_NEAR_HOT_FUNCTION_COUNT);
  const uint32_t function = identity % FIXED_NEAR_HOT_FUNCTION_COUNT;
  return call_fixed_near_hot(function, cell, input);
}

static uint32_t expected_fixed_near_hot_identity(uint32_t identity,
                                                 uint32_t input) {
  if (identity == FIXED_NEAR_HOT_SPECIALIZATION_COUNT)
    return input + 0x5a5au;
  const uint8_t cell = (uint8_t)(identity / FIXED_NEAR_HOT_FUNCTION_COUNT);
  const uint32_t function = identity % FIXED_NEAR_HOT_FUNCTION_COUNT;
  return expected_fixed_near_hot(function, cell, input);
}

static void print_batch_schedule(uint32_t batch, uint32_t first, uint32_t end) {
  SRE_printf("[FIXED-NEAR-HOT] batch=%u/%u schedule:", batch,
             FIXED_NEAR_HOT_BATCH_COUNT);
  for (uint32_t identity = first; identity < end; ++identity) {
    if (identity == FIXED_NEAR_HOT_SPECIALIZATION_COUNT) {
      SRE_printf(" public");
    } else {
      const uint32_t cell = identity / FIXED_NEAR_HOT_FUNCTION_COUNT;
      const uint32_t function = identity % FIXED_NEAR_HOT_FUNCTION_COUNT;
      SRE_printf(" id=%u(cell=%u funcIndex=%u)", identity, cell, function);
    }
  }
  SRE_printf("\n");
}

static int wait_for_compile_target(uint64_t baseline, uint64_t target,
                                   uint32_t expectedDelta, uint32_t batch,
                                   uint32_t core, const char *phase) {
  uint32_t emptyRounds = 0;
  for (uint32_t round = 0; round < FIXED_NEAR_HOT_WAIT_ROUNDS; ++round) {
    ejit_taskpool_stats_t stats = {0};
    if (ejit_taskpool_get_stats(&stats) != EJIT_OK) {
      SRE_printf("[FIXED-NEAR-HOT][core=%u] FAIL get taskpool stats\n", core);
      return 0;
    }
    if (stats.compileFailed || stats.publishFailed) {
      SRE_printf("[FIXED-NEAR-HOT][core=%u] FAIL compile=%llu publish=%llu\n",
                 core, (unsigned long long)stats.compileFailed,
                 (unsigned long long)stats.publishFailed);
      return 0;
    }
    const uint32_t pending = ejit_taskpool_pending_count();
    if (stats.asyncCompiles >= target && pending == 0)
      return 1;
    if (pending == 0 && stats.pendingEntries == 0 &&
        stats.queueApproxSize == 0 && stats.asyncCompiles < target) {
      if (++emptyRounds >= 5u) {
        const uint64_t delta = stats.asyncCompiles >= baseline
                                   ? stats.asyncCompiles - baseline
                                   : 0;
        SRE_printf(
            "[FIXED-NEAR-HOT][core=%u] FAIL %s batch=%u pending=0 "
            "compiled_delta=%llu/%u; PGO same-function admission/deferred "
            "or missing T1 request (AOT fallback)\n",
            core, phase, batch, (unsigned long long)delta, expectedDelta);
        return 0;
      }
    } else {
      emptyRounds = 0;
    }
    if ((round % 500u) == 0)
      SRE_printf("[FIXED-NEAR-HOT][core=%u] waiting %s compiles=%llu/%llu "
                 "pending=%u\n",
                 core, phase, (unsigned long long)stats.asyncCompiles,
                 (unsigned long long)target, pending);
    (void)SRE_TaskDelay(FIXED_NEAR_HOT_WAIT_TICKS);
  }
  SRE_printf("[FIXED-NEAR-HOT][core=%u] FAIL %s timeout\n", core, phase);
  return 0;
}

static int validate_pool_stats(const ejit_code_pool_stats_v2_t *stats,
                               uint32_t core) {
  int failures = 0;
  for (uint32_t pool = 0; pool < FIXED_NEAR_HOT_POOL_COUNT; ++pool) {
    const ejit_code_pool_stats_t *detail = &stats->nearHot[pool];
    const uint32_t required =
        pool == FIXED_NEAR_HOT_CELL_COUNT ? 1u : FIXED_NEAR_HOT_FUNCTION_COUNT;
    if (detail->usedBytes == 0 || detail->reservedBytes < detail->usedBytes ||
        detail->finalizedRangeCount < required ||
        detail->sealInvocations == 0) {
      SRE_printf(
          "[FIXED-NEAR-HOT][core=%u] FAIL pool=%u used=%llu reserved=%llu "
          "finalized=%llu/%u seals=%llu\n",
          core, pool, (unsigned long long)detail->usedBytes,
          (unsigned long long)detail->reservedBytes,
          (unsigned long long)detail->finalizedRangeCount, required,
          (unsigned long long)detail->sealInvocations);
      ++failures;
    }
  }
  if (stats->far.usedBytes == 0 || stats->far.finalizedRangeCount == 0) {
    SRE_printf("[FIXED-NEAR-HOT][core=%u] FAIL far T1 pool used=%llu "
               "finalized=%llu\n",
               core, (unsigned long long)stats->far.usedBytes,
               (unsigned long long)stats->far.finalizedRangeCount);
    ++failures;
  }
  // The public v2 C ABI intentionally exposes counters only. The paired
  // ejit_print_code_pool_stats() call below prints each detailed [base,end),
  // pending/finalized/full/fallback record for alignment and overlap review.
  return failures == 0;
}

static void print_layout_summary(const ejit_code_pool_stats_v2_t *stats,
                                 uint32_t core) {
  SRE_printf("[FIXED-NEAR-HOT][core=%u] layout summary: pool=versions "
             "exec_bytes pool_span tail_padding density_permille\n",
             core);
  for (uint32_t pool = 0; pool < FIXED_NEAR_HOT_POOL_COUNT; ++pool) {
    const ejit_code_pool_stats_t *detail = &stats->nearHot[pool];
    const uint64_t padding = detail->reservedBytes > detail->usedBytes
                                 ? detail->reservedBytes - detail->usedBytes
                                 : 0;
    const uint64_t density =
        detail->reservedBytes
            ? (detail->usedBytes * 1000u) / detail->reservedBytes
            : 0;
    SRE_printf("[FIXED-NEAR-HOT][core=%u] layout pool=%s%u versions=%llu "
               "exec_bytes=%llu pool_span=%llu tail_padding=%llu "
               "density=%llu/1000\n",
               core, pool == FIXED_NEAR_HOT_CELL_COUNT ? "public" : "cell",
               pool, (unsigned long long)detail->finalizedRangeCount,
               (unsigned long long)detail->usedBytes,
               (unsigned long long)detail->reservedBytes,
               (unsigned long long)padding, (unsigned long long)density);
  }
  SRE_printf("[FIXED-NEAR-HOT][core=%u] exact first/last address, span, "
             "4K gaps, fallback and failedPoolBitmap are in runtime "
             "range/compiled summaries above; counter-only ABI fields do not "
             "infer those values.\n",
             core);
}

static int run_producer(void) {
  const uint32_t core = (uint32_t)g_ucLocalCoreID;
  if (__atomic_load_n(&g_fixed_near_hot_stage, __ATOMIC_ACQUIRE) !=
      FIXED_NEAR_HOT_WORKER_READY) {
    SRE_printf("[FIXED-NEAR-HOT][core=%u] FAIL run core %u first\n", core,
               FIXED_NEAR_HOT_WORKER_CORE);
    return -2;
  }

  for (uint32_t cell = 0; cell < FIXED_NEAR_HOT_CELL_COUNT; ++cell) {
    if (ejit_activate("cell", cell) != EJIT_OK) {
      SRE_printf("[FIXED-NEAR-HOT][core=%u] FAIL activate cell=%u\n", core,
                 cell);
      return -3;
    }
  }
  ejit_clear_cache();

  ejit_taskpool_stats_t initialStats = {0};
  if (ejit_taskpool_get_stats(&initialStats) != EJIT_OK) {
    SRE_printf("[FIXED-NEAR-HOT][core=%u] FAIL initial stats\n", core);
    return -4;
  }
  const uint64_t compileBaseline = initialStats.asyncCompiles;
  uint64_t sink = 0;

  SRE_printf("[FIXED-NEAR-HOT][core=%u] PGO profile batches=%u/%u "
             "configured_batch=%u effective_batch=%u samples=%u/function "
             "identity_count=%u (cell-major; public-isolated)\n",
             core, FIXED_NEAR_HOT_BATCH_COUNT, FIXED_NEAR_HOT_BATCH_COUNT,
             FIXED_NEAR_HOT_PROFILE_BATCH, FIXED_NEAR_HOT_BATCH_WIDTH,
             FIXED_NEAR_HOT_T1_SAMPLES, FIXED_NEAR_HOT_IDENTITY_COUNT);
  for (uint32_t batch = 0; batch < FIXED_NEAR_HOT_BATCH_COUNT; ++batch) {
    const uint32_t first =
        batch < FIXED_NEAR_HOT_SPECIALIZATION_BATCH_COUNT
            ? batch * FIXED_NEAR_HOT_BATCH_WIDTH
            : FIXED_NEAR_HOT_SPECIALIZATION_COUNT;
    const uint32_t end =
        batch < FIXED_NEAR_HOT_SPECIALIZATION_BATCH_COUNT
            ? (first + FIXED_NEAR_HOT_BATCH_WIDTH <
                       FIXED_NEAR_HOT_SPECIALIZATION_COUNT
                   ? first + FIXED_NEAR_HOT_BATCH_WIDTH
                   : FIXED_NEAR_HOT_SPECIALIZATION_COUNT)
            : FIXED_NEAR_HOT_IDENTITY_COUNT;
    const uint32_t batchSize = end - first;
    ejit_taskpool_stats_t beforeBatch = {0};
    if (ejit_taskpool_get_stats(&beforeBatch) != EJIT_OK) {
      SRE_printf("[FIXED-NEAR-HOT][core=%u] FAIL batch stats\n", core);
      return -5;
    }
    print_batch_schedule(batch + 1u, first, end);

    // The first call for each identity is expected to use AOT and enqueue T1.
    // Waiting for the batch to drain before sampling prevents AOT fallback
    // calls from being mistaken for real Tier-1 samples.
    for (uint32_t identity = first; identity < end; ++identity) {
      const uint32_t seed = 0x10000u + identity * 0x100u;
      const uint32_t got = call_fixed_near_hot_identity(identity, seed);
      const uint32_t expected =
          expected_fixed_near_hot_identity(identity, seed);
      if (got != expected) {
        SRE_printf("[FIXED-NEAR-HOT][core=%u] FAIL identity=%u got=%u "
                   "expected=%u\n",
                   core, identity, got, expected);
        return -6;
      }
      sink ^= got;
    }
    if (!wait_for_compile_target(beforeBatch.asyncCompiles,
                                 beforeBatch.asyncCompiles + batchSize,
                                 batchSize, batch + 1u, core, "T1"))
      return -7;
    SRE_printf("[FIXED-NEAR-HOT][core=%u] batch=%u/%u T1 ready "
               "identities=%u/%u\n",
               core, batch + 1u, FIXED_NEAR_HOT_BATCH_COUNT, end,
               FIXED_NEAR_HOT_IDENTITY_COUNT);

    for (uint32_t sample = 0; sample < FIXED_NEAR_HOT_T1_SAMPLES; ++sample) {
      for (uint32_t identity = first; identity < end; ++identity) {
        const uint32_t seed = 0x10000u + identity * 0x100u + sample + 1u;
        const uint32_t got = call_fixed_near_hot_identity(identity, seed);
        const uint32_t expected =
            expected_fixed_near_hot_identity(identity, seed);
        if (got != expected) {
          SRE_printf("[FIXED-NEAR-HOT][core=%u] FAIL identity=%u "
                     "sample=%u got=%u expected=%u\n",
                     core, identity, sample + 1u, got, expected);
          return -8;
        }
        sink ^= got;
      }
      if (sample == 0 || ((sample + 1u) % 16u) == 0)
        SRE_printf("[FIXED-NEAR-HOT][core=%u] batch=%u/%u sample=%u/%u "
                   "identities=%u/%u\n",
                   core, batch + 1u, FIXED_NEAR_HOT_BATCH_COUNT, sample + 1u,
                   FIXED_NEAR_HOT_T1_SAMPLES, end,
                   FIXED_NEAR_HOT_IDENTITY_COUNT);
      if (((sample + 1u) % 8u) == 0)
        (void)SRE_TaskDelay(1u);
    }

    ejit_taskpool_stats_t afterSamples = {0};
    if (ejit_taskpool_get_stats(&afterSamples) != EJIT_OK) {
      SRE_printf("[FIXED-NEAR-HOT][core=%u] FAIL sample stats\n", core);
      return -9;
    }
    SRE_printf("[FIXED-NEAR-HOT][core=%u] batch=%u/%u sample complete; "
               "T2 pending observation=%u queue=%u (best effort; it may "
               "already have flushed)\n",
               core, batch + 1u, FIXED_NEAR_HOT_BATCH_COUNT,
               ejit_taskpool_pending_count(), afterSamples.queueApproxSize);
    if (!wait_for_compile_target(beforeBatch.asyncCompiles,
                                 beforeBatch.asyncCompiles + 2u * batchSize,
                                 2u * batchSize, batch + 1u, core, "T2")) {
      ejit_taskpool_print_stats();
      return -10;
    }
    SRE_printf("[FIXED-NEAR-HOT][core=%u] batch=%u/%u T2 finalized "
               "identities=%u/%u\n",
               core, batch + 1u, FIXED_NEAR_HOT_BATCH_COUNT, end,
               FIXED_NEAR_HOT_IDENTITY_COUNT);
  }

  ejit_code_pool_stats_v2_t stats = {0};
  ejit_taskpool_stats_t finalTaskStats = {0};
  if (ejit_taskpool_get_stats(&finalTaskStats) != EJIT_OK ||
      finalTaskStats.compileFailed || finalTaskStats.publishFailed ||
      finalTaskStats.asyncCompiles <
          compileBaseline + FIXED_NEAR_HOT_IDENTITY_COUNT * 2u ||
      finalTaskStats.readyEntries < FIXED_NEAR_HOT_IDENTITY_COUNT ||
      ejit_taskpool_pending_count() != 0 ||
      ejit_get_code_pool_stats_v2(&stats) != EJIT_OK ||
      !validate_pool_stats(&stats, core)) {
    SRE_printf("[FIXED-NEAR-HOT][core=%u] FAIL pool validation\n", core);
    ejit_print_code_pool_stats();
    ejit_taskpool_print_stats();
    return -11;
  }
  __atomic_store_n(&g_fixed_near_hot_sink, sink, __ATOMIC_RELEASE);
  __atomic_store_n(&g_fixed_near_hot_stage, FIXED_NEAR_HOT_PUBLISHED,
                   __ATOMIC_RELEASE);
  ejit_set_log_level(EJIT_LOG_VERBOSE);
  ejit_print_code_pool_stats();
  ejit_taskpool_print_compiled();
  print_layout_summary(&stats, core);
  SRE_printf(
      "[FIXED-NEAR-HOT][core=%u] PASS producer: T2=%u cell "
      "specializations + public; T1 far used; batches=%u/%u "
      "compiled_delta=%llu expected_min=%u pending=0 "
      "compileFailed=0 publishFailed=0 fallback=0 and "
      "failedPoolBitmap=0 must be confirmed in runtime summary; run "
      "test_ejit_fixed_near_hot_print on core %u\n",
      core, FIXED_NEAR_HOT_SPECIALIZATION_COUNT, FIXED_NEAR_HOT_BATCH_COUNT,
      FIXED_NEAR_HOT_BATCH_COUNT,
      (unsigned long long)(finalTaskStats.asyncCompiles - compileBaseline),
      FIXED_NEAR_HOT_IDENTITY_COUNT * 2u, FIXED_NEAR_HOT_WORKER_CORE);
  return 0;
}

static int run_worker(void) {
  const uint32_t core = (uint32_t)g_ucLocalCoreID;
  if (__atomic_load_n(&g_fixed_near_hot_stage, __ATOMIC_ACQUIRE) ==
      FIXED_NEAR_HOT_RESET) {
    for (uint32_t cell = 0; cell < FIXED_NEAR_HOT_CELL_COUNT; ++cell) {
      g_fixed_near_hot_config[cell].multiplier = 10u + cell;
      g_fixed_near_hot_config[cell].bias = 0x1000u + cell;
    }
    __atomic_store_n(&g_fixed_near_hot_sink, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_fixed_near_hot_stage, FIXED_NEAR_HOT_WORKER_READY,
                     __ATOMIC_RELEASE);
    SRE_printf("[FIXED-NEAR-HOT][core=%u] PASS fixed worker ready; run "
               "test_ejit_period on core %u\n",
               core, FIXED_NEAR_HOT_PRODUCER_CORE);
    return 0;
  }
  SRE_printf("[FIXED-NEAR-HOT][core=%u] worker already ready stage=%u\n", core,
             __atomic_load_n(&g_fixed_near_hot_stage, __ATOMIC_ACQUIRE));
  return 0;
}

int test_ejit_fixed_near_hot_print(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  const uint32_t core = (uint32_t)g_ucLocalCoreID;
  if (core != FIXED_NEAR_HOT_WORKER_CORE) {
    SRE_printf("[FIXED-NEAR-HOT][core=%u] FAIL print must run on worker %u\n",
               core, FIXED_NEAR_HOT_WORKER_CORE);
    return -8;
  }
  if (__atomic_load_n(&g_fixed_near_hot_stage, __ATOMIC_ACQUIRE) <
      FIXED_NEAR_HOT_PUBLISHED) {
    SRE_printf("[FIXED-NEAR-HOT][core=%u] FAIL producer has not published\n",
               core);
    return -9;
  }
  ejit_code_pool_stats_v2_t stats = {0};
  if (ejit_get_code_pool_stats_v2(&stats) != EJIT_OK ||
      !validate_pool_stats(&stats, core)) {
    SRE_printf("[FIXED-NEAR-HOT][core=%u] FAIL worker pool validation\n", core);
    return -10;
  }
  ejit_taskpool_stats_t taskStats = {0};
  if (ejit_taskpool_get_stats(&taskStats) != EJIT_OK ||
      taskStats.compileFailed || taskStats.publishFailed ||
      taskStats.pendingEntries != 0 || ejit_taskpool_pending_count() != 0) {
    SRE_printf("[FIXED-NEAR-HOT][core=%u] FAIL final taskpool state\n", core);
    ejit_taskpool_print_stats();
    return -11;
  }
  SRE_printf("[FIXED-NEAR-HOT][core=%u] === 17 POOLS / COMPILED ===\n", core);
  ejit_set_log_level(EJIT_LOG_VERBOSE);
  ejit_taskpool_print_stats();
  ejit_print_code_pool_stats();
  ejit_taskpool_print_compiled();
  print_layout_summary(&stats, core);
  SRE_printf("[FIXED-NEAR-HOT][core=%u] PASS worker: pending=0 finalized=17 "
             "failedPoolBitmap=0 (see worker flush summary) sink=0x%llx\n",
             core,
             (unsigned long long)__atomic_load_n(&g_fixed_near_hot_sink,
                                                 __ATOMIC_ACQUIRE));
  __atomic_store_n(&g_fixed_near_hot_stage, FIXED_NEAR_HOT_PRINTED,
                   __ATOMIC_RELEASE);
  return 0;
}

int test_ejit_period(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  const uint32_t core = (uint32_t)g_ucLocalCoreID;
  SRE_printf("\n=== EJIT fixed near-hot 17-pool demo (core=%u) ===\n", core);
  call_init_array_functions();

  ejit_config_t config = {0};
  config.compileMode = EJIT_COMPILE_ASYNC;
  config.optLevel = EJIT_OPT_L2;
  config.enableLogger = true;
  config.forceStaticRegistry = true;
  const ejit_status_t rc = ejit_init_pgo(&config);
  const uint32_t worker = ejit_taskpool_get_worker_core();
  SRE_printf("[FIXED-NEAR-HOT][core=%u] init rc=%d worker=%u stage=%u\n", core,
             (int)rc, worker,
             __atomic_load_n(&g_fixed_near_hot_stage, __ATOMIC_ACQUIRE));
  if (rc != EJIT_OK || worker != FIXED_NEAR_HOT_WORKER_CORE) {
    SRE_printf("[FIXED-NEAR-HOT][core=%u] FAIL init/worker expected=%u\n", core,
               FIXED_NEAR_HOT_WORKER_CORE);
    return -1;
  }
  if (core == FIXED_NEAR_HOT_WORKER_CORE)
    return run_worker();
  if (core == FIXED_NEAR_HOT_PRODUCER_CORE)
    return run_producer();
  SRE_printf("[FIXED-NEAR-HOT][core=%u] skip: use cores %u and %u\n", core,
             FIXED_NEAR_HOT_WORKER_CORE, FIXED_NEAR_HOT_PRODUCER_CORE);
  return 0;
}
