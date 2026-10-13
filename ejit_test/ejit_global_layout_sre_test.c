// Cold board flow: core 6 test_ejit_period initializes the single worker;
// core 16 test_ejit_period runs the bounded workload. Do not shutdown between
// shell calls. Use the existing SDK registry/init-array and shared-code setup.
// Build the runtime with worker core 6, four PGO admissions, audit/diagnostics
// ON. a==0 selects Baseline, a!=0 selects real online PGO; use the same value
// on both cores after a fresh reset. This is a demo fixture, NOT board
// evidence.
#include "ejit_test_helpers.h"
#include <stdint.h>

extern void SRE_printf(const char *, ...);
extern uint32_t SRE_TaskDelay(uint32_t);
extern void call_init_array_functions(void);
extern uint8_t g_ucLocalCoreID;

static __attribute__((noinline)) uint32_t layout_business(uint32_t input,
                                                          uint32_t tag) {
  uint32_t value = input + tag;
  for (uint32_t i = 0; i < 16; ++i)
    if ((value ^ i) & 1)
      value = value * 3 + i;
    else
      value = value + 7 + tag;
  return value;
}
#define LAYOUT_ENTRY(NAME, TAG)                                                \
  ejit_entry uint32_t global_layout_##NAME(uint32_t x) {                       \
    return layout_business(x, TAG);                                            \
  }
LAYOUT_ENTRY(A, 0u)
LAYOUT_ENTRY(B, 1u)
LAYOUT_ENTRY(C, 2u)
LAYOUT_ENTRY(D, 3u)
LAYOUT_ENTRY(E, 4u)
LAYOUT_ENTRY(F, 5u)
LAYOUT_ENTRY(G, 6u)
LAYOUT_ENTRY(H, 7u)
LAYOUT_ENTRY(I, 8u)
LAYOUT_ENTRY(J, 9u)
LAYOUT_ENTRY(K, 10u)
LAYOUT_ENTRY(L, 11u)
LAYOUT_ENTRY(M, 12u)
LAYOUT_ENTRY(N, 13u)
LAYOUT_ENTRY(O, 14u)
LAYOUT_ENTRY(P, 15u)
LAYOUT_ENTRY(Q, 16u)
LAYOUT_ENTRY(R, 17u)
LAYOUT_ENTRY(S, 18u)
LAYOUT_ENTRY(T, 19u)
LAYOUT_ENTRY(U, 20u)
LAYOUT_ENTRY(V, 21u)
LAYOUT_ENTRY(W, 22u)
LAYOUT_ENTRY(X, 23u)
LAYOUT_ENTRY(Y, 24u)
LAYOUT_ENTRY(Z, 25u)

typedef uint32_t (*layout_fn_t)(uint32_t);
static layout_fn_t entries[26] = {
    global_layout_A, global_layout_B, global_layout_C, global_layout_D,
    global_layout_E, global_layout_F, global_layout_G, global_layout_H,
    global_layout_I, global_layout_J, global_layout_K, global_layout_L,
    global_layout_M, global_layout_N, global_layout_O, global_layout_P,
    global_layout_Q, global_layout_R, global_layout_S, global_layout_T,
    global_layout_U, global_layout_V, global_layout_W, global_layout_X,
    global_layout_Y, global_layout_Z};
extern uint32_t __ejit_funcidx_global_layout_A;
extern uint32_t __ejit_funcidx_global_layout_B;
extern uint32_t __ejit_funcidx_global_layout_C;
extern uint32_t __ejit_funcidx_global_layout_D;
extern uint32_t __ejit_funcidx_global_layout_E;
extern uint32_t __ejit_funcidx_global_layout_F;
extern uint32_t __ejit_funcidx_global_layout_G;
extern uint32_t __ejit_funcidx_global_layout_H;
extern uint32_t __ejit_funcidx_global_layout_I;
extern uint32_t __ejit_funcidx_global_layout_J;
extern uint32_t __ejit_funcidx_global_layout_K;
extern uint32_t __ejit_funcidx_global_layout_L;
extern uint32_t __ejit_funcidx_global_layout_M;
extern uint32_t __ejit_funcidx_global_layout_N;
extern uint32_t __ejit_funcidx_global_layout_O;
extern uint32_t __ejit_funcidx_global_layout_P;
extern uint32_t __ejit_funcidx_global_layout_Q;
extern uint32_t __ejit_funcidx_global_layout_R;
extern uint32_t __ejit_funcidx_global_layout_S;
extern uint32_t __ejit_funcidx_global_layout_T;
extern uint32_t __ejit_funcidx_global_layout_U;
extern uint32_t __ejit_funcidx_global_layout_V;
extern uint32_t __ejit_funcidx_global_layout_W;
extern uint32_t __ejit_funcidx_global_layout_X;
extern uint32_t __ejit_funcidx_global_layout_Y;
extern uint32_t __ejit_funcidx_global_layout_Z;
static uint32_t *indices[26] = {
    &__ejit_funcidx_global_layout_A, &__ejit_funcidx_global_layout_B,
    &__ejit_funcidx_global_layout_C, &__ejit_funcidx_global_layout_D,
    &__ejit_funcidx_global_layout_E, &__ejit_funcidx_global_layout_F,
    &__ejit_funcidx_global_layout_G, &__ejit_funcidx_global_layout_H,
    &__ejit_funcidx_global_layout_I, &__ejit_funcidx_global_layout_J,
    &__ejit_funcidx_global_layout_K, &__ejit_funcidx_global_layout_L,
    &__ejit_funcidx_global_layout_M, &__ejit_funcidx_global_layout_N,
    &__ejit_funcidx_global_layout_O, &__ejit_funcidx_global_layout_P,
    &__ejit_funcidx_global_layout_Q, &__ejit_funcidx_global_layout_R,
    &__ejit_funcidx_global_layout_S, &__ejit_funcidx_global_layout_T,
    &__ejit_funcidx_global_layout_U, &__ejit_funcidx_global_layout_V,
    &__ejit_funcidx_global_layout_W, &__ejit_funcidx_global_layout_X,
    &__ejit_funcidx_global_layout_Y, &__ejit_funcidx_global_layout_Z};
