//===-- ejit_pgo_sve_multicore_test.c - Board PGO/SVE smoke test ----------===//
//
// Board flow:
//   1. Run test_ejit_period_pgo_sve on core 6. It initializes the fixed
//      shared worker and waits for the producer.
//   2. Run test_ejit_period_pgo_sve on core 16. It activates cell 0 and drives
//      one independent function through Tier-1 sampling and Tier-2 PGOUse.
//
// The test deliberately pauses between the two tiers. Core 6 prints the
// published Tier-1 entry while it is still marked tier1-collecting, then arms
// the exact-name dump filter before allowing Tier-2. The final dump is thus a
// direct Tier-2 IR/assembly witness. Do not call ejit_shutdown(): the owner
// worker and attached facade must remain alive across the two core sessions.
//
//===----------------------------------------------------------------------===//

#include <stdbool.h>
#include <stdint.h>

#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"

extern void SRE_printf(const char *format, ...);
extern uint32_t SRE_TaskDelay(uint32_t tick);
extern void call_init_array_functions(void);
extern uint8_t g_ucLocalCoreID;

#ifndef EJIT_SHARED_SECTION_ATTR
#define EJIT_SHARED_SECTION_ATTR __attribute__((section(".mc_shared")))
#endif

#define PGO_SVE_WORKER_CORE 6u
#define PGO_SVE_PRODUCER_CORE 16u
#define PGO_SVE_VECTOR_LENGTH 256u
#define PGO_SVE_MAX_CALLS 6000u
#define PGO_SVE_WAIT_ROUNDS 6000u
#define PGO_SVE_WAIT_TICKS 10u

static const char kPgoSveFunctionName[] = "pgo_sve_probe";

struct PgoSveCfg {
  ejit_may_const uint32_t bias;
};

EJIT_SHARED_SECTION_ATTR ejit_period_arr(cell)
struct PgoSveCfg g_pgo_sve_cfg[1];
EJIT_SHARED_SECTION_ATTR volatile uint32_t g_pgo_sve_owner_ready;
EJIT_SHARED_SECTION_ATTR volatile uint32_t g_pgo_sve_tier1_ready;
EJIT_SHARED_SECTION_ATTR volatile uint32_t g_pgo_sve_allow_tier2;
EJIT_SHARED_SECTION_ATTR volatile uint32_t g_pgo_sve_producer_done;
EJIT_SHARED_SECTION_ATTR volatile int32_t g_pgo_sve_producer_rc;
EJIT_SHARED_SECTION_ATTR volatile uint64_t g_pgo_sve_start_async;

// The loop has a runtime trip count and externally visible stores, so L3 SVE
// vectorization has a stable marker (ptrue/whilelo + ld1/st1 or scalable-vector
// IR) without making the test depend on application-private headers.
ejit_entry uint32_t pgo_sve_probe(ejit_period_arr_ind(cell) uint8_t cell,
                                  const uint32_t *__restrict src,
                                  uint32_t *__restrict dst, uint32_t count) {
  const uint32_t bias = g_pgo_sve_cfg[cell].bias;
  for (uint32_t i = 0; i < count; ++i)
    dst[i] = src[i] + bias;
  return dst[count - 1] ^ count;
}

