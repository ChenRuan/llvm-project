//===-- ejit_bound_ptr_sre_multicore_test.c - cross-core snapshot demo ----===//
//
// Board flow after a reset:
//   1. Core 8:  test_ejit_period
//      Initializes EJIT, owns the shared compile worker, and arms the dump.
//   2. Core 18: test_ejit_period
//      Enqueues compilation from a stack-local bound object, destroys that
//      object, waits for publication, then verifies a JIT cache hit using a
//      second object whose dynamic field has a different value.
//   3. Core 8:  test_ejit_bound_ptr_print
//      Prints the worker-local optimized IR/ASM.
//
// Do not call ejit_shutdown(): the owner worker and facade must survive across
// shell invocations.
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

#define BOUND_PTR_WORKER_CORE 8u
#define BOUND_PTR_PRODUCER_CORE 18u
#define BOUND_PTR_CELL_A 1u
#define BOUND_PTR_CELL_B 2u
#define BOUND_PTR_TRP 2u
#define BOUND_PTR_WAIT_ROUNDS 6000u
#define BOUND_PTR_WAIT_TICKS 10u

enum BoundPtrDemoStage {
  BOUND_PTR_DEMO_RESET = 0,
  BOUND_PTR_DEMO_WORKER_READY = 1,
  BOUND_PTR_DEMO_COMPILED = 2,
  BOUND_PTR_DEMO_PRINTED = 3,
};

struct CellRelated {
  ejit_may_const uint32_t algorithm;
  ejit_may_const uint32_t scale;
  uint32_t runtimeBias;
};

EJIT_SHARED_SECTION_ATTR ejit_period_arr(cell)
uint32_t g_bound_cells[8];
EJIT_SHARED_SECTION_ATTR ejit_period_arr(trp)
uint32_t g_bound_trps[8];
EJIT_SHARED_SECTION_ATTR volatile uint32_t g_bound_ptr_demo_stage;
EJIT_SHARED_SECTION_ATTR volatile uint64_t g_bound_ptr_demo_sink;

ejit_entry uint32_t
bound_cell_config_mc(ejit_period_arr_ind(cell) uint8_t cellIndex,
                     ejit_period_arr_ind(trp) uint8_t trpIndex,
                     EJIT_BOUND_PTR(cell) const struct CellRelated *cellRelated,
                     uint32_t input) {
  if (cellRelated->algorithm == 7u)
    return input * cellRelated->scale + cellRelated->runtimeBias + cellIndex +
           trpIndex;
  return input + cellRelated->runtimeBias;
}

__attribute__((noinline)) static uint32_t
enqueue_from_stack(uint8_t cell, uint32_t scale, uint32_t runtimeBias) {
  struct CellRelated local = {7u, scale, runtimeBias};
  return bound_cell_config_mc(cell, BOUND_PTR_TRP, &local, 10u);
}

// Encourage reuse of the caller stack after enqueue_from_stack has returned.
// Correctness must not depend on whether this happens to overlap its old slot.
__attribute__((noinline)) static void clobber_producer_stack(void) {
  volatile uint8_t bytes[512];
  for (uint32_t i = 0; i < sizeof(bytes); ++i)
    bytes[i] = (uint8_t)(0xa5u ^ i);
  __asm__ volatile("" : : "r"(&bytes[0]) : "memory");
}

static int wait_for_two_compiles(uint64_t baseline) {
  for (uint32_t round = 0; round < BOUND_PTR_WAIT_ROUNDS; ++round) {
    ejit_taskpool_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    (void)ejit_taskpool_get_stats(&stats);
    if (stats.compileFailed || stats.publishFailed) {
      SRE_printf("[BOUND-PTR-MC] FAIL compile=%llu publish=%llu\n",
                 (unsigned long long)stats.compileFailed,
                 (unsigned long long)stats.publishFailed);
      return 0;
    }
    if (stats.asyncCompiles >= baseline + 2u &&
        ejit_taskpool_pending_count() == 0)
      return 1;
    if ((round % 500u) == 0)
      SRE_printf("[BOUND-PTR-MC] waiting compiles=%llu/2 pending=%u\n",
                 (unsigned long long)(stats.asyncCompiles - baseline),
                 ejit_taskpool_pending_count());
    (void)SRE_TaskDelay(BOUND_PTR_WAIT_TICKS);
  }
  return 0;
}

