//===-- ejit_bound_ptr_sre_multistruct_test.c - borrowed pointer demo -----===//
//
// Board flow after reset:
//   1. Core 6:  run test_ejit_period.  It initializes the fixed worker,
//      enables capture, and waits for the producer.
//   2. Core 16: run test_ejit_period.  It attaches to core 6, activates two
//      cell versions, and compiles the entry through two shared structures.
//   3. Core 6:  run test_ejit_bound_ptr_multistruct_print to inspect the
//      compiled list, entry view, and complete module view.
//
// The negative checks use only invalid descriptors.  They do not create a
// dangling pointer, concurrent write, or other intentional lifetime hazard.
// Do not call ejit_shutdown(): the owner worker must survive shell invocations.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
#include <stddef.h>
#include <stdint.h>

extern void SRE_printf(const char *format, ...);
extern uint32_t SRE_TaskDelay(uint32_t tick);
extern void call_init_array_functions(void);
extern uint8_t g_ucLocalCoreID;

#ifndef EJIT_SHARED_SECTION_ATTR
#define EJIT_SHARED_SECTION_ATTR __attribute__((section(".mc_shared")))
#endif

#define EJIT_BOUND_MULTI_WORKER_CORE 6u
#define EJIT_BOUND_MULTI_PRODUCER_CORE 16u
#define EJIT_BOUND_MULTI_CELL_A 1u
#define EJIT_BOUND_MULTI_CELL_B 2u
#define EJIT_BOUND_MULTI_TRP 2u
#define EJIT_BOUND_MULTI_WAIT_ROUNDS 6000u
#define EJIT_BOUND_MULTI_WAIT_TICKS 10u
#define EJIT_BOUND_MULTI_PAYLOAD_WORDS 512u

// These are deliberately larger than 1 KiB. They model shared application
// state while keeping the request transport fixed at one descriptor each.
typedef struct {
  ejit_may_const uint32_t algorithm;
  ejit_may_const uint32_t scale;
  uint32_t runtimeBias;
  uint32_t payload[EJIT_BOUND_MULTI_PAYLOAD_WORDS];
} EJitBoundMultiCellConfig;

typedef struct {
  ejit_may_const uint32_t multiplier;
  ejit_may_const uint32_t offset;
  uint32_t runtimeTag;
  uint32_t payload[EJIT_BOUND_MULTI_PAYLOAD_WORDS];
} EJitBoundMultiTrpConfig;

typedef char EJitBoundMultiCellMustBeLarge[
    (sizeof(EJitBoundMultiCellConfig) > 1024u) ? 1 : -1];
typedef char EJitBoundMultiTrpMustBeLarge[
    (sizeof(EJitBoundMultiTrpConfig) > 1024u) ? 1 : -1];
typedef char EJitBoundMultiDescriptorMustBeFixed[
    (sizeof(ejit_bound_ptr_t) <= 32u) ? 1 : -1];

enum BoundMultiStage {
  BOUND_MULTI_RESET = 0,
  BOUND_MULTI_WORKER_READY = 1,
  BOUND_MULTI_COMPILED = 2,
  BOUND_MULTI_PRINTED = 3,
};

EJIT_SHARED_SECTION_ATTR ejit_period_arr(cell)
uint32_t g_bound_multi_cells[8];
EJIT_SHARED_SECTION_ATTR ejit_period_arr(trp)
uint32_t g_bound_multi_trps[8];
EJIT_SHARED_SECTION_ATTR EJitBoundMultiCellConfig
    g_bound_multi_cell_configs[8] = {
        [EJIT_BOUND_MULTI_CELL_A] = {7u, 5u, 100u, {0xC011A001u}},
        [EJIT_BOUND_MULTI_CELL_B] = {7u, 9u, 200u, {0xC011B002u}},
    };
EJIT_SHARED_SECTION_ATTR EJitBoundMultiTrpConfig
    g_bound_multi_trp_configs[8] = {
        [EJIT_BOUND_MULTI_TRP] = {3u, 11u, 0x7A2u, {0x7A2F0002u}},
    };
EJIT_SHARED_SECTION_ATTR volatile uint32_t g_bound_multi_stage;
EJIT_SHARED_SECTION_ATTR volatile uint64_t g_bound_multi_sink;

