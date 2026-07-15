/**
 * EJIT cache-chain lookup benchmark.
 *
 * Engineered worst case for a weak identity hash: many entry functions with
 * small consecutive funcIndexes, all on ONE period type, each with many
 * instances. A hash that folds funcIndex in raw collapses funcIndex ^ instanceId
 * onto a handful of keys, so every specialization of this family lands in a few
 * buckets -- filling each 16-slot bucket into a 16-deep chain whose slots all
 * share one identityHash. The lookup prefilter (identityHash != key) then
 * rejects nothing and every lookup runs the full funcIndex/dims/versions
 * compare against all 16 slots. A hash that diffuses funcIndex spreads the same
 * specializations across all buckets with distinct keys: short chains, one full
 * compare per lookup.
 *
 * NFUNC*NINST is sized to fill the cache exactly (no eviction) so this measures
 * pure chain-scan cost, not recompile churn. Warm up compiles every
 * specialization; the timed loop is pure cache hits. Uses only public API
 * (ejit_taskpool_get_stats), so the same binary runs against either runtime.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ejit_test_helpers.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"

#define NINST 16
#define NFUNC 16
#define NSPEC (NFUNC * NINST)

struct Cfg {
  ejit_may_const uint32_t v;
};
ejit_period_arr(p) struct Cfg g[NINST];

/* 16 entry functions, all indexed by period `p`, trivial bodies so the per-call
   cost is dominated by the wrapper's cache lookup rather than the JIT'd body. */
#define DEF_F(n)                                                               \
  ejit_entry uint32_t f##n(ejit_period_arr_ind(p) uint8_t i) {                 \
    return g[i].v + (n);                                                       \
  }
DEF_F(0) DEF_F(1) DEF_F(2) DEF_F(3) DEF_F(4) DEF_F(5) DEF_F(6) DEF_F(7)
DEF_F(8) DEF_F(9) DEF_F(10) DEF_F(11) DEF_F(12) DEF_F(13) DEF_F(14) DEF_F(15)

typedef uint32_t (*fn_t)(uint8_t);
static fn_t funcs[NFUNC] = {f0,  f1,  f2,  f3,  f4,  f5,  f6,  f7,
                            f8,  f9,  f10, f11, f12, f13, f14, f15};

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(void) {
  for (int i = 0; i < NINST; i++)
    g[i].v = 100u + (uint32_t)i;

  ejit_config_t cfg;
  ejit_default_config(&cfg);
  ejit_init(&cfg);
  for (int i = 0; i < NINST; i++)
    ejit_activate("p", (uint8_t)i);

  /* Warm up: compile+publish every (func, instance). Drain per call because the
     in-flight dedup is keyed on funcIndex alone. */
  volatile uint32_t sink = 0;
  for (int i = 0; i < NINST; i++)
    for (int f = 0; f < NFUNC; f++) {
      sink += funcs[f]((uint8_t)i);
      ejit_drain_taskpool();
    }

  ejit_taskpool_stats_t s0;
  memset(&s0, 0, sizeof s0);
  ejit_taskpool_get_stats(&s0);
  printf("warmup: expected=%d ready=%u compiles=%llu\n", NSPEC, s0.readyEntries,
         (unsigned long long)s0.asyncCompiles);
  if (s0.readyEntries < (uint32_t)NSPEC)
    printf("  note: %u of %d specializations live (%u evicted during warmup)\n",
           s0.readyEntries, NSPEC, (uint32_t)NSPEC - s0.readyEntries);

  /* Timed loop: pure cache-hit lookups over every specialization. */
  const long REP = 40000;
  double t0 = now_ms();
  for (long r = 0; r < REP; r++)
    for (int i = 0; i < NINST; i++)
      for (int f = 0; f < NFUNC; f++)
        sink += funcs[f]((uint8_t)i);
  double t1 = now_ms();
  (void)sink;

  ejit_taskpool_stats_t s1;
  memset(&s1, 0, sizeof s1);
  ejit_taskpool_get_stats(&s1);

  long lookups = REP * (long)NINST * NFUNC;
  double ms = t1 - t0;
  printf("lookups=%ld  time=%.1f ms  ns/lookup=%.2f\n", lookups, ms,
         ms * 1e6 / (double)lookups);
  printf("loop delta: cacheHits=%llu compiles=%llu (compiles>0 => eviction "
         "churn)\n",
         (unsigned long long)(s1.cacheHits - s0.cacheHits),
         (unsigned long long)(s1.asyncCompiles - s0.asyncCompiles));

  ejit_shutdown();
  return 0;
}
