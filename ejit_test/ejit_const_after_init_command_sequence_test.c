// Host-runnable lifecycle regression for the board command. This mocks only
// the platform/runtime boundary; the real PGO integration remains board-only.

#include <stdarg.h>
#include <stdio.h>

#define EJIT_CONST_AFTER_INIT_HOST_TEST 1
#include "ejit_const_after_init_sre_multicore_test.c"

uint8_t g_ucLocalCoreID;

static uint8_t g_runtime_ready[256];
static ejit_taskpool_stats_t g_stats;
static uint32_t g_init_calls;
static uint32_t g_dump_arm_calls;
static uint32_t g_function_dump_calls;
static uint32_t g_module_dump_calls;
static uint32_t g_failures;

#define CHECK(condition, message)                                               \
  do {                                                                          \
    if (!(condition)) {                                                         \
      printf("FAIL: %s\n", message);                                           \
      ++g_failures;                                                             \
    }                                                                           \
  } while (0)

int ejit_init_pgo(const void *config) {
  (void)config;
  ++g_init_calls;
  g_runtime_ready[g_ucLocalCoreID] = 1u;
  return 0;
}

int ejit_taskpool_get_stats(ejit_taskpool_stats_t *out) {
  *out = g_stats;
  return 0;
}

uint32_t ejit_taskpool_pending_count(void) { return 0u; }

uint32_t ejit_taskpool_get_worker_core(void) {
  return g_runtime_ready[g_ucLocalCoreID] ? CONST_WORKER_CORE : 0xffffffffu;
}

void ejit_taskpool_print_stats(void) {}
void ejit_taskpool_print_compiled(void) {}

void ejit_dump_func(const char *name) {
  (void)name;
  ++g_dump_arm_calls;
}

void ejit_print_dumped(const char *name) {
  (void)name;
  ++g_function_dump_calls;
}

void ejit_print_dumped_module(const char *name) {
  (void)name;
  ++g_module_dump_calls;
}

void SRE_printf(const char *format, ...) { (void)format; }

uint32_t SRE_TaskDelay(uint32_t tick) {
  (void)tick;
  g_stats.asyncCompiles = CONST_EXPECTED_COMPILES;
  g_stats.readyEntries = 1u;
  return 0u;
}

static int run_on(uint8_t core) {
  g_ucLocalCoreID = core;
  return test_ejit_period(0, 0, 0, 0);
}

static int dump_on(uint8_t core) {
  g_ucLocalCoreID = core;
  return test_ejit_const_dump(0, 0, 0, 0);
}

int main(void) {
  CHECK(run_on(CONST_WORKER_CORE) == 0, "first worker setup");
  CHECK(g_init_calls == 1u, "worker initialized exactly once");
  CHECK(g_dump_arm_calls == 1u, "dump armed exactly once");
  CHECK(run_on(CONST_WORKER_CORE) == 0, "repeated worker setup");
  CHECK(g_init_calls == 1u, "repeated worker setup does not reinitialize");
  CHECK(g_dump_arm_calls == 1u, "repeated worker setup does not re-arm");

  CHECK(run_on(CONST_PRODUCER_CORE) == 0, "first producer run");
  CHECK(g_init_calls == 2u, "producer initialized exactly once");
  CHECK(g_const_run_state == CONST_RUN_COMPLETE, "producer completed");
  CHECK(g_stats.asyncCompiles == CONST_EXPECTED_COMPILES,
        "first run observed exactly two mock compiles");

  const uint32_t initAfterCompletion = g_init_calls;
  const uint64_t compilesAfterCompletion = g_stats.asyncCompiles;
  const uint32_t scaleAfterCompletion = g_const_scale;
  const int64_t biasAfterCompletion = g_const_bias;
  const ConstMode modeAfterCompletion = g_const_mode;

  CHECK(dump_on(CONST_WORKER_CORE) == 0, "first read-only dump");
  CHECK(dump_on(CONST_WORKER_CORE) == 0, "second read-only dump");
  CHECK(g_function_dump_calls == 2u && g_module_dump_calls == 2u,
        "both dump views repeated");
  CHECK(g_init_calls == initAfterCompletion,
        "dump does not initialize runtime");

  CHECK(run_on(CONST_PRODUCER_CORE) == 0, "completed producer revalidation");
  CHECK(g_init_calls == initAfterCompletion,
        "producer rerun does not reinitialize");
  CHECK(g_stats.asyncCompiles == compilesAfterCompletion,
        "producer rerun expects no new compile");
  CHECK(g_const_scale == scaleAfterCompletion &&
            g_const_bias == biasAfterCompletion &&
            g_const_mode == modeAfterCompletion,
        "producer rerun does not rewrite immutable globals");

  CHECK(run_on(3u) == -1, "wrong core rejected");
  CHECK(g_init_calls == initAfterCompletion,
        "wrong core rejected before initialization");

  __atomic_store_n(&g_const_run_state, CONST_RUN_ACTIVE, __ATOMIC_RELEASE);
  CHECK(run_on(CONST_PRODUCER_CORE) == -4, "re-entrant producer rejected");
  CHECK(g_init_calls == initAfterCompletion,
        "re-entrant producer rejected before initialization");
  __atomic_store_n(&g_const_run_state, CONST_RUN_COMPLETE, __ATOMIC_RELEASE);

  CHECK(g_runtime_ready[CONST_WORKER_CORE] != 0u,
        "worker remains live after all commands");
  if (g_failures != 0u)
    return 1;
  printf("PASS: const-after-init board command lifecycle\n");
  return 0;
}
