//===-- ejit_local_free_dim_sre_test.c - worker6 / owner16 demo ------------===//
//
// Reset, run test_ejit_period on core 6, then on core 16; finally run
// test_ejit_local_free_dim_print on core 6. Keep the shared worker alive.
// This source is a board procedure, not evidence that a board run passed.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"

extern void SRE_printf(const char *, ...);
extern uint32_t SRE_TaskDelay(uint32_t);
extern void call_init_array_functions(void);
extern uint8_t g_ucLocalCoreID;

struct LocalSlot {
  ejit_may_const uint32_t mode;
  uint32_t payload;
};
struct LocalCell { struct LocalSlot slots[10]; };

__attribute__((section(".mc_shared"), aligned(64)))
ejit_period_arr(cell) struct LocalCell g_local_free_cells[2];
static volatile struct { uint32_t slot; } g_local_free_runtime;
static volatile uint32_t g_local_free_initializer_calls;

static __attribute__((always_inline)) unsigned next_local_slot(void) {
  ++g_local_free_initializer_calls;
  return g_local_free_runtime.slot;
}

ejit_entry uint32_t
local_free_work(ejit_period_arr_ind(cell) uint32_t cell) {
  const size_t offset ejit_free_dim =
      (size_t)next_local_slot() * sizeof(struct LocalSlot);
  const struct LocalSlot *p = (const struct LocalSlot *)(
      (const char *)&g_local_free_cells[cell].slots[0] + offset);
  return p->mode * 100u + p->payload;
}

// The volatile call boundary prevents the controller from bypassing the entry
// wrapper by folding a direct call before runtime compilation is requested.
static uint32_t (*volatile g_local_free_call)(uint32_t) = local_free_work;

static int check_call(unsigned cell, unsigned slot, unsigned mode) {
  unsigned before = g_local_free_initializer_calls;
  g_local_free_runtime.slot = slot;
  uint32_t actual = g_local_free_call(cell);
  uint32_t expected = mode * 100u + cell * 1000u + slot;
  if (actual != expected || g_local_free_initializer_calls != before + 1u) {
    SRE_printf("[LFD] FAIL cell=%u slot=%u got=%u expected=%u init=%u/%u\n",
               cell, slot, actual, expected, g_local_free_initializer_calls,
               before + 1u);
    return 0;
  }
  return 1;
}

static int wait_generation(unsigned cell, unsigned mode, uint64_t compiles) {
  for (unsigned round = 0; round != 6000; ++round) {
    for (unsigned slot = 0; slot != 10; ++slot)
      if (!check_call(cell, slot, mode))
        return 0;
    ejit_taskpool_stats_t stats;
    if (ejit_taskpool_get_stats(&stats) != EJIT_OK ||
        stats.compileFailed || stats.publishFailed)
      return 0;
    if (stats.asyncCompiles >= compiles && !ejit_taskpool_pending_count())
      return 1;
    SRE_TaskDelay(2);
  }
  SRE_printf("[LFD] FAIL bounded Tier-2 wait timed out\n");
  return 0;
}

int test_ejit_period(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a; (void)b; (void)c; (void)d;
  unsigned core = g_ucLocalCoreID;
  call_init_array_functions();
  if (core == 6u)
    for (unsigned cell = 0; cell != 2; ++cell)
      for (unsigned slot = 0; slot != 10; ++slot) {
        g_local_free_cells[cell].slots[slot].mode = cell ? 11u : 7u;
        g_local_free_cells[cell].slots[slot].payload = cell * 1000u + slot;
      }
  ejit_config_t cfg = {0};
  cfg.compileMode = EJIT_COMPILE_ASYNC;
  cfg.optLevel = EJIT_OPT_L2;
  cfg.enableLogger = true;
  cfg.forceStaticRegistry = true;
  if (ejit_init_pgo(&cfg) != EJIT_OK)
    return -1;
  if (ejit_taskpool_get_worker_core() != 6u)
    return -2;
  if (core == 6u) {
    ejit_dump_func("local_free_work");
    SRE_printf("[LFD] worker6 ready; run core16 from a fresh cold start\n");
    return 0;
  }
  if (core != 16u)
    return -3;
  g_local_free_initializer_calls = 0;
  for (unsigned cell = 0; cell != 2; ++cell) {
    if (ejit_activate("cell", cell) != EJIT_OK)
      return -4;
    // Check the very first wrapper invocation before waiting/prewarming.
    if (!check_call(cell, 9u, cell ? 11u : 7u))
      return -5;
    SRE_printf("[LFD] cold cell=%u slot=9 correct\n", cell);
    if (!wait_generation(cell, cell ? 11u : 7u, 2u * (cell + 1u)))
      return -6;
  }
  if (ejit_deactivate("cell", 1u) != EJIT_OK)
    return -7;
  for (unsigned slot = 0; slot != 10; ++slot)
    g_local_free_cells[1].slots[slot].mode = 19u;
  if (ejit_activate("cell", 1u) != EJIT_OK ||
      !check_call(1u, 8u, 19u) || !wait_generation(1u, 19u, 6u))
    return -8;
  if (!check_call(0u, 7u, 7u))
    return -9;
  ejit_taskpool_print_stats();
  SRE_printf("[LFD] workload checks passed; inspect worker6 IR/ASM separately\n");
  return 0;
}

int test_ejit_local_free_dim_print(void) {
  if (g_ucLocalCoreID != 6u)
    return -1;
  ejit_print_dumped("local_free_work");
  ejit_taskpool_print_compiled();
  return 0;
}