static int run_producer(void) {
  uint32_t stage = __atomic_load_n(&g_bound_ptr_demo_stage, __ATOMIC_ACQUIRE);
  if (stage != BOUND_PTR_DEMO_WORKER_READY) {
    SRE_printf("[BOUND-PTR-MC][core=18] FAIL stage=%u; run core 8 first\n",
               stage);
    return -3;
  }

  if (ejit_activate("cell", BOUND_PTR_CELL_A) != EJIT_OK ||
      ejit_activate("cell", BOUND_PTR_CELL_B) != EJIT_OK ||
      ejit_activate("trp", BOUND_PTR_TRP) != EJIT_OK) {
    SRE_printf("[BOUND-PTR-MC][core=18] FAIL activate\n");
    return -4;
  }

  ejit_clear_cache();
  ejit_taskpool_stats_t before;
  memset(&before, 0, sizeof(before));
  (void)ejit_taskpool_get_stats(&before);

  // The first call returns AOT while the worker is pending. The local object
  // is dead before this producer starts waiting. Stack clobbering strengthens
  // the lifetime stress, while the taskpool unit test supplies the
  // deterministic enqueue-before-poll ownership proof.
  uint32_t aotA = enqueue_from_stack(BOUND_PTR_CELL_A, 5u, 100u);
  clobber_producer_stack();
  uint32_t aotB = enqueue_from_stack(BOUND_PTR_CELL_B, 9u, 100u);
  clobber_producer_stack();
  const uint32_t expectedAotA = 153u;
  const uint32_t expectedAotB = 194u;
  if (aotA != expectedAotA || aotB != expectedAotB) {
    SRE_printf("[BOUND-PTR-MC][core=18] FAIL AOT cell1=%u/%u cell2=%u/%u\n",
               aotA, expectedAotA, aotB, expectedAotB);
    return -5;
  }

  if (!wait_for_two_compiles(before.asyncCompiles)) {
    SRE_printf("[BOUND-PTR-MC][core=18] FAIL compile timeout\n");
    ejit_taskpool_print_stats();
    return -6;
  }

  ejit_taskpool_stats_t compiled;
  memset(&compiled, 0, sizeof(compiled));
  (void)ejit_taskpool_get_stats(&compiled);

  // Each cell keeps its own stable scale, while runtimeBias deliberately
  // changes. Reusing cell 1's scale=5 specialization for cell 2 would return
  // 254 instead of 294 and fail this check.
  uint32_t jitA = enqueue_from_stack(BOUND_PTR_CELL_A, 5u, 200u);
  uint32_t jitB = enqueue_from_stack(BOUND_PTR_CELL_B, 9u, 200u);
  const uint32_t expectedJitA = 253u;
  const uint32_t expectedJitB = 294u;
  ejit_taskpool_stats_t after;
  memset(&after, 0, sizeof(after));
  (void)ejit_taskpool_get_stats(&after);
  if (jitA != expectedJitA || jitB != expectedJitB) {
    SRE_printf("[BOUND-PTR-MC][core=18] FAIL JIT cell1=%u/%u cell2=%u/%u\n",
               jitA, expectedJitA, jitB, expectedJitB);
    return -7;
  }
  if (after.cacheHits < compiled.cacheHits + 2u) {
    SRE_printf("[BOUND-PTR-MC][core=18] FAIL no cache hit before=%llu "
               "after=%llu; check shared code-pointer support\n",
               (unsigned long long)compiled.cacheHits,
               (unsigned long long)after.cacheHits);
    return -8;
  }

  __atomic_store_n(&g_bound_ptr_demo_sink,
                   ((uint64_t)jitA << 32) | (uint64_t)jitB, __ATOMIC_RELEASE);
  __atomic_store_n(&g_bound_ptr_demo_stage, BOUND_PTR_DEMO_COMPILED,
                   __ATOMIC_RELEASE);
  SRE_printf("[BOUND-PTR-MC][core=18] PASS cell1 AOT/JIT=%u/%u cell2 "
             "AOT/JIT=%u/%u compiles=2 hits=%llu; run "
             "test_ejit_bound_ptr_print on core 8\n",
             aotA, jitA, aotB, jitB,
             (unsigned long long)(after.cacheHits - compiled.cacheHits));
  return 0;
}

static int run_worker(void) {
  uint32_t stage = __atomic_load_n(&g_bound_ptr_demo_stage, __ATOMIC_ACQUIRE);
  if (stage == BOUND_PTR_DEMO_RESET) {
    __atomic_store_n(&g_bound_ptr_demo_sink, 0u, __ATOMIC_RELEASE);
    ejit_dump_func("bound_cell_config_mc");
    __atomic_store_n(&g_bound_ptr_demo_stage, BOUND_PTR_DEMO_WORKER_READY,
                     __ATOMIC_RELEASE);
    SRE_printf("[BOUND-PTR-MC][core=8] worker ready; run test_ejit_period on "
               "core 18\n");
    return 0;
  }

  SRE_printf("[BOUND-PTR-MC][core=8] worker already initialized stage=%u; "
             "use test_ejit_bound_ptr_print for output\n",
             stage);
  return 0;
}

int test_ejit_bound_ptr_print(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;

  const uint32_t core = (uint32_t)g_ucLocalCoreID;
  if (core != BOUND_PTR_WORKER_CORE) {
    SRE_printf("[BOUND-PTR-MC][core=%u] FAIL: dump is worker-local; run "
               "test_ejit_bound_ptr_print on core 8\n",
               core);
    return -9;
  }

  uint32_t stage = __atomic_load_n(&g_bound_ptr_demo_stage, __ATOMIC_ACQUIRE);
  if (stage < BOUND_PTR_DEMO_COMPILED) {
    SRE_printf("[BOUND-PTR-MC][core=8] FAIL stage=%u; run test_ejit_period "
               "on core 18 first\n",
               stage);
    return -10;
  }
  if (stage == BOUND_PTR_DEMO_PRINTED) {
    SRE_printf("[BOUND-PTR-MC][core=8] dump already printed\n");
    return 0;
  }

  ejit_dump_func("");
  SRE_printf("\n[BOUND-PTR-MC] === COMPILED VERSIONS ===\n");
  ejit_taskpool_print_compiled();
  SRE_printf("\n[BOUND-PTR-MC] === OPTIMIZED FUNCTION ===\n");
  ejit_print_dumped("bound_cell_config_mc");
  SRE_printf("[BOUND-PTR-MC][core=8] expect: algorithm/scale loads and "
             "branch removed; runtimeBias load retained. The function dump "
             "is the latest capture (cell=2, scale=9).\n");
  SRE_printf("[BOUND-PTR-MC][core=8] PASS sink=0x%llx\n",
             (unsigned long long)__atomic_load_n(&g_bound_ptr_demo_sink,
                                                 __ATOMIC_ACQUIRE));
  __atomic_store_n(&g_bound_ptr_demo_stage, BOUND_PTR_DEMO_PRINTED,
                   __ATOMIC_RELEASE);
  return 0;
}

int test_ejit_period(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;

  const uint32_t core = (uint32_t)g_ucLocalCoreID;
  SRE_printf("\n=== EJIT bound-pointer multicore demo (core=%u) ===\n", core);
  call_init_array_functions();

  ejit_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.compileMode = EJIT_COMPILE_ASYNC;
  cfg.optLevel = EJIT_OPT_L2;
  cfg.enableLogger = true;
  cfg.forceStaticRegistry = true;

  ejit_status_t rc = ejit_init(&cfg);
  uint32_t worker = ejit_taskpool_get_worker_core();
  SRE_printf("[BOUND-PTR-MC][core=%u] init rc=%d worker=%u stage=%u\n", core,
             (int)rc, worker,
             __atomic_load_n(&g_bound_ptr_demo_stage, __ATOMIC_ACQUIRE));
  if (rc != EJIT_OK)
    return -1;
  if (worker != BOUND_PTR_WORKER_CORE) {
    SRE_printf("[BOUND-PTR-MC] FAIL worker=%u; reset and run core 8 first\n",
               worker);
    return -2;
  }

  if (core == BOUND_PTR_WORKER_CORE)
    return run_worker();
  if (core == BOUND_PTR_PRODUCER_CORE)
    return run_producer();

  SRE_printf("[BOUND-PTR-MC][core=%u] skip: use core 8 or core 18\n", core);
  return 0;
}
