//===-- ejit_const_after_init_sre_multicore_test.c -----------------------===//
//
// Board flow after rebuilding Clang, EJIT, and the business package:
//   1. Product startup runs its init-array path exactly once.
//   2. Core 6:  run test_ejit_period once to start the fixed worker.
//   3. Core 16: run test_ejit_period once to finalize the globals before the
//      first JIT-triggering call, then drive PGO Tier-1/Tier-2.
//   4. Core 6:  run test_ejit_const_dump any number of times for read-only
//      function/module dumps.
//
// This repeatable command deliberately never calls call_init_array_functions().
// A standalone environment that does not provide product startup must expose
// init-array as a separate one-shot setup command. Do not call ejit_shutdown():
// the owner worker must survive the shell return.
//
//===----------------------------------------------------------------------===//

#if !defined(EJIT_CONST_AFTER_INIT_HOST_TEST) && !defined(__has_attribute)
#error "EJIT const-after-init demo requires Clang attribute detection"
#endif
#if !defined(EJIT_CONST_AFTER_INIT_HOST_TEST) &&                              \
    (!__has_attribute(ejit_const_after_init) || !__has_attribute(ejit_entry))
#error "rebuild and use the EJIT Clang before compiling this demo"
#endif

typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef long long int64_t;

#ifndef NULL
#define NULL ((void *)0)
#endif
#ifdef EJIT_CONST_AFTER_INIT_HOST_TEST
#define EJIT_CONST_AFTER_INIT
#define EJIT_ENTRY_ATTR
#else
#define EJIT_CONST_AFTER_INIT __attribute__((ejit_const_after_init))
#define EJIT_ENTRY_ATTR __attribute__((ejit_entry))
#endif

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

extern int ejit_init_pgo(const void *config);
extern int ejit_taskpool_get_stats(ejit_taskpool_stats_t *out);
extern uint32_t ejit_taskpool_pending_count(void);
extern uint32_t ejit_taskpool_get_worker_core(void);
extern void ejit_taskpool_print_stats(void);
extern void ejit_taskpool_print_compiled(void);
extern void ejit_dump_func(const char *name);
extern void ejit_print_dumped(const char *name);
extern void ejit_print_dumped_module(const char *name);

extern void SRE_printf(const char *format, ...);
extern uint32_t SRE_TaskDelay(uint32_t tick);
extern uint8_t g_ucLocalCoreID;

#ifndef EJIT_SHARED_SECTION_ATTR
#ifdef EJIT_CONST_AFTER_INIT_HOST_TEST
#define EJIT_SHARED_SECTION_ATTR
#else
#define EJIT_SHARED_SECTION_ATTR __attribute__((section(".mc_shared")))
#endif
#endif

#define CONST_WORKER_CORE 6u
#define CONST_PRODUCER_CORE 16u
#define CONST_WAIT_ROUNDS 6000u
#define CONST_EXPECTED_COMPILES 2u

#define CONST_RUN_IDLE 0u
#define CONST_RUN_ACTIVE 1u
#define CONST_RUN_COMPLETE 2u
#define CONST_RUN_FAILED 3u

typedef enum ConstMode {
  CONST_MODE_ADD = 0,
  CONST_MODE_MIX = 1,
} ConstMode;

EJIT_SHARED_SECTION_ATTR EJIT_CONST_AFTER_INIT uint32_t g_const_scale;
EJIT_SHARED_SECTION_ATTR EJIT_CONST_AFTER_INIT int64_t g_const_bias;
EJIT_SHARED_SECTION_ATTR EJIT_CONST_AFTER_INIT ConstMode g_const_mode;
EJIT_SHARED_SECTION_ATTR volatile uint32_t g_const_initialized;
EJIT_SHARED_SECTION_ATTR volatile uint32_t g_const_run_state;
EJIT_SHARED_SECTION_ATTR volatile uint32_t g_const_worker_armed;
EJIT_SHARED_SECTION_ATTR volatile uint64_t g_const_sink;

EJIT_ENTRY_ATTR uint64_t const_after_init_kernel(uint32_t x) {
  uint64_t value = (uint64_t)x * (uint64_t)g_const_scale;
  if (g_const_mode == CONST_MODE_MIX)
    value ^= (uint64_t)g_const_bias;
  else
    value += (uint64_t)g_const_bias;
  return value;
}