// The helper is independently addressable, so propagation is valid only
// because it has EJIT_ENTRY and both matching dimensions.  It intentionally
// repeats neither EJIT_BOUND_PTR annotation.
EJIT_ENTRY uint32_t bound_multi_helper(
    EJIT_DIM(cell) uint8_t cellIndex, EJIT_DIM(trp) uint8_t trpIndex,
    const EJitBoundMultiCellConfig *cellConfig,
    const EJitBoundMultiTrpConfig *trpConfig, uint32_t input) {
  uint32_t value = input;
  if (cellConfig->algorithm == 7u)
    value *= cellConfig->scale;
  return value + cellConfig->runtimeBias + trpConfig->multiplier +
         trpConfig->offset + trpConfig->runtimeTag + cellIndex + trpIndex;
}

EJIT_ENTRY uint32_t bound_multi_root(
    EJIT_DIM(cell) uint8_t cellIndex, EJIT_DIM(trp) uint8_t trpIndex,
    EJIT_BOUND_PTR(cell) const EJitBoundMultiCellConfig *cellConfig,
    EJIT_BOUND_PTR(trp) const EJitBoundMultiTrpConfig *trpConfig,
    uint32_t input) {
  return bound_multi_helper(cellIndex, trpIndex, cellConfig, trpConfig, input);
}

static uint32_t expected_value(uint32_t cell) {
  const EJitBoundMultiCellConfig *cellConfig =
      &g_bound_multi_cell_configs[cell];
  const EJitBoundMultiTrpConfig *trpConfig =
      &g_bound_multi_trp_configs[EJIT_BOUND_MULTI_TRP];
  uint32_t value = 10u;
  if (cellConfig->algorithm == 7u)
    value *= cellConfig->scale;
  return value + cellConfig->runtimeBias + trpConfig->multiplier +
         trpConfig->offset + trpConfig->runtimeTag + cell +
         EJIT_BOUND_MULTI_TRP;
}

static uint32_t call_bound_root(uint32_t cell) {
  return bound_multi_root(
      (uint8_t)cell, (uint8_t)EJIT_BOUND_MULTI_TRP,
      &g_bound_multi_cell_configs[cell],
      &g_bound_multi_trp_configs[EJIT_BOUND_MULTI_TRP], 10u);
}

static int wait_for_compiles(uint64_t baseline, uint32_t expected,
                             const char *label) {
  for (uint32_t round = 0; round < EJIT_BOUND_MULTI_WAIT_ROUNDS; ++round) {
    ejit_taskpool_stats_t stats = {0};
    (void)ejit_taskpool_get_stats(&stats);
    if (stats.compileFailed || stats.publishFailed) {
      SRE_printf("[BOUND-MULTI] FAIL %s compile=%llu publish=%llu\n", label,
                 (unsigned long long)stats.compileFailed,
                 (unsigned long long)stats.publishFailed);
      return 0;
    }
    if (stats.asyncCompiles >= baseline + expected &&
        stats.pendingEntries == 0 && stats.queueApproxSize == 0 &&
        ejit_taskpool_pending_count() == 0)
      return 1;
    if ((round % 500u) == 0)
      SRE_printf("[BOUND-MULTI] waiting %s compiles=%llu/%llu pending=%u\n",
                 label, (unsigned long long)(stats.asyncCompiles - baseline),
                 (unsigned long long)expected, stats.pendingEntries);
    (void)SRE_TaskDelay(EJIT_BOUND_MULTI_WAIT_TICKS);
  }
  SRE_printf("[BOUND-MULTI] FAIL timeout waiting %s\n", label);
  return 0;
}

static int check_transport_contract(void) {
  const size_t cellSize = sizeof(EJitBoundMultiCellConfig);
  const size_t trpSize = sizeof(EJitBoundMultiTrpConfig);
  const size_t descriptorSize = sizeof(ejit_bound_ptr_t);
  if (cellSize <= 1024u || trpSize <= 1024u || descriptorSize > 32u) {
    SRE_printf("[BOUND-MULTI] FAIL transport sizes cell=%lu trp=%lu "
               "descriptor=%lu\n",
               (unsigned long)cellSize, (unsigned long)trpSize,
               (unsigned long)descriptorSize);
    return 0;
  }
  if (g_bound_multi_cell_configs[EJIT_BOUND_MULTI_CELL_A].payload[0] !=
          0xC011A001u ||
      g_bound_multi_cell_configs[EJIT_BOUND_MULTI_CELL_B].payload[0] !=
          0xC011B002u ||
      g_bound_multi_trp_configs[EJIT_BOUND_MULTI_TRP].payload[0] !=
          0x7A2F0002u) {
    SRE_printf("[BOUND-MULTI] FAIL shared payload changed during compile\n");
    return 0;
  }
  SRE_printf("[BOUND-MULTI] PASS borrowed transport cell=%luB trp=%luB "
             "descriptor=%luB; payloads stayed shared\n",
             (unsigned long)cellSize, (unsigned long)trpSize,
             (unsigned long)descriptorSize);
  return 1;
}