static const char *names[26] = {
    "global_layout_A", "global_layout_B", "global_layout_C", "global_layout_D",
    "global_layout_E", "global_layout_F", "global_layout_G", "global_layout_H",
    "global_layout_I", "global_layout_J", "global_layout_K", "global_layout_L",
    "global_layout_M", "global_layout_N", "global_layout_O", "global_layout_P",
    "global_layout_Q", "global_layout_R", "global_layout_S", "global_layout_T",
    "global_layout_U", "global_layout_V", "global_layout_W", "global_layout_X",
    "global_layout_Y", "global_layout_Z"};

int test_ejit_period(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)b;
  (void)c;
  (void)d;
  uint32_t core = g_ucLocalCoreID;
  if (core != 6 && core != 16)
    return -1;
  if (core == 6)
    call_init_array_functions();
  ejit_config_t config;
  ejit_default_config(&config);
  config.compileMode = EJIT_COMPILE_ASYNC;
  config.forceStaticRegistry = true;
  ejit_layout_target_t manifest[26] = {{0}};
  for (uint32_t i = 0; i < 26; ++i)
    manifest[i].entryName = names[i]; // Explicit A..Z rank, no dimensions.
  if (ejit_init_global_layout(&config, manifest, 26, a != 0) != EJIT_OK) {
    SRE_printf("[GLOBAL-LAYOUT] initialization failed core=%u\n", core);
    return -2;
  }
  if (core == 6) {
    SRE_printf("[GLOBAL-LAYOUT] worker ready core=6 targets=26 pgo=%u\n",
               a != 0);
    return 0;
  }
  ejit_global_layout_status_t status = {0};
  for (uint32_t round = 0; round < 6000; ++round) {
    // Arrival starts at H; the final allocation MUST still be A..Z.
    for (uint32_t j = 0; j < 26; ++j) {
      uint32_t rank = (j + 7) % 26;
      uint32_t input = round + 11;
      if (entries[rank](input) != layout_business(input, rank))
        return -3;
    }
    if (ejit_get_global_layout_status(&status) != EJIT_OK)
      return -4;
    if (status.state == 7 || status.state == 8) {
      SRE_printf("[GLOBAL-LAYOUT] FAILED state=%u rank=%u error=%u "
                 "ready=%u/%u published=%u near=%llu far=%llu\n",
                 status.state, status.errorRank, status.error, status.ready,
                 status.required, status.published,
                 (unsigned long long)status.retainedNearBytes,
                 (unsigned long long)status.retainedFarBytes);
      return -5;
    }
    if (status.state == 6)
      break;
    if ((round % 100) == 0)
      SRE_printf("[GLOBAL-LAYOUT] collecting ready=%u/%u active=%u "
                 "object=%llu finalReserved=%llu\n",
                 status.ready, status.required, status.activeProfiles,
                 (unsigned long long)status.artifactBytes,
                 (unsigned long long)status.retainedNearBytes);
    (void)SRE_TaskDelay(10);
  }
  if (status.state != 6 || status.required != 26 || status.published != 26)
    return -6;
  uintptr_t previous = 0;
  for (uint32_t rank = 0; rank < 26; ++rank) {
    void *body = 0;
    uint32_t token = 0;
    int rc = ejit_taskpool_compile_or_get(*indices[rank], 0, 0, &body, &token);
    if (rc != EJIT_OK || !body || (uintptr_t)body <= previous) {
      if (rc == EJIT_OK && body)
        ejit_taskpool_release_read(token);
      return -7;
    }
    previous = (uintptr_t)body;
    uint32_t actual = ((layout_fn_t)body)(19);
    ejit_taskpool_release_read(token);
    if (actual != layout_business(19, rank))
      return -8;
    SRE_printf("[GLOBAL-LAYOUT] rank=%u name=%s realBody=%p\n", rank,
               names[rank], body);
  }
  ejit_print_code_pool_stats();
  SRE_printf("[GLOBAL-LAYOUT] COMPLETE core16 business/order checks passed\n");
  return 0;
}
