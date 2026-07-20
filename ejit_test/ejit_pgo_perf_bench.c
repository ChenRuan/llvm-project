/**
 * EJIT online-PGO board benchmark.
 *
 * The benchmark separates the three execution phases:
 *   AOT fallback -> Tier-1 instrumented JIT -> Tier-2 PGO-use JIT.
 * Tier-2 is armed by the 64th Tier-1 cache hit in the current implementation.
 * The function body contains a strongly biased runtime branch so PGO has useful
 * control-flow data; may_const fields provide the normal EJIT specialization.
 */

#include <stdint.h>
#include <string.h>

#include "ejit_test_helpers.h"

#if defined(EJIT_FREESTANDING)
extern uint64_t SRE_CycleCountGet64(void);
extern int SRE_printf(const char *format, ...);
extern uint32_t SRE_TaskDelay(uint32_t tick);
#define BENCH_PRINT(...) SRE_printf(__VA_ARGS__)
static uint64_t benchNow(void) { return SRE_CycleCountGet64(); }
static void benchYield(void) { (void)SRE_TaskDelay(1); }
#else
#include <stdio.h>
#include <time.h>
#define BENCH_PRINT(...) printf(__VA_ARGS__)
static uint64_t benchNow(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
static void benchYield(void) {
  struct timespec ts = {0, 1000000};
  nanosleep(&ts, 0);
}
#endif

#define PGO_INNER_ITERS 5000u
#define PGO_TIER1_SAMPLES 24u
#define PGO_TRIGGER_CALLS 80u
#define PGO_TIER2_SAMPLES 1000u
#define PGO_WAIT_TICKS 2000u

struct PgoCellCfg {
  ejit_may_const uint32_t hotMul;
  ejit_may_const uint32_t coldMul;
  ejit_may_const uint32_t biasLimit;
  uint32_t salt;
};

ejit_period_arr(cell) struct PgoCellCfg g_pgoCells[16];

static volatile uint64_t g_pgoSink;

ejit_entry uint64_t
pgo_biased_work(ejit_period_arr_ind(cell) uint8_t cellIdx, uint32_t seed) {
  const struct PgoCellCfg *cfg = &g_pgoCells[cellIdx];
  uint64_t sum = seed + cfg->salt;

  for (uint32_t i = 0; i < PGO_INNER_ITERS; ++i) {
    uint32_t v = (seed + i * 17u + (i >> 3)) & 1023u;
    if (v < cfg->biasLimit) {
      sum += (uint64_t)(v + 3u) * cfg->hotMul;
      sum ^= sum >> 11;
    } else {
      sum += (uint64_t)(v ^ 0x5a5u) * cfg->coldMul;
      sum = (sum << 7) | (sum >> 57);
      sum ^= 0x9e3779b97f4a7c15ULL;
    }
  }
  return sum;
}

static uint64_t pgo_biased_work_aot(uint8_t cellIdx, uint32_t seed) {
  const struct PgoCellCfg *cfg = &g_pgoCells[cellIdx];
  uint64_t sum = seed + cfg->salt;

  for (uint32_t i = 0; i < PGO_INNER_ITERS; ++i) {
    uint32_t v = (seed + i * 17u + (i >> 3)) & 1023u;
    if (v < cfg->biasLimit) {
      sum += (uint64_t)(v + 3u) * cfg->hotMul;
      sum ^= sum >> 11;
    } else {
      sum += (uint64_t)(v ^ 0x5a5u) * cfg->coldMul;
      sum = (sum << 7) | (sum >> 57);
      sum ^= 0x9e3779b97f4a7c15ULL;
    }
  }
  return sum;
}

struct BenchResult {
  uint64_t total;
  uint64_t min;
  uint64_t max;
  uint64_t checksum;
};

typedef uint64_t (*BenchFn)(uint8_t, uint32_t);

static struct BenchResult runBench(const char *name, BenchFn fn, uint8_t cellIdx,
                                   uint32_t samples, uint32_t seedBase) {
  struct BenchResult r = {0, UINT64_MAX, 0, 0};
  for (uint32_t i = 0; i < samples; ++i) {
    uint64_t t0 = benchNow();
    uint64_t value = fn(cellIdx, seedBase + i);
    uint64_t dt = benchNow() - t0;
    r.total += dt;
    if (dt < r.min)
      r.min = dt;
    if (dt > r.max)
      r.max = dt;
    r.checksum ^= value + ((uint64_t)i << 32);
  }
  g_pgoSink ^= r.checksum;
  BENCH_PRINT("[PGO-BENCH] %s samples=%u avg=%llu min=%llu max=%llu "
              "checksum=0x%llx\n",
              name, samples, (unsigned long long)(r.total / samples),
              (unsigned long long)r.min, (unsigned long long)r.max,
              (unsigned long long)r.checksum);
  return r;
}

static int waitForCompiles(uint64_t expected) {
  for (uint32_t i = 0; i < PGO_WAIT_TICKS; ++i) {
    ejit_taskpool_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    if (ejit_taskpool_get_stats(&stats) == EJIT_OK &&
        stats.asyncCompiles >= expected)
      return 1;
    benchYield();
  }
  return 0;
}

int test_ejit_period(void) {
  const uint8_t cellIdx = 0;
  ejit_config_t cfg;
  ejit_taskpool_stats_t stats;
  struct BenchResult aot;
  struct BenchResult tier1;
  struct BenchResult tier2;

  BENCH_PRINT("=== EJIT Online-PGO Performance Benchmark ===\n");
  BENCH_PRINT("inner=%u tier1_samples=%u trigger_calls=%u tier2_samples=%u\n",
              PGO_INNER_ITERS, PGO_TIER1_SAMPLES, PGO_TRIGGER_CALLS,
              PGO_TIER2_SAMPLES);

  g_pgoCells[cellIdx].hotMul = 3;
  g_pgoCells[cellIdx].coldMul = 11;
  g_pgoCells[cellIdx].biasLimit = 1000; // About 97.7% hot / 2.3% cold.
  g_pgoCells[cellIdx].salt = 0x12345678u;

  aot = runBench("AOT-direct", pgo_biased_work_aot, cellIdx,
                 PGO_TIER1_SAMPLES, 0x2000u);

  ejit_default_config(&cfg);
  if (ejit_init_pgo(&cfg) != EJIT_OK) {
    BENCH_PRINT("[PGO-BENCH] FAIL: ejit_init_pgo\n");
    return -1;
  }
  if (ejit_activate("cell", cellIdx) != EJIT_OK) {
    BENCH_PRINT("[PGO-BENCH] FAIL: activate cell=%u\n", cellIdx);
    ejit_shutdown();
    return -1;
  }

  // First call is allowed to use AOT fallback while Tier-1 is queued.
  g_pgoSink ^= pgo_biased_work(cellIdx, 0x1000u);
  if (!waitForCompiles(1)) {
    BENCH_PRINT("[PGO-BENCH] FAIL: Tier-1 did not publish\n");
    ejit_taskpool_print_stats();
    ejit_shutdown();
    return -1;
  }

  // Stay below the default 64-hit threshold, so this is Tier-1 only.
  tier1 = runBench("Tier1-instrumented", pgo_biased_work, cellIdx,
                   PGO_TIER1_SAMPLES, 0x2000u);

  // Cross the hit threshold. Tier-1 remains callable while Tier-2 compiles.
  for (uint32_t i = 0; i < PGO_TRIGGER_CALLS; ++i)
    g_pgoSink ^= pgo_biased_work(cellIdx, 0x3000u + i);

  if (!waitForCompiles(2)) {
    BENCH_PRINT("[PGO-BENCH] FAIL: Tier-2 did not publish\n");
    ejit_taskpool_print_stats();
    ejit_shutdown();
    return -1;
  }

  memset(&stats, 0, sizeof(stats));
  (void)ejit_taskpool_get_stats(&stats);
  BENCH_PRINT("[PGO-BENCH] Tier2 ready: compiles=%llu hits=%llu ready=%u "
              "failed=%llu publish_failed=%llu\n",
              (unsigned long long)stats.asyncCompiles,
              (unsigned long long)stats.cacheHits, stats.readyEntries,
              (unsigned long long)stats.compileFailed,
              (unsigned long long)stats.publishFailed);

  tier2 = runBench("Tier2-PGOUse-compare", pgo_biased_work, cellIdx,
                   PGO_TIER1_SAMPLES, 0x2000u);
  if (aot.checksum != tier1.checksum || tier1.checksum != tier2.checksum) {
    BENCH_PRINT("[PGO-BENCH] FAIL: checksum mismatch AOT=0x%llx "
                "Tier1=0x%llx Tier2=0x%llx\n",
                (unsigned long long)aot.checksum,
                (unsigned long long)tier1.checksum,
                (unsigned long long)tier2.checksum);
    ejit_shutdown();
    return -1;
  }

  {
    uint64_t tier1Avg = tier1.total / PGO_TIER1_SAMPLES;
    uint64_t tier2Avg = tier2.total / PGO_TIER1_SAMPLES;
    uint64_t ratioPermille = tier2Avg ? tier1Avg * 1000u / tier2Avg : 0;
    BENCH_PRINT("[PGO-BENCH] Tier1/Tier2 speed ratio=%llu.%03llux "
                "(greater than 1.000 means Tier2 is faster)\n",
                (unsigned long long)(ratioPermille / 1000u),
                (unsigned long long)(ratioPermille % 1000u));
  }

  runBench("Tier2-PGOUse-steady", pgo_biased_work, cellIdx,
           PGO_TIER2_SAMPLES, 0x4000u);

  ejit_shutdown();
  BENCH_PRINT("=== PGO Benchmark Complete sink=0x%llx ===\n",
              (unsigned long long)g_pgoSink);
  return 0;
}