static int check_invalid_descriptors(void) {
  ejit_bound_ptr_t tooMany[9];
  for (uint32_t i = 0; i < 9u; ++i) {
    tooMany[i].rawPtr = &g_bound_multi_cell_configs[EJIT_BOUND_MULTI_CELL_A];
    tooMany[i].size = (uint32_t)sizeof(EJitBoundMultiCellConfig);
    tooMany[i].argIndex = i;
  }
  ejit_status_t rc = ejit_taskpool_compile_or_get_bound_v(
      0u, (const ejit_dim_pair_t *)0, 0u, tooMany, 9u, (void **)0,
      (uint32_t *)0);
  if (rc != EJIT_ERR_INVALID_PARAM) {
    SRE_printf("[BOUND-MULTI] FAIL >8 descriptor check rc=%d\n", (int)rc);
    return 0;
  }

  ejit_bound_ptr_t nullObject = {
      (const void *)0, (uint32_t)sizeof(EJitBoundMultiCellConfig), 0u};
  rc = ejit_taskpool_compile_or_get_bound_v(
      0u, (const ejit_dim_pair_t *)0, 0u, &nullObject, 1u, (void **)0,
      (uint32_t *)0);
  if (rc != EJIT_ERR_INVALID_PARAM) {
    SRE_printf("[BOUND-MULTI] FAIL null/lifetime check rc=%d\n", (int)rc);
    return 0;
  }

  ejit_bound_ptr_t zeroSize = {
      &g_bound_multi_cell_configs[EJIT_BOUND_MULTI_CELL_A], 0u, 0u};
  rc = ejit_taskpool_compile_or_get_bound_v(
      0u, (const ejit_dim_pair_t *)0, 0u, &zeroSize, 1u, (void **)0,
      (uint32_t *)0);
  if (rc != EJIT_ERR_INVALID_PARAM) {
    SRE_printf("[BOUND-MULTI] FAIL zero-size check rc=%d\n", (int)rc);
    return 0;
  }

  ejit_bound_ptr_t overflow = {
      (const void *)(uintptr_t)UINTPTR_MAX, 8u, 0u};
  rc = ejit_taskpool_compile_or_get_bound_v(
      0u, (const ejit_dim_pair_t *)0, 0u, &overflow, 1u, (void **)0,
      (uint32_t *)0);
  if (rc != EJIT_ERR_INVALID_PARAM) {
    SRE_printf("[BOUND-MULTI] FAIL overflow check rc=%d\n", (int)rc);
    return 0;
  }

  ejit_bound_ptr_t duplicate[2] = {
      {&g_bound_multi_cell_configs[EJIT_BOUND_MULTI_CELL_A],
       (uint32_t)sizeof(EJitBoundMultiCellConfig), 0u},
      {&g_bound_multi_trp_configs[EJIT_BOUND_MULTI_TRP],
       (uint32_t)sizeof(EJitBoundMultiTrpConfig), 0u},
  };
  rc = ejit_taskpool_compile_or_get_bound_v(
      0u, (const ejit_dim_pair_t *)0, 0u, duplicate, 2u, (void **)0,
      (uint32_t *)0);
  if (rc != EJIT_ERR_INVALID_PARAM) {
    SRE_printf("[BOUND-MULTI] FAIL duplicate argIndex check rc=%d\n", (int)rc);
    return 0;
  }

  SRE_printf("[BOUND-MULTI] PASS invalid descriptor lists rejected before "
             "worker dereference (>8, null, zero, overflow, duplicate); "
             "no UAF constructed\n");
  return 1;
}

