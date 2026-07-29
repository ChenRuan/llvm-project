//===-- ejit_jit_local_helpers_sre_test.c ---------------------------------===//
//
// SRE functional demo for -fejit-cross-jit-helpers:
//
//   jit_local_entry -> jit_local_helper_b -> jit_local_helper_c
//
// Start test_ejit_period on the intended worker core first, then start it on a
// business core. The first call enqueues an async compile and runs AOT
// fallback; the second call verifies the JIT result. The worker then prints
// optimized IR/ASM. Both ordinary helpers must remain as specialized
// definitions and the calls should be direct code-pool-local branches.
//
// Compile BOTH this file and ejit_jit_local_helpers_sre_helper.c with:
//   -O2 -fejit-cross-jit-helpers
// Link/merge with:
//   clang -fuse-ld=lld -fejit-cross-jit-helpers -r main.o helper.o -o test.o
//
//===----------------------------------------------------------------------===//

#include <stdbool.h>
#include <stdint.h>

#include "ejit_test_helpers.h"

extern void SRE_printf(const char *format, ...);
extern uint32_t SRE_TaskDelay(uint32_t tick);
extern void call_init_array_functions(void);
extern uint8_t g_ucLocalCoreID;

#ifndef EJIT_SHARED_SECTION_ATTR
#define EJIT_SHARED_SECTION_ATTR __attribute__((section(".mc_shared")))
#endif

#define TEST_CELL 1u
#define EXPECTED_RESULT 247u
#define WAIT_ROUNDS 400u
#define WAIT_TICKS 10u
#define IDLE_TICKS 40000u

enum { PHASE_IDLE, PHASE_ARMED, PHASE_VERIFIED, PHASE_PRINTED };

struct LocalHelperCfg {
  ejit_may_const uint32_t mode;
  ejit_may_const uint32_t bias;
  uint32_t runtimeCounter;
};

EJIT_SHARED_SECTION_ATTR ejit_period_arr(cell)
struct LocalHelperCfg g_localHelperCfg[2];
EJIT_SHARED_SECTION_ATTR static uint32_t g_testPhase;
EJIT_SHARED_SECTION_ATTR static uint32_t g_businessClaim;

extern uint32_t jit_local_helper_b(uint8_t cell);

ejit_entry __attribute__((noinline)) uint32_t
jit_local_entry(ejit_period_arr_ind(cell) uint8_t cell) {
  return jit_local_helper_b(cell) * 2u + 1u;
}

static uint32_t core_id(void) { return (uint32_t)g_ucLocalCoreID; }

static uint32_t load_phase(void) {
  return __atomic_load_n(&g_testPhase, __ATOMIC_ACQUIRE);
}

static void store_phase(uint32_t phase) {
  __atomic_store_n(&g_testPhase, phase, __ATOMIC_RELEASE);
}

static void idle_forever(uint32_t core, const char *role) {
  for (;;) {
    SRE_TaskDelay(IDLE_TICKS);
    SRE_printf("[JITHELPER][core=%u] idle role=%s phase=%u\n", core, role,
               load_phase());
  }
}

static bool wait_for_compile(void) {
  for (uint32_t i = 0; i < WAIT_ROUNDS; ++i) {
    ejit_taskpool_stats_t stats;
    ejit_taskpool_get_stats(&stats);
    if (ejit_taskpool_pending_count() == 0 && stats.readyEntries != 0)
      return true;
    SRE_TaskDelay(WAIT_TICKS);
  }
  return false;
}

static void run_worker(uint32_t core) {
  while (load_phase() < PHASE_VERIFIED)
    SRE_TaskDelay(WAIT_TICKS);

  SRE_printf("[JITHELPER][core=%u] printing optimized IR/ASM\n", core);
  ejit_print_dumped("jit_local_entry");
  store_phase(PHASE_PRINTED);
  idle_forever(core, "worker-complete");
}

static void run_business(uint32_t core) {
  uint32_t unclaimed = 0;
  if (!__atomic_compare_exchange_n(&g_businessClaim, &unclaimed, core + 1u,
                                   false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    idle_forever(core, "non-owner-business");

  const struct LocalHelperCfg initial = {
      .mode = 11u, .bias = 5u, .runtimeCounter = 0u};
  g_localHelperCfg[TEST_CELL] = initial;

  ejit_status_t activateRc = ejit_activate("cell", TEST_CELL);
  SRE_printf("[JITHELPER][core=%u] activate rc=%d\n", core, (int)activateRc);

  ejit_dump_func("jit_local_entry");
  store_phase(PHASE_ARMED);

  uint32_t first = jit_local_entry(TEST_CELL);
  SRE_printf("[JITHELPER][core=%u] first(AOT)=%u expected=%u\n", core, first,
             EXPECTED_RESULT);
  if (first != EXPECTED_RESULT)
    idle_forever(core, "aot-mismatch");

  if (!wait_for_compile()) {
    SRE_printf("[JITHELPER][core=%u] FAIL: compile timeout\n", core);
    ejit_taskpool_print_stats();
    idle_forever(core, "compile-timeout");
  }

  uint32_t second = jit_local_entry(TEST_CELL);
  SRE_printf("[JITHELPER][core=%u] second(JIT)=%u expected=%u\n", core, second,
             EXPECTED_RESULT);
  if (second != EXPECTED_RESULT)
    idle_forever(core, "jit-mismatch");

  SRE_printf("[JITHELPER][core=%u] PASS: nested JIT-local helpers\n", core);
  ejit_taskpool_print_compiled();
  ejit_taskpool_print_stats();
  store_phase(PHASE_VERIFIED);

  for (uint32_t i = 0; i < WAIT_ROUNDS; ++i) {
    if (load_phase() >= PHASE_PRINTED)
      idle_forever(core, "test-complete");
    SRE_TaskDelay(WAIT_TICKS);
  }
  idle_forever(core, "dump-timeout");
}

int test_ejit_period(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;

  uint32_t core = core_id();
  call_init_array_functions();

  ejit_config_t cfg;
  ejit_default_config(&cfg);
  cfg.compileMode = EJIT_COMPILE_ASYNC;
  cfg.optLevel = EJIT_OPT_L2;
  cfg.enableLogger = false;
  cfg.forceStaticRegistry = true;

  ejit_status_t initRc = ejit_init(&cfg);
  uint32_t worker = ejit_taskpool_get_worker_core();
  SRE_printf("[JITHELPER][core=%u] init rc=%d worker=%u\n", core, (int)initRc,
             worker);
  if (initRc != EJIT_OK)
    idle_forever(core, "init-failed");

  if (core == worker) {
    __atomic_store_n(&g_businessClaim, 0u, __ATOMIC_RELEASE);
    store_phase(PHASE_IDLE);
    run_worker(core);
  }

  run_business(core);
  return 0;
}
