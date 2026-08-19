//===-- ejit_dump_sre_multicore_test.c - worker-local dump demo ----------===//
//
// Board flow:
//   1. Reset the board and run test_ejit_period on core 8. It initializes the
//      shared worker, enables dump-all capture, and returns "worker ready".
//   2. Run test_ejit_period on core 18. It independently triggers JIT
//      compilation of dump_demo_func_a/b/c and waits for all three compiles.
//   3. Run test_ejit_period on core 8 again. The worker-local payloads are
//      printed there: three function-only views, then the complete module for
//      dump_demo_func_b.
//
// Do not call ejit_shutdown(): the owner worker and attached facade must stay
// alive across shell invocations.
//
//===----------------------------------------------------------------------===//

#include <stdint.h>
#include <string.h>

#include "ejit_test_helpers.h"

extern void SRE_printf(const char *format, ...);
extern uint32_t SRE_TaskDelay(uint32_t tick);
extern void call_init_array_functions(void);
extern uint8_t g_ucLocalCoreID;

#ifndef EJIT_SHARED_SECTION_ATTR
#define EJIT_SHARED_SECTION_ATTR __attribute__((section(".mc_shared")))
#endif

#define DUMP_WORKER_CORE 8u
#define DUMP_PRODUCER_CORE 18u
#define DUMP_WAIT_ROUNDS 6000u
#define DUMP_WAIT_TICKS 10u

enum DumpDemoStage {
  DUMP_DEMO_RESET = 0,
  DUMP_DEMO_WORKER_READY = 1,
  DUMP_DEMO_COMPILED = 2,
  DUMP_DEMO_PRINTED = 3,
};

struct DumpDemoCfg {
  ejit_may_const uint32_t multiplier;
  ejit_may_const uint32_t mode;
  uint32_t bias;
};

EJIT_SHARED_SECTION_ATTR ejit_period_arr(cell)
struct DumpDemoCfg g_dump_demo_cfg[1];
EJIT_SHARED_SECTION_ATTR volatile uint32_t g_dump_demo_stage;
EJIT_SHARED_SECTION_ATTR volatile uint64_t g_dump_demo_sink;

__attribute__((noinline)) static uint32_t
dump_demo_helper(uint32_t value, uint32_t bias) {
  return value * 3u + bias;
}

ejit_entry uint32_t
dump_demo_func_a(ejit_period_arr_ind(cell) uint8_t cell, uint32_t value) {
  const struct DumpDemoCfg *cfg = &g_dump_demo_cfg[cell];
  return dump_demo_helper(value * cfg->multiplier, cfg->bias) + 0xa1u;
}

ejit_entry uint32_t
dump_demo_func_b(ejit_period_arr_ind(cell) uint8_t cell, uint32_t value) {
  const struct DumpDemoCfg *cfg = &g_dump_demo_cfg[cell];
  uint32_t result = dump_demo_helper(value * cfg->multiplier, cfg->bias);
  return cfg->mode ? result + 0xb2u : result - 0xb2u;
}

ejit_entry uint32_t
dump_demo_func_c(ejit_period_arr_ind(cell) uint8_t cell, uint32_t value) {
  const struct DumpDemoCfg *cfg = &g_dump_demo_cfg[cell];
  return dump_demo_helper(value + cfg->multiplier, cfg->bias) ^ 0xc3u;
}

static int wait_for_three_compiles(uint64_t baseline) {
  for (uint32_t round = 0; round < DUMP_WAIT_ROUNDS; ++round) {
    ejit_taskpool_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    (void)ejit_taskpool_get_stats(&stats);
    if (stats.compileFailed || stats.publishFailed) {
      SRE_printf("[DUMP-MC] FAIL compile=%llu publish=%llu\n",
                 (unsigned long long)stats.compileFailed,
                 (unsigned long long)stats.publishFailed);
      return 0;
    }
    if (stats.asyncCompiles >= baseline + 3u &&
        ejit_taskpool_pending_count() == 0)
      return 1;
    if ((round % 500u) == 0)
      SRE_printf("[DUMP-MC] waiting compiles=%llu/%llu pending=%u\n",
                 (unsigned long long)(stats.asyncCompiles - baseline), 3ull,
                 ejit_taskpool_pending_count());
    (void)SRE_TaskDelay(DUMP_WAIT_TICKS);
  }
  return 0;
}

