/**
 * EJIT volatile / atomic may_const test.
 *
 * A volatile or atomic field must be read on every access, so it can never be
 * folded into a constant -- even when it also carries ejit_may_const. Clang
 * already withholds the per-load !ejit.may_const marker from such loads, but the
 * GV-level ejit_may_const_field offset list records the field regardless, so the
 * offset-matching paths (reAnnotateMayConst at AOT, isMayConstLoad's fallback at
 * JIT time) could hand the marker back and let the value be substituted.
 *
 * The failure is invisible in a single call: the specialization is compiled with
 * whatever the field held at the time, and only a later write reveals that the
 * value was frozen. So the test compiles, then mutates, then re-reads.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ejit_test_helpers.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"

struct Cfg {
  ejit_may_const volatile uint32_t vol; /* must NOT be folded */
  ejit_may_const uint32_t atom;         /* read atomically -> must NOT be folded */
  ejit_may_const uint32_t norm;         /* plain -> folding is expected */
};
#define N 8
ejit_period_arr(cell) struct Cfg g_cfg[N];

ejit_entry
uint32_t read_vol(ejit_period_arr_ind(cell) uint8_t ci) {
  return g_cfg[ci].vol + g_cfg[ci].norm;
}

ejit_entry
uint32_t read_atomic(ejit_period_arr_ind(cell) uint8_t ci) {
  return __atomic_load_n(&g_cfg[ci].atom, __ATOMIC_RELAXED) + g_cfg[ci].norm;
}

static int failures = 0;

#define CHECK(cond, fmt, ...)                                                 \
  do {                                                                        \
    if (cond) {                                                               \
      printf("  OK  : " fmt "\n", ##__VA_ARGS__);                             \
    } else {                                                                  \
      printf("  FAIL: " fmt "\n", ##__VA_ARGS__);                             \
      failures++;                                                             \
    }                                                                         \
  } while (0)

int main(int argc, char **argv) {
  const uint8_t ci = (argc >= 2) ? (uint8_t)atoi(argv[1]) : 5;

  printf("=== EJIT volatile / atomic may_const test (cellIdx=%u) ===\n", ci);

  g_cfg[ci].vol = 10;
  g_cfg[ci].atom = 30;
  g_cfg[ci].norm = 100;

  ejit_config_t cfg;
  ejit_default_config(&cfg);
  if (ejit_init(&cfg) != EJIT_OK) {
    printf("  FAIL: ejit_init\n");
    return 1;
  }
  ejit_activate("cell", ci);

  /* Force the specialization to be compiled against the current values. */
  (void)read_vol(ci);
  (void)read_atomic(ci);
  ejit_drain_taskpool();

  CHECK(read_vol(ci) == 110, "volatile: JIT-served read = 110 (10 + 100)");
  CHECK(read_atomic(ci) == 130, "atomic:   JIT-served read = 130 (30 + 100)");

  /* Mutate the fields the JIT must NOT have frozen. A substituted load keeps
     returning the stale value; a real load picks the new one up. */
  g_cfg[ci].vol = 20;
  __atomic_store_n(&g_cfg[ci].atom, 60, __ATOMIC_RELAXED);

  CHECK(read_vol(ci) == 120,
        "volatile field is re-read after it changes (want 120, got %u)",
        read_vol(ci));
  CHECK(read_atomic(ci) == 160,
        "atomic field is re-read after it changes (want 160, got %u)",
        read_atomic(ci));

  ejit_shutdown();

  printf("\n=== %s: %d failure(s) ===\n", failures ? "FAIL" : "PASS", failures);
  return failures ? 1 : 0;
}