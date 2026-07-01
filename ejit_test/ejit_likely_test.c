/**
 * EJIT __builtin_expect (MC_LIKELY) 回归测试
 *
 * 背景:
 *   业务在 ejit_entry 函数中使用 #define MC_LIKELY(x) (__builtin_expect((!!(x)),1)),
 *   编译器在 lower-expect pass 崩溃。根因是 PASS3 (EJitWrapperGen) 重写 ejit_entry
 *   入口块时, replaceAllUsesWith 无法更新 PHI incoming block (它不在 BasicBlock 的
 *   use list 里), erase 原入口块后 PHI incoming 变悬空指针, lower-expect 的
 *   handlePhiDef 解引用崩溃。
 *
 *   短路 && / || 会在 merge block 产生 PHI (incoming 含入口块), 恰好触发该 bug。
 *   本测试覆盖以下形态, 验证修复后 JIT 特化仍正确:
 *     1. MC_LIKELY(a && b)  — 短路 && 产生 PHI, bug 的最小复现
 *     2. MC_LIKELY(a || b)  — 短路 || 产生 PHI
 *     3. MC_UNLIKELY(a)     — __builtin_expect(..., 0)
 *     4. 循环条件 + MC_LIKELY
 *     5. 嵌套 MC_LIKELY
 *
 * 运行:
 *   ./ejit_likely_test        # 默认 cellIdx=0
 *   ./ejit_likely_test 1      # cellIdx=1
 *
 * 编译 (-O1/-O2/-O3 任意, 修复前 -O1/-O2/-O3 全崩在 lower-expect):
 *   build/bin/clang -O2 -c ejit_likely_test.c -o ejit_likely_test.o
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define MC_LIKELY(x)   (__builtin_expect((!!(x)), 1))
#define MC_UNLIKELY(x) (__builtin_expect((!!(x)), 0))

//===-- 结构体与全局变量 ---------------------------------------------------===//

struct CellCfg {
  __attribute__((ejit_may_const)) uint32_t cellType;
  __attribute__((ejit_may_const)) uint32_t cellId;
  uint32_t trafficLoad;
};

#define N_CELL 16
__attribute__((ejit_period_arr("cell"))) struct CellCfg g_cellCfg[N_CELL];

//===-- ejit_entry 函数 (均使用 __builtin_expect) -------------------------===//

// 1. 短路 && — bug 最小复现: merge block 的 PHI incoming 含入口块
__attribute__((ejit_entry))
uint32_t likely_and(
    __attribute__((ejit_period_arr_ind("cell"))) uint8_t cellIdx)
{
  uint32_t ct = g_cellCfg[cellIdx].cellType;
  if (MC_LIKELY(ct == 0xFD && g_cellCfg[cellIdx].cellId < 1000))
    return 1000;
  return 0;
}

// 2. 短路 || — 同样产生 merge PHI
__attribute__((ejit_entry))
uint32_t likely_or(
    __attribute__((ejit_period_arr_ind("cell"))) uint8_t cellIdx)
{
  uint32_t ct = g_cellCfg[cellIdx].cellType;
  if (MC_LIKELY(ct == 0xFD || ct == 0xEC))
    return 2000;
  return 0;
}

// 3. MC_UNLIKELY — expect 的第二参数为 0
__attribute__((ejit_entry))
uint32_t unlikely_branch(
    __attribute__((ejit_period_arr_ind("cell"))) uint8_t cellIdx)
{
  if (MC_UNLIKELY(g_cellCfg[cellIdx].cellType == 0xFF))
    return 3000;
  return 1;
}

// 4. 循环条件使用 MC_LIKELY
__attribute__((ejit_entry))
uint32_t likely_loop(
    __attribute__((ejit_period_arr_ind("cell"))) uint8_t cellIdx)
{
  uint32_t s = 0;
  uint32_t bound = g_cellCfg[cellIdx].cellId;   // may_const, JIT 时常量化
  for (uint32_t i = 0; MC_LIKELY(i < bound); i++)
    s += 1;
  return s;
}

// 5. 嵌套 MC_LIKELY — 多层短路
__attribute__((ejit_entry))
uint32_t likely_nested(
    __attribute__((ejit_period_arr_ind("cell"))) uint8_t cellIdx)
{
  uint32_t ct = g_cellCfg[cellIdx].cellType;
  uint32_t id = g_cellCfg[cellIdx].cellId;
  if (MC_LIKELY(ct == 0xFD)) {
    if (MC_LIKELY(id > 0 && id < 1000))
      return 5000;
  }
  return 0;
}

//===-- 运行时 API ---------------------------------------------------------===//

extern int ejit_init(const void *cfg);
extern int ejit_activate(const char *name, unsigned char idx);
extern void ejit_shutdown(void);

typedef struct {
  uint32_t entries;
  uint64_t codeSize;
  uint64_t maxSize;
  uint64_t hits;
  uint64_t misses;
  uint64_t evictions;
  uint32_t active;
} ejit_stats_t;

extern int ejit_get_stats(ejit_stats_t *stats);

#ifdef EJIT_SRE_SHARED_TASKPOOL
#include <string.h>
// In a shared-taskpool build the async AOT wrapper drives
// ejit_taskpool_compile_or_get instead of the legacy LRU ABI, and the single
// cross-core worker is started ONLY by owner election inside ejit_init when the
// compile mode is Async. A mode flip after a Sync init cannot start the
// owner-controlled worker, so the test must initialize in Async mode (matching
// the canonical ejit_config_t ABI) to exercise the JIT at all.
typedef struct {
  int compileMode;          // ejit_compile_mode_t: 1 = EJIT_COMPILE_ASYNC
  int optLevel;             // ejit_opt_level_t:    2 = EJIT_OPT_L2
  size_t maxCodeMemory, maxDataMemory, maxCacheEntries, maxCacheSize;
  _Bool enableLogger, forceStaticRegistry;
  const char *dumpJITDir;
} ejit_async_config_t;
static const ejit_async_config_t ejit_async_cfg = {
    .compileMode = 1, .optLevel = 2,
    .maxCodeMemory = 512 * 1024, .maxDataMemory = 256 * 1024,
    .maxCacheEntries = 64, .maxCacheSize = 1024 * 1024,
};
#define EJIT_INIT_CFG (&ejit_async_cfg)
typedef struct {
  unsigned long long cacheHits, asyncCompiles, asyncEnqueues, alreadyPending;
  unsigned long long queueFull, compileFailed, publishFailed, instanceDisabled;
  unsigned readyEntries, pendingEntries, queueApproxSize, reserved;
} ejit_taskpool_stats_t;
extern unsigned ejit_taskpool_pending_count(void);
extern int ejit_taskpool_get_stats(ejit_taskpool_stats_t *out);
static void ejit_drain_taskpool(void) {
  while (ejit_taskpool_pending_count() > 0)
    ;
}
#else
#define EJIT_INIT_CFG (0)
static void ejit_drain_taskpool(void) {}
#endif

//===-- 断言 ---------------------------------------------------------------===//

static int g_failures = 0;

#define VERIFY(cond, fmt, ...) do {                                \
  if (!(cond)) {                                                    \
    printf("  FAIL: " fmt "\n", ##__VA_ARGS__);                    \
    g_failures++;                                                   \
  } else {                                                          \
    printf("  OK:   " fmt "\n", ##__VA_ARGS__);                    \
  }                                                                 \
} while (0)

//===-- main ---------------------------------------------------------------===//

int main(int argc, char **argv)
{
  uint8_t ci = (argc >= 2) ? (uint8_t)atoi(argv[1]) : 0;

  printf("=== EJIT __builtin_expect (MC_LIKELY) Regression Test ===\n");
  printf("cellIdx=%u\n\n", ci);

  // Init data: even cells -> 0xFD, odd -> 0xEC; cellId == cellIdx
  for (int i = 0; i < N_CELL; i++) {
    g_cellCfg[i].cellType    = (i % 2) ? 0xEC : 0xFD;
    g_cellCfg[i].cellId      = (uint32_t)i;
    g_cellCfg[i].trafficLoad = 0;
  }

  int rc = ejit_init(EJIT_INIT_CFG);
  VERIFY(rc == 0, "ejit_init returned %d", rc);
  ejit_activate("cell", ci);

  uint32_t ct = g_cellCfg[ci].cellType;
  uint32_t id = g_cellCfg[ci].cellId;

  // 1. MC_LIKELY(a && b)
  printf("\n--- likely_and(%u) ---\n", ci);
  uint32_t r1 = likely_and(ci);
  uint32_t e1 = (ct == 0xFD && id < 1000) ? 1000 : 0;
  VERIFY(r1 == e1, "likely_and=%u (expected %u)", r1, e1);

  // 2. MC_LIKELY(a || b)
  printf("\n--- likely_or(%u) ---\n", ci);
  uint32_t r2 = likely_or(ci);
  uint32_t e2 = (ct == 0xFD || ct == 0xEC) ? 2000 : 0;
  VERIFY(r2 == e2, "likely_or=%u (expected %u)", r2, e2);

  // 3. MC_UNLIKELY(a)
  printf("\n--- unlikely_branch(%u) ---\n", ci);
  uint32_t r3 = unlikely_branch(ci);
  uint32_t e3 = (ct == 0xFF) ? 3000 : 1;
  VERIFY(r3 == e3, "unlikely_branch=%u (expected %u)", r3, e3);

  // 4. MC_LIKELY in loop condition
  printf("\n--- likely_loop(%u) ---\n", ci);
  uint32_t r4 = likely_loop(ci);
  VERIFY(r4 == id, "likely_loop=%u (expected %u)", r4, id);

  // 5. nested MC_LIKELY
  printf("\n--- likely_nested(%u) ---\n", ci);
  uint32_t r5 = likely_nested(ci);
  uint32_t e5 = (ct == 0xFD && id > 0 && id < 1000) ? 5000 : 0;
  VERIFY(r5 == e5, "likely_nested=%u (expected %u)", r5, e5);

  // JIT 编译确实发生 (修复前根本无法编译到这里)
  printf("\n--- JIT Stats ---\n");
  ejit_drain_taskpool();
#ifdef EJIT_SRE_SHARED_TASKPOOL
  ejit_taskpool_stats_t tp; memset(&tp, 0, sizeof(tp));
  ejit_taskpool_get_stats(&tp);
  printf("  ready=%u  hits=%llu  compiles=%llu\n",
         tp.readyEntries, (unsigned long long)tp.cacheHits,
         (unsigned long long)tp.asyncCompiles);
  VERIFY(tp.asyncCompiles >= 1, "JIT compiles >= 1 (actual %llu)",
         (unsigned long long)tp.asyncCompiles);
#else
  ejit_stats_t s;
  ejit_get_stats(&s);
  printf("  entries=%u  hits=%llu  misses=%llu\n",
         s.entries, (unsigned long long)s.hits, (unsigned long long)s.misses);
  VERIFY(s.entries >= 1, "JIT entries >= 1 (actual %u)", s.entries);
#endif

  ejit_shutdown();
  printf("\n=== %s (%d failure(s)) ===\n",
         g_failures ? "FAILED" : "ALL PASSED", g_failures);
  return g_failures ? 1 : 0;
}
