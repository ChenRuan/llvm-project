/**
 * EJIT bitcode-template cache benchmark for the SRE shared taskpool.
 *
 * Run test_ejit_period on all participating cores. Each non-worker core owns
 * one period instance selected by core id and requests three generations of
 * the same ejit_entry specialization. With roughly 30 business cores this
 * produces roughly 90 compiles from one registered bitcode blob: the first
 * compile parses the blob and the rest should reuse the cached pristine module
 * template. Build LLVMEJIT with EJIT_DIAG_ENABLE to collect compile_timing
 * load_bitcode/lookup/compile_total records from the worker core.
 *
 * The test intentionally never calls ejit_shutdown: a peer core must not tear
 * down the process-global shared worker while other cores are still running.
 */

#include <stdbool.h>
#include <stdint.h>

#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"

extern void SRE_printf(const char *format, ...);
extern uint32_t SRE_TaskDelay(uint32_t tick);
extern void call_init_array_functions(void);
extern uint8_t g_ucLocalCoreID;

#define TEMPLATE_INSTANCES 32u
#define TEMPLATE_GENERATIONS 3u
#define ENQUEUE_SETTLE_TICKS 1000u
#define WAIT_ROUNDS 600u
#define WAIT_TICKS 100u
#define IDLE_TICKS 40000u

struct TemplateCfg {
  ejit_may_const uint32_t multiplier;
  ejit_may_const uint32_t bias;
  uint32_t generation;
};

ejit_period_arr(tp) struct TemplateCfg g_templateCfg[TEMPLATE_INSTANCES];

ejit_entry __attribute__((noinline)) uint64_t
template_multiversion(ejit_period_arr_ind(tp) uint8_t instance,
                      uint32_t input) {
  const struct TemplateCfg *cfg = &g_templateCfg[instance];
  uint64_t value = (uint64_t)input * cfg->multiplier + cfg->bias;
  value ^= value << 13;
  value ^= value >> 7;
  return value + cfg->generation;
}

static uint32_t local_core_id(void) { return (uint32_t)g_ucLocalCoreID; }

static uint64_t expected_value(uint8_t instance, uint32_t input) {
  const struct TemplateCfg *cfg = &g_templateCfg[instance];
  uint64_t value = (uint64_t)input * cfg->multiplier + cfg->bias;
  value ^= value << 13;
  value ^= value >> 7;
  return value + cfg->generation;
}

static void idle_forever(uint32_t core, const char *role) {
  for (;;) {
    SRE_TaskDelay(IDLE_TICKS);
    SRE_printf("[TEMPLATE][core=%u] idle role=%s\n", core, role);
  }
}

static bool wait_for_worker_drain(uint32_t core, uint32_t generation) {
  /* Let every producer enqueue its specialization before observing drain. */
  SRE_TaskDelay(ENQUEUE_SETTLE_TICKS);

  uint32_t zeroRounds = 0;
  for (uint32_t round = 0; round < WAIT_ROUNDS; ++round) {
    uint32_t pending = ejit_taskpool_pending_count();
    if (pending == 0) {
      if (++zeroRounds == 3) {
        ejit_taskpool_stats_t stats = {0};
        ejit_taskpool_get_stats(&stats);
        SRE_printf("[TEMPLATE][core=%u] gen=%u drained round=%u "
                   "ready=%u compiles=%llu hits=%llu pending=%u\n",
                   core, generation, round, stats.readyEntries,
                   (unsigned long long)stats.asyncCompiles,
                   (unsigned long long)stats.cacheHits, pending);
        return true;
      }
    } else {
      zeroRounds = 0;
    }

    if ((round % 50u) == 0u)
      SRE_printf("[TEMPLATE][core=%u] gen=%u waiting round=%u pending=%u\n",
                 core, generation, round, pending);
    SRE_TaskDelay(WAIT_TICKS);
  }

  SRE_printf("[TEMPLATE][core=%u] gen=%u FAIL: worker drain timeout\n", core,
             generation);
  return false;
}