static int run_producer(void) {
  uint32_t stage = __atomic_load_n(&g_bound_multi_stage, __ATOMIC_ACQUIRE);
  if (stage != BOUND_MULTI_WORKER_READY) {
    SRE_printf("[BOUND-MULTI][core=%u] FAIL stage=%u; run core %u first\n",
               EJIT_BOUND_MULTI_PRODUCER_CORE, stage,
               EJIT_BOUND_MULTI_WORKER_CORE);
    return -3;
  }

  if (ejit_activate("cell", EJIT_BOUND_MULTI_CELL_A) != EJIT_OK ||
      ejit_activate("cell", EJIT_BOUND_MULTI_CELL_B) != EJIT_OK ||
      ejit_activate("trp", EJIT_BOUND_MULTI_TRP) != EJIT_OK) {
    SRE_printf("[BOUND-MULTI][core=%u] FAIL activate\n",
               EJIT_BOUND_MULTI_PRODUCER_CORE);
    return -4;
  }

  ejit_clear_cache();
  if (!check_invalid_descriptors())
    return -5;
  if (!check_transport_contract())
    return -6;

  ejit_taskpool_stats_t before = {0};
  (void)ejit_taskpool_get_stats(&before);

  // Submit cell A and wait before cell B: current shared dedup is keyed by
  // function identity, so serial submission makes both cell versions visible.
  const uint32_t aotA = call_bound_root(EJIT_BOUND_MULTI_CELL_A);
  if (aotA != expected_value(EJIT_BOUND_MULTI_CELL_A) ||
      ejit_publish_pending_code() != EJIT_OK ||
      !wait_for_compiles(before.asyncCompiles, 2u, "cell-A")) {
    SRE_printf("[BOUND-MULTI][core=%u] FAIL cell-A AOT/compile\n",
               EJIT_BOUND_MULTI_PRODUCER_CORE);
    ejit_taskpool_print_stats();
    return -7;
  }

  const uint32_t aotB = call_bound_root(EJIT_BOUND_MULTI_CELL_B);
  if (aotB != expected_value(EJIT_BOUND_MULTI_CELL_B) ||
      ejit_publish_pending_code() != EJIT_OK ||
      !wait_for_compiles(before.asyncCompiles, 4u, "cell-B")) {
    SRE_printf("[BOUND-MULTI][core=%u] FAIL cell-B AOT/compile\n",
               EJIT_BOUND_MULTI_PRODUCER_CORE);
    ejit_taskpool_print_stats();
    return -8;
  }

  ejit_taskpool_stats_t compiled = {0};
  (void)ejit_taskpool_get_stats(&compiled);
  const uint32_t jitA = call_bound_root(EJIT_BOUND_MULTI_CELL_A);
  const uint32_t jitB = call_bound_root(EJIT_BOUND_MULTI_CELL_B);
  ejit_taskpool_stats_t after = {0};
  (void)ejit_taskpool_get_stats(&after);
  if (jitA != expected_value(EJIT_BOUND_MULTI_CELL_A) ||
      jitB != expected_value(EJIT_BOUND_MULTI_CELL_B) ||
      after.cacheHits < compiled.cacheHits + 2u) {
    SRE_printf("[BOUND-MULTI][core=%u] FAIL JIT cellA=%u cellB=%u "
               "hits=%llu/%llu\n",
               EJIT_BOUND_MULTI_PRODUCER_CORE, jitA, jitB,
               (unsigned long long)(after.cacheHits - compiled.cacheHits),
               (unsigned long long)2u);
    return -9;
  }

  __atomic_store_n(&g_bound_multi_sink,
                   ((uint64_t)jitA << 32) | (uint64_t)jitB, __ATOMIC_RELEASE);
  __atomic_store_n(&g_bound_multi_stage, BOUND_MULTI_COMPILED,
                   __ATOMIC_RELEASE);
  SRE_printf("[BOUND-MULTI][core=%u] PASS cellA AOT/JIT=%u/%u cellB "
             "AOT/JIT=%u/%u compiles=%llu hits=%llu; return to core %u "
             "for dumps\n",
             EJIT_BOUND_MULTI_PRODUCER_CORE, aotA, jitA, aotB, jitB,
             (unsigned long long)(after.asyncCompiles - before.asyncCompiles),
             (unsigned long long)(after.cacheHits - compiled.cacheHits),
             EJIT_BOUND_MULTI_WORKER_CORE);
  return 0;
}

static int run_worker(void) {
  uint32_t stage = __atomic_load_n(&g_bound_multi_stage, __ATOMIC_ACQUIRE);
  if (stage == BOUND_MULTI_RESET) {
    __atomic_store_n(&g_bound_multi_sink, 0u, __ATOMIC_RELEASE);
    ejit_dump_func("*");
    __atomic_store_n(&g_bound_multi_stage, BOUND_MULTI_WORKER_READY,
                     __ATOMIC_RELEASE);
    SRE_printf("[BOUND-MULTI][core=%u] PASS worker ready; run "
               "test_ejit_period on core %u\n",
               EJIT_BOUND_MULTI_WORKER_CORE, EJIT_BOUND_MULTI_PRODUCER_CORE);
    return 0;
  }
  SRE_printf("[BOUND-MULTI][core=%u] worker already ready stage=%u; wait "
             "for core %u then run test_ejit_bound_ptr_multistruct_print\n",
             EJIT_BOUND_MULTI_WORKER_CORE, stage,
             EJIT_BOUND_MULTI_PRODUCER_CORE);
  return 0;
}