static int run_producer(void) {
  uint32_t stage = __atomic_load_n(&g_dump_demo_stage, __ATOMIC_ACQUIRE);
  if (stage != DUMP_DEMO_WORKER_READY) {
    SRE_printf("[DUMP-MC][core=18] FAIL stage=%u; run core 8 first\n", stage);
    return -3;
  }

  if (ejit_activate("cell", 0) != EJIT_OK)
    return -4;

  ejit_clear_cache();
  ejit_taskpool_stats_t before;
  memset(&before, 0, sizeof(before));
  (void)ejit_taskpool_get_stats(&before);

  uint32_t a = dump_demo_func_a(0, 11u);
  uint32_t b = dump_demo_func_b(0, 13u);
  uint32_t c = dump_demo_func_c(0, 17u);
  uint32_t expectedA = dump_demo_helper(11u * 5u, 7u) + 0xa1u;
  uint32_t expectedB = dump_demo_helper(13u * 5u, 7u) + 0xb2u;
  uint32_t expectedC = dump_demo_helper(17u + 5u, 7u) ^ 0xc3u;
  if (a != expectedA || b != expectedB || c != expectedC) {
    SRE_printf("[DUMP-MC][core=18] FAIL values a=%u/%u b=%u/%u c=%u/%u\n",
               a, expectedA, b, expectedB, c, expectedC);
    return -5;
  }

  if (!wait_for_three_compiles(before.asyncCompiles)) {
    SRE_printf("[DUMP-MC][core=18] FAIL compile timeout\n");
    ejit_taskpool_print_stats();
    return -6;
  }

  __atomic_store_n(&g_dump_demo_sink, (uint64_t)a ^ b ^ c, __ATOMIC_RELEASE);
  __atomic_store_n(&g_dump_demo_stage, DUMP_DEMO_COMPILED, __ATOMIC_RELEASE);
  SRE_printf("[DUMP-MC][core=18] PASS: three entries compiled; now run "
             "test_ejit_period on core 8 again\n");
  return 0;
}

static int run_worker(void) {
  uint32_t stage = __atomic_load_n(&g_dump_demo_stage, __ATOMIC_ACQUIRE);
  if (stage == DUMP_DEMO_RESET) {
    g_dump_demo_cfg[0].multiplier = 5u;
    g_dump_demo_cfg[0].mode = 1u;
    g_dump_demo_cfg[0].bias = 7u;
    __atomic_store_n(&g_dump_demo_sink, 0u, __ATOMIC_RELEASE);
    ejit_dump_all(true);
    __atomic_store_n(&g_dump_demo_stage, DUMP_DEMO_WORKER_READY,
                     __ATOMIC_RELEASE);
    SRE_printf("[DUMP-MC][core=8] worker ready; run test_ejit_period on "
               "core 18\n");
    return 0;
  }

  if (stage != DUMP_DEMO_COMPILED) {
    SRE_printf("[DUMP-MC][core=8] stage=%u; waiting for core 18\n", stage);
    return stage == DUMP_DEMO_PRINTED ? 0 : -7;
  }

  ejit_dump_all(false);
  SRE_printf("\n[DUMP-MC] === FUNCTION VIEW A ===\n");
  ejit_print_dumped("dump_demo_func_a");
  SRE_printf("\n[DUMP-MC] === FUNCTION VIEW B ===\n");
  ejit_print_dumped("dump_demo_func_b");
  SRE_printf("\n[DUMP-MC] === FUNCTION VIEW C ===\n");
  ejit_print_dumped("dump_demo_func_c");
  SRE_printf("\n[DUMP-MC] === COMPLETE MODULE VIEW B ===\n");
  ejit_print_dumped_module("dump_demo_func_b");
  SRE_printf("[DUMP-MC][core=8] PASS sink=0x%llx\n",
             (unsigned long long)__atomic_load_n(&g_dump_demo_sink,
                                                  __ATOMIC_ACQUIRE));
  __atomic_store_n(&g_dump_demo_stage, DUMP_DEMO_PRINTED, __ATOMIC_RELEASE);
  return 0;
}

int test_ejit_period(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;

  const uint32_t core = (uint32_t)g_ucLocalCoreID;
  SRE_printf("\n=== EJIT dump views multicore demo (core=%u) ===\n", core);
  call_init_array_functions();

  ejit_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.compileMode = EJIT_COMPILE_ASYNC;
  cfg.optLevel = EJIT_OPT_L2;
  cfg.enableLogger = true;
  cfg.forceStaticRegistry = true;

  ejit_status_t rc = ejit_init(&cfg);
  uint32_t worker = ejit_taskpool_get_worker_core();
  SRE_printf("[DUMP-MC][core=%u] init rc=%d worker=%u stage=%u\n", core,
             (int)rc, worker,
             __atomic_load_n(&g_dump_demo_stage, __ATOMIC_ACQUIRE));
  if (rc != EJIT_OK)
    return -1;
  if (worker != DUMP_WORKER_CORE) {
    SRE_printf("[DUMP-MC] FAIL worker=%u; reset and run core 8 first\n",
               worker);
    return -2;
  }

  if (core == DUMP_WORKER_CORE)
    return run_worker();
  if (core == DUMP_PRODUCER_CORE)
    return run_producer();

  SRE_printf("[DUMP-MC][core=%u] skip: use core 8 or core 18\n", core);
  return 0;
}