static uint64_t expected_value(uint32_t x) {
  uint64_t value = (uint64_t)x * 7u;
  return value ^ (uint64_t)-19;
}

static void finalize_globals_once(void) {
  if (__atomic_load_n(&g_const_initialized, __ATOMIC_ACQUIRE) != 0u)
    return;
  g_const_scale = 7u;
  g_const_bias = -19;
  g_const_mode = CONST_MODE_MIX;
  __atomic_thread_fence(__ATOMIC_RELEASE);
  __atomic_store_n(&g_const_initialized, 1u, __ATOMIC_RELEASE);
}

static int ensure_ejit_ready(uint32_t core) {
  uint32_t worker = ejit_taskpool_get_worker_core();
  if (worker == CONST_WORKER_CORE) {
    SRE_printf("[CONST-AI][core=%u] EJIT already ready worker=%u\n", core,
               worker);
    return 0;
  }

  int rc = ejit_init_pgo(NULL);
  worker = ejit_taskpool_get_worker_core();
  SRE_printf("[CONST-AI][core=%u] init rc=%d worker=%u\n", core, (int)rc,
             worker);
  if (rc != 0)
    return -2;
  if (worker != CONST_WORKER_CORE) {
    SRE_printf("[CONST-AI] FAIL: reset and run core %u first\n",
               CONST_WORKER_CORE);
    return -3;
  }
  return 0;
}

static int verify_published_version(void) {
  for (uint32_t i = 0; i < 128u; ++i) {
    uint32_t x = 0x10000u + i;
    uint64_t got = const_after_init_kernel(x);
    if (got != expected_value(x)) {
      SRE_printf("[CONST-AI] FAIL post-publish i=%u\n", i);
      return -8;
    }
  }
  return 0;
}

// Read-only diagnostic entry: no init-array, EJIT init, registration, global
// finalization, shutdown, or reset side effects are allowed here.
int test_ejit_const_dump(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;

  const uint32_t core = (uint32_t)g_ucLocalCoreID;
  if (core != CONST_WORKER_CORE) {
    SRE_printf("[CONST-AI] dump skip core=%u; use worker core %u\n", core,
               CONST_WORKER_CORE);
    return -1;
  }
  if (__atomic_load_n(&g_const_run_state, __ATOMIC_ACQUIRE) !=
      CONST_RUN_COMPLETE) {
    SRE_printf("[CONST-AI] dump unavailable: producer run not complete\n");
    return -9;
  }
  SRE_printf("[CONST-AI] captured function view:\n");
  ejit_print_dumped("const_after_init_kernel");
  SRE_printf("[CONST-AI] captured module view:\n");
  ejit_print_dumped_module("const_after_init_kernel");
  ejit_taskpool_print_compiled();
  return 0;
}