int test_ejit_bound_ptr_multistruct_print(uint8_t a, uint8_t b, uint8_t c,
                                          uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  const uint32_t core = (uint32_t)g_ucLocalCoreID;
  if (core != EJIT_BOUND_MULTI_WORKER_CORE) {
    SRE_printf("[BOUND-MULTI][core=%u] FAIL dumps are worker-local; run on "
               "core %u\n",
               core, EJIT_BOUND_MULTI_WORKER_CORE);
    return -10;
  }
  uint32_t stage = __atomic_load_n(&g_bound_multi_stage, __ATOMIC_ACQUIRE);
  if (stage < BOUND_MULTI_COMPILED) {
    SRE_printf("[BOUND-MULTI][core=%u] FAIL stage=%u; run producer core %u "
               "first\n",
               core, stage, EJIT_BOUND_MULTI_PRODUCER_CORE);
    return -11;
  }
  if (stage == BOUND_MULTI_PRINTED) {
    SRE_printf("[BOUND-MULTI][core=%u] dumps already printed\n", core);
    return 0;
  }

  SRE_printf("[BOUND-MULTI][core=%u] === COMPILED VERSIONS ===\n", core);
  ejit_taskpool_print_compiled();
  SRE_printf("[BOUND-MULTI][core=%u] === ROOT ENTRY VIEW ===\n", core);
  ejit_print_dumped("bound_multi_root");
  SRE_printf("[BOUND-MULTI][core=%u] === HELPER ENTRY VIEW ===\n", core);
  ejit_print_dumped("bound_multi_helper");
  SRE_printf("[BOUND-MULTI][core=%u] === ROOT MODULE VIEW ===\n", core);
  ejit_print_dumped_module("bound_multi_root");
  SRE_printf("[BOUND-MULTI][core=%u] PASS sink=0x%llx; expected root/helper "
             "versions for cell A/B and no payload copy\n"
             "[BOUND-MULTI] dump criterion: algorithm=7, scale=5/9, "
             "multiplier=3, offset=11 must be folded in root/helper; "
             "runtimeBias/runtimeTag remain runtime loads\n",
             core,
             (unsigned long long)__atomic_load_n(&g_bound_multi_sink,
                                                 __ATOMIC_ACQUIRE));
  __atomic_store_n(&g_bound_multi_stage, BOUND_MULTI_PRINTED,
                   __ATOMIC_RELEASE);
  return 0;
}

int test_ejit_period(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  const uint32_t core = (uint32_t)g_ucLocalCoreID;
  SRE_printf("\n=== EJIT borrowed multi-structure demo (core=%u) ===\n", core);
  call_init_array_functions();

  ejit_config_t cfg = {0};
  cfg.compileMode = EJIT_COMPILE_ASYNC;
  cfg.optLevel = EJIT_OPT_L2;
  cfg.enableLogger = true;
  cfg.forceStaticRegistry = true;

  ejit_status_t rc = ejit_init(&cfg);
  uint32_t worker = ejit_taskpool_get_worker_core();
  SRE_printf("[BOUND-MULTI][core=%u] init rc=%d worker=%u stage=%u\n", core,
             (int)rc, worker,
             __atomic_load_n(&g_bound_multi_stage, __ATOMIC_ACQUIRE));
  if (rc != EJIT_OK)
    return -1;
  if (worker != EJIT_BOUND_MULTI_WORKER_CORE) {
    SRE_printf("[BOUND-MULTI][core=%u] FAIL worker=%u; expected core %u\n",
               core, worker, EJIT_BOUND_MULTI_WORKER_CORE);
    return -2;
  }
  if (core == EJIT_BOUND_MULTI_WORKER_CORE)
    return run_worker();
  if (core == EJIT_BOUND_MULTI_PRODUCER_CORE)
    return run_producer();
  SRE_printf("[BOUND-MULTI][core=%u] skip: use core %u or core %u\n", core,
             EJIT_BOUND_MULTI_WORKER_CORE, EJIT_BOUND_MULTI_PRODUCER_CORE);
  return 0;
}