static uint32_t load_u32(volatile uint32_t *value) {
  return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static uint64_t load_u64(volatile uint64_t *value) {
  return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static bool wait_for_flag(volatile uint32_t *value, uint32_t expected,
                          const char *what, uint32_t core) {
  for (uint32_t round = 0; round < PGO_SVE_WAIT_ROUNDS; ++round) {
    if (load_u32(value) == expected)
      return true;
    if ((round % 500u) == 0)
      SRE_printf("[PGO-SVE][core=%u] waiting %s\n", core, what);
    (void)SRE_TaskDelay(PGO_SVE_WAIT_TICKS);
  }
  SRE_printf("[PGO-SVE][core=%u] FAIL: timeout waiting %s\n", core, what);
  return false;
}

static bool wait_for_tier1_or_done(uint32_t core) {
  for (uint32_t round = 0; round < PGO_SVE_WAIT_ROUNDS; ++round) {
    if (load_u32(&g_pgo_sve_tier1_ready) != 0)
      return true;
    if (load_u32(&g_pgo_sve_producer_done) != 0)
      return false;
    if ((round % 500u) == 0)
      SRE_printf("[PGO-SVE][core=%u] waiting Tier-1 publish\n", core);
    (void)SRE_TaskDelay(PGO_SVE_WAIT_TICKS);
  }
  SRE_printf("[PGO-SVE][core=%u] FAIL: timeout waiting Tier-1 publish\n", core);
  return false;
}

static bool wait_for_producer_or_timeout(uint32_t core) {
  for (uint32_t round = 0; round < PGO_SVE_WAIT_ROUNDS; ++round) {
    if (load_u32(&g_pgo_sve_producer_done) != 0)
      return true;
    if ((round % 500u) == 0)
      SRE_printf("[PGO-SVE][core=%u] waiting Tier-2 completion\n", core);
    (void)SRE_TaskDelay(PGO_SVE_WAIT_TICKS);
  }
  SRE_printf("[PGO-SVE][core=%u] FAIL: timeout waiting Tier-2 completion\n",
             core);
  return false;
}

static ejit_config_t make_pgo_config(void) {
  ejit_config_t cfg = {0};
  cfg.compileMode = EJIT_COMPILE_ASYNC;
  cfg.optLevel = EJIT_OPT_L3;
  cfg.enableLogger = true;
  cfg.forceStaticRegistry = true;
  return cfg;
}

static int producer_fail(uint32_t core, int rc, const char *reason) {
  SRE_printf("[PGO-SVE][core=%u] FAIL: %s\n", core, reason);
  __atomic_store_n(&g_pgo_sve_producer_rc, rc, __ATOMIC_RELEASE);
  __atomic_store_n(&g_pgo_sve_producer_done, 1u, __ATOMIC_RELEASE);
  return rc;
}

static bool verify_probe_result(const uint32_t *src, const uint32_t *dst,
                                uint32_t bias, uint32_t result) {
  const uint32_t expectedFirst = src[0] + bias;
  const uint32_t expectedLast = src[PGO_SVE_VECTOR_LENGTH - 1u] + bias;
  return dst[0] == expectedFirst &&
         dst[PGO_SVE_VECTOR_LENGTH - 1u] == expectedLast &&
         result == (expectedLast ^ PGO_SVE_VECTOR_LENGTH);
}

static int run_producer(uint32_t core) {
  if (!wait_for_flag(&g_pgo_sve_owner_ready, 1u, "owner ready", core))
    return producer_fail(core, -10, "owner did not become ready");

  ejit_status_t activate = ejit_activate("cell", 0u);
  if (activate != EJIT_OK)
    return producer_fail(core, -11, "ejit_activate(cell,0) failed");

  uint32_t src[PGO_SVE_VECTOR_LENGTH];
  uint32_t dst[PGO_SVE_VECTOR_LENGTH];
  for (uint32_t i = 0; i < PGO_SVE_VECTOR_LENGTH; ++i) {
    src[i] = i + 1u;
    dst[i] = 0u;
  }

  ejit_taskpool_stats_t before = {0};
  if (ejit_taskpool_get_stats(&before) != EJIT_OK)
    return producer_fail(core, -12, "cannot read initial taskpool stats");
  const uint64_t startCompileFailed = before.compileFailed;
  const uint64_t startPublishFailed = before.publishFailed;
  const uint64_t startAsync = load_u64(&g_pgo_sve_start_async);
  const uint64_t targetAsync = startAsync + 2u;
  SRE_printf(
      "[PGO-SVE][core=%u] attached cell=0 start_async=%llu target=%llu\n", core,
      (unsigned long long)startAsync, (unsigned long long)targetAsync);

  // The first call requests Tier-1 and remains the only call until Tier-1 is
  // published. This prevents the 64th hit from enqueueing Tier-2 before the
  // worker has printed the Tier-1 state and armed the Tier-2 dump filter.
  uint32_t result = pgo_sve_probe(0u, src, dst, PGO_SVE_VECTOR_LENGTH);
  if (!verify_probe_result(src, dst, g_pgo_sve_cfg[0].bias, result))
    return producer_fail(core, -13, "function result changed across tiers");

  bool tier1Published = false;
  for (uint32_t round = 0; round < PGO_SVE_WAIT_ROUNDS; ++round) {
    ejit_taskpool_stats_t stats = {0};
    if (ejit_taskpool_get_stats(&stats) != EJIT_OK)
      return producer_fail(core, -14, "cannot read taskpool stats");
    if (stats.compileFailed > startCompileFailed ||
        stats.publishFailed > startPublishFailed)
      return producer_fail(core, -15, "compile or publish failure");
    if (stats.asyncCompiles >= startAsync + 1u) {
      tier1Published = true;
      break;
    }
    (void)SRE_TaskDelay(1u);
  }

  if (!tier1Published)
    return producer_fail(core, -16, "Tier-1 publish timeout");

  __atomic_store_n(&g_pgo_sve_tier1_ready, 1u, __ATOMIC_RELEASE);
  SRE_printf("[PGO-SVE][core=%u] Tier-1 published; pausing before Tier-2\n",
             core);
  if (!wait_for_flag(&g_pgo_sve_allow_tier2, 1u, "Tier-2 release", core))
    return producer_fail(core, -17, "owner did not release Tier-2");

  for (uint32_t round = 0; round < PGO_SVE_MAX_CALLS; ++round) {
    result = pgo_sve_probe(0u, src, dst, PGO_SVE_VECTOR_LENGTH);
    if (!verify_probe_result(src, dst, g_pgo_sve_cfg[0].bias, result))
      return producer_fail(core, -13, "function result changed across tiers");

    ejit_taskpool_stats_t stats = {0};
    if (ejit_taskpool_get_stats(&stats) != EJIT_OK)
      return producer_fail(core, -14, "cannot read taskpool stats");
    if (stats.compileFailed > startCompileFailed ||
        stats.publishFailed > startPublishFailed)
      return producer_fail(core, -15, "compile or publish failure");
    if (stats.asyncCompiles >= targetAsync && stats.pendingEntries == 0u &&
        ejit_taskpool_pending_count() == 0u) {
      SRE_printf("[PGO-SVE][core=%u] Tier-2 published after %u calls\n", core,
                 round + 1u);
      __atomic_store_n(&g_pgo_sve_producer_rc, 0, __ATOMIC_RELEASE);
      __atomic_store_n(&g_pgo_sve_producer_done, 1u, __ATOMIC_RELEASE);
      return 0;
    }
    (void)SRE_TaskDelay(1u);
  }

  ejit_taskpool_print_stats();
  return producer_fail(core, -18, "Tier-2 completion timeout");
}

static int run_owner(uint32_t core) {
  g_pgo_sve_cfg[0].bias = 7u;
  __atomic_store_n(&g_pgo_sve_owner_ready, 0u, __ATOMIC_RELEASE);
  __atomic_store_n(&g_pgo_sve_tier1_ready, 0u, __ATOMIC_RELEASE);
  __atomic_store_n(&g_pgo_sve_allow_tier2, 0u, __ATOMIC_RELEASE);
  __atomic_store_n(&g_pgo_sve_producer_done, 0u, __ATOMIC_RELEASE);
  __atomic_store_n(&g_pgo_sve_producer_rc, -1, __ATOMIC_RELEASE);

  ejit_config_t cfg = make_pgo_config();
  ejit_status_t rc = ejit_init_pgo(&cfg);
  uint32_t worker = ejit_taskpool_get_worker_core();
  SRE_printf("[PGO-SVE][core=%u] init rc=%d worker=%u opt=L3\n", core, (int)rc,
             worker);
  if (rc != EJIT_OK)
    return -20;
  if (worker != PGO_SVE_WORKER_CORE)
    return -21;

  ejit_taskpool_stats_t stats = {0};
  if (ejit_taskpool_get_stats(&stats) != EJIT_OK)
    return -22;
  __atomic_store_n(&g_pgo_sve_start_async, stats.asyncCompiles,
                   __ATOMIC_RELEASE);

#if defined(EJIT_SRE_SVE_VECTORIZATION)
  SRE_printf("[PGO-SVE] SVE_ON=1 T2_VECTOR_EVIDENCE=scaled-vector-dump\n");
#else
  SRE_printf("[PGO-SVE] SVE_ON=0 T2_VECTOR_EVIDENCE=scalar-dump-expected\n");
#endif
  SRE_printf("[PGO-SVE] T1_POLICY=NO_VECTOR_PASS; "
             "T1 evidence is the collecting entry\n");

  __atomic_store_n(&g_pgo_sve_owner_ready, 1u, __ATOMIC_RELEASE);
  if (!wait_for_tier1_or_done(core)) {
    ejit_taskpool_print_stats();
    return -23;
  }

  SRE_printf(
      "[PGO-SVE][core=%u] T1 evidence: published tier1-collecting entry\n",
      core);
  ejit_taskpool_print_compiled();
  // Instrumented captures are intentionally disabled by the runtime. Arm the
  // exact-name filter now so the next capture is the PGOUse/Tier-2 result.
  ejit_dump_func(kPgoSveFunctionName);
  __atomic_store_n(&g_pgo_sve_allow_tier2, 1u, __ATOMIC_RELEASE);

  if (!wait_for_producer_or_timeout(core)) {
    ejit_taskpool_print_stats();
    return -24;
  }
  int producerRc = __atomic_load_n(&g_pgo_sve_producer_rc, __ATOMIC_ACQUIRE);
  if (producerRc != 0) {
    ejit_taskpool_print_stats();
    return producerRc;
  }

  ejit_taskpool_print_compiled();
  SRE_printf("[PGO-SVE][core=%u] T2 evidence: exact-name dump follows\n", core);
  ejit_print_dumped(kPgoSveFunctionName);
  SRE_printf("[PGO-SVE][core=%u] PASS: core6 worker + core16 PGO Gen/Use\n",
             core);
  return 0;
}

int test_ejit_period_pgo_sve(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;

  const uint32_t core = (uint32_t)g_ucLocalCoreID;
  SRE_printf("\n=== EJIT PGO/SVE two-core test (core=%u) ===\n", core);
  call_init_array_functions();

  if (core == PGO_SVE_WORKER_CORE)
    return run_owner(core);
  if (core == PGO_SVE_PRODUCER_CORE)
    return run_producer(core);

  SRE_printf("[PGO-SVE][core=%u] skip: run on core %u then core %u\n", core,
             PGO_SVE_WORKER_CORE, PGO_SVE_PRODUCER_CORE);
  return -30;
}