int test_ejit_period(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;

  const uint32_t core = (uint32_t)g_ucLocalCoreID;
  if (core != CONST_WORKER_CORE && core != CONST_PRODUCER_CORE) {
    SRE_printf("[CONST-AI] skip core=%u; use core %u or %u\n", core,
               CONST_WORKER_CORE, CONST_PRODUCER_CORE);
    return -1;
  }
  SRE_printf("\n=== EJIT const-after-init test (core=%u) ===\n", core);

  uint32_t state = __atomic_load_n(&g_const_run_state, __ATOMIC_ACQUIRE);
  if (state == CONST_RUN_FAILED) {
    SRE_printf("[CONST-AI] previous run failed; recover or reset before retry\n");
    return -10;
  }

  if (core == CONST_WORKER_CORE) {
    if (state == CONST_RUN_COMPLETE)
      return test_ejit_const_dump(a, b, c, d);
    int rc = ensure_ejit_ready(core);
    if (rc != 0)
      return rc;
    uint32_t expected = 0u;
    if (__atomic_compare_exchange_n(&g_const_worker_armed, &expected, 1u, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      ejit_dump_func("const_after_init_kernel");
    else
      SRE_printf("[CONST-AI] worker already armed; keeping live worker\n");
    SRE_printf("[CONST-AI] worker ready; run test_ejit_period on core %u\n",
               CONST_PRODUCER_CORE);
    return 0;
  }

  if (state == CONST_RUN_COMPLETE) {
    int rc = verify_published_version();
    if (rc != 0)
      return rc;
    SRE_printf("[CONST-AI] already complete; existing published version OK\n");
    ejit_taskpool_print_stats();
    return 0;
  }
  if (state == CONST_RUN_ACTIVE) {
    SRE_printf("[CONST-AI] producer run already active; duplicate rejected\n");
    return -4;
  }
  uint32_t expectedState = CONST_RUN_IDLE;
  if (!__atomic_compare_exchange_n(&g_const_run_state, &expectedState,
                                   CONST_RUN_ACTIVE, 0, __ATOMIC_ACQ_REL,
                                   __ATOMIC_ACQUIRE)) {
    SRE_printf("[CONST-AI] producer state changed to %u; retry later\n",
               expectedState);
    return -4;
  }

  int rc = ensure_ejit_ready(core);
  if (rc != 0) {
    __atomic_store_n(&g_const_run_state, CONST_RUN_FAILED, __ATOMIC_RELEASE);
    return rc;
  }

  // This is the contract boundary: all writes and publication complete before
  // the first call that can enqueue JIT compilation.
  finalize_globals_once();

  ejit_taskpool_stats_t before = {0};
  ejit_taskpool_get_stats(&before);

  int complete = 0;
  for (uint32_t round = 0; round < CONST_WAIT_ROUNDS; ++round) {
    uint64_t got = const_after_init_kernel(round);
    uint64_t expected = expected_value(round);
    __atomic_fetch_xor(&g_const_sink, got, __ATOMIC_RELAXED);
    if (got != expected) {
      SRE_printf("[CONST-AI] FAIL round=%u got=0x%llx expected=0x%llx\n",
                 round, (unsigned long long)got,
                 (unsigned long long)expected);
      __atomic_store_n(&g_const_run_state, CONST_RUN_FAILED,
                       __ATOMIC_RELEASE);
      return -5;
    }

    ejit_taskpool_stats_t now = {0};
    ejit_taskpool_get_stats(&now);
    if (now.compileFailed != before.compileFailed ||
        now.publishFailed != before.publishFailed) {
      SRE_printf("[CONST-AI] FAIL compile/publish failure\n");
      __atomic_store_n(&g_const_run_state, CONST_RUN_FAILED,
                       __ATOMIC_RELEASE);
      return -6;
    }
    if (now.asyncCompiles >=
            before.asyncCompiles + CONST_EXPECTED_COMPILES &&
        now.readyEntries > before.readyEntries &&
        ejit_taskpool_pending_count() == 0u) {
      complete = 1;
      break;
    }
    (void)SRE_TaskDelay(1u);
  }

  ejit_taskpool_print_stats();
  if (!complete) {
    SRE_printf("[CONST-AI] FAIL: PGO completion timeout\n");
    __atomic_store_n(&g_const_run_state, CONST_RUN_FAILED, __ATOMIC_RELEASE);
    return -7;
  }

  // Continue only after both PGO compilations have completed, the Ready count
  // has grown, and no request remains pending. These calls exercise the
  // published Tier-2 version.
  rc = verify_published_version();
  if (rc != 0) {
    __atomic_store_n(&g_const_run_state, CONST_RUN_FAILED, __ATOMIC_RELEASE);
    return rc;
  }
  ejit_taskpool_print_compiled();
  __atomic_store_n(&g_const_run_state, CONST_RUN_COMPLETE, __ATOMIC_RELEASE);
  SRE_printf("[CONST-AI] PASS scale=%u bias=%lld mode=%d sink=0x%llx\n",
             g_const_scale, (long long)g_const_bias, (int)g_const_mode,
             (unsigned long long)__atomic_load_n(&g_const_sink,
                                                  __ATOMIC_RELAXED));
  SRE_printf("[CONST-AI] return to core %u and run test_ejit_const_dump "
             "for repeatable function/module dumps\n",
             CONST_WORKER_CORE);
  return 0;
}