int test_ejit_period(uint8_t cellIdxArg, uint8_t trpIdxArg,
                     uint8_t sliceIdxArg, uint8_t carrierIdxArg) {
  (void)cellIdxArg;
  (void)trpIdxArg;
  (void)sliceIdxArg;
  (void)carrierIdxArg;

  const uint32_t core = local_core_id();
  const uint8_t instance = (uint8_t)(core % TEMPLATE_INSTANCES);

  SRE_printf("\n=== EJIT SRE Multi-Core Bitcode Template Benchmark ===\n");
  SRE_printf("[TEMPLATE][core=%u] enter instance=%u generations=%u\n", core,
             instance, TEMPLATE_GENERATIONS);

  call_init_array_functions();

  ejit_config_t config = {
      .compileMode = EJIT_COMPILE_ASYNC,
      .optLevel = EJIT_OPT_L2,
      .enableLogger = false,
      .forceStaticRegistry = true,
  };
  ejit_status_t initRc = ejit_init(&config);
  uint32_t workerCore = ejit_taskpool_get_worker_core();
  SRE_printf("[TEMPLATE][core=%u] init rc=%d workerCore=%u\n", core,
             (int)initRc, workerCore);
  if (initRc != EJIT_OK)
    idle_forever(core, "init-failed");
  if (core == workerCore)
    idle_forever(core, "compile-worker");

  for (uint32_t generation = 0; generation < TEMPLATE_GENERATIONS;
       ++generation) {
    struct TemplateCfg *cfg = &g_templateCfg[instance];
    cfg->multiplier = 17u + core * 3u + generation * 11u;
    cfg->bias = 0x100u + core * 7u + generation * 13u;
    cfg->generation = generation;

    ejit_status_t activeRc;
    if (generation == 0) {
      activeRc = ejit_activate("tp", instance);
    } else {
      ejit_status_t inactiveRc = ejit_deactivate("tp", instance);
      activeRc = ejit_activate("tp", instance);
      SRE_printf("[TEMPLATE][core=%u] gen=%u version bump deactivate=%d "
                 "activate=%d\n",
                 core, generation, (int)inactiveRc, (int)activeRc);
    }
    if (activeRc != EJIT_OK)
      idle_forever(core, "activate-failed");

    const uint32_t input = 1000u + core * 19u + generation;
    const uint64_t expected = expected_value(instance, input);
    uint64_t first = template_multiversion(instance, input);
    SRE_printf("[TEMPLATE][core=%u] gen=%u first=0x%llx expected=0x%llx "
               "[%s]\n",
               core, generation, (unsigned long long)first,
               (unsigned long long)expected,
               first == expected ? "MATCH" : "MISMATCH");
    if (first != expected)
      idle_forever(core, "fallback-mismatch");

    if (!wait_for_worker_drain(core, generation))
      idle_forever(core, "compile-timeout");

    uint64_t hit = template_multiversion(instance, input);
    SRE_printf("[TEMPLATE][core=%u] gen=%u stable=0x%llx expected=0x%llx "
               "[%s]\n",
               core, generation, (unsigned long long)hit,
               (unsigned long long)expected,
               hit == expected ? "MATCH" : "MISMATCH");
    if (hit != expected)
      idle_forever(core, "jit-mismatch");
  }

  ejit_taskpool_stats_t stats = {0};
  ejit_taskpool_get_stats(&stats);
  SRE_printf("[TEMPLATE][core=%u] PASS instance=%u ready=%u compiles=%llu "
             "enqueues=%llu pending=%u alreadyPending=%llu failed=%llu\n",
             core, instance, stats.readyEntries,
             (unsigned long long)stats.asyncCompiles,
             (unsigned long long)stats.asyncEnqueues, stats.pendingEntries,
             (unsigned long long)stats.alreadyPending,
             (unsigned long long)stats.compileFailed);

  idle_forever(core, "benchmark-complete");
  return 0;
}
