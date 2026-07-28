/**
 * EJIT LTO 跨模块内联集成测试 — 主 TU
 *
 * 目的: 验证 ThinLTO / FullLTO 下, 其他 TU 的子函数在 AOT 阶段被内联进
 * ejit_entry, 且 !ejit.may_const metadata 得到保留, JIT 可以正确特化。
 *
 * 本测试由两个 TU 组成:
 *   - ejit_lto_inline_test.c   (本文件): ejit_entry + period 定义
 *   - ejit_lto_inline_test_b.c (第二 TU): 被 ejit_entry 调用的子函数
 *
 * 编译:  ./build.sh --arch=aarch64 ejit_lto_inline_test
 * 运行:  ./out/ejit_lto_inline_test [cellIdx]    (默认 0)
 *
 * 验证点:
 *   1. LTO 下子函数被内联, JIT 编译成功 (entries >= 1)
 *   2. 所有特化值正确 (不同 cellIdx 产生不同特化结果)
 *   3. 二次调用命中缓存
 *   4. static 时间窗 + ejit_period_arr 时间窗同时工作
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ejit_test_helpers.h"

//===-- TU-A 的 EJIT 数据 --------------------------------------------------===//

struct BoardCfg {
  ejit_may_const uint32_t boardType;
  ejit_may_const uint32_t revision;
  uint32_t runtimeCounter;
};

struct CellCfg {
  ejit_may_const uint32_t cellType;   // FDD=0xBB, TDD=0xCC
  ejit_may_const uint32_t cellId;
  uint32_t trafficLoad;               // 非 may_const, 运行期可变
};

#define N_CELL 8

ejit_period(static) struct BoardCfg g_boardCfg;
ejit_period_arr(cell)  struct CellCfg  g_cellCfg[N_CELL];

//===-- TU-B 的接口 --------------------------------------------------------===//

// 子函数在 TU-B 中定义, 访问 TU-A 的 ejit_may_const 字段。
// LTO 下应被内联到调用方。
extern uint32_t lookup_board_type(void);           // reads g_boardCfg.boardType
extern uint32_t lookup_cell_type(uint8_t idx);     // reads g_cellCfg[idx].cellType
extern uint32_t compute_priority(uint8_t idx);     // reads boardType + cellType
extern uint32_t chain_call(uint8_t idx);           // calls lookup_cell_type

extern void b_seed_cell(uint8_t idx, uint32_t cellType);

//===-- EJIT entry 函数 ----------------------------------------------------===//

// entry 1: 仅依赖 static 时间窗, 调用链跨越 TU
ejit_entry
uint32_t jit_board_check(void)
{
  // lookup_board_type() in TU-B reads g_boardCfg.boardType (may_const)
  uint32_t t = lookup_board_type();
  if (t == 0xB0)
    return 300;
  return 3;
}

// entry 2: 依赖 cell 时间窗, 子函数在 TU-B
ejit_entry
uint32_t jit_cell_check(ejit_period_arr_ind(cell) uint8_t idx)
{
  // lookup_cell_type() in TU-B reads g_cellCfg[idx].cellType (may_const)
  uint32_t t = lookup_cell_type(idx);
  if (t == 0xBB)
    return 200;
  if (t == 0xCC)
    return 300;
  return 4;
}

// entry 3: 多级调用链, 子函数调用子函数
ejit_entry
uint32_t jit_chain_check(ejit_period_arr_ind(cell) uint8_t idx)
{
  // chain_call() → lookup_cell_type() in TU-B
  uint32_t t = chain_call(idx);
  if (t == 0xBB)
    return 200;
  if (t == 0xCC)
    return 300;
  return 5;
}

// entry 4: 同时依赖 static + cell
ejit_entry
uint32_t jit_priority(ejit_period_arr_ind(cell) uint8_t idx)
{
  // compute_priority() in TU-B reads both g_boardCfg.boardType and
  // g_cellCfg[idx].cellType
  uint32_t p = compute_priority(idx);
  // boardType=0xB0 时 +100, cellType=0xBB 时 +200
  return p;
}

//===-- 运行时 API (来自 helpers / EJitRuntime.h) ---------------------------===//

extern void ejit_shutdown(void);

//===-- 断言 ---------------------------------------------------------------===//

static int g_failures = 0;

#define VERIFY(cond, fmt, ...) do {                  \
  if (!(cond)) {                                     \
    printf("  FAIL: " fmt "\n", ##__VA_ARGS__);      \
    g_failures++;                                    \
  } else {                                           \
    printf("  OK:   " fmt "\n", ##__VA_ARGS__);      \
  }                                                  \
} while (0)

//===-- main ---------------------------------------------------------------===//

int main(int argc, char **argv)
{
  uint8_t ci = (argc >= 2) ? (uint8_t)atoi(argv[1]) : 0;
  if (ci >= N_CELL) ci = 0;

  printf("=== EJIT LTO cross-module inline test ===\n");
  printf("cellIdx=%u\n\n", ci);

  // Seed data
  g_boardCfg.boardType = 0xB0;
  g_boardCfg.revision  = 2;
  b_seed_cell(ci, 0xBB);          // TU-B seeds cell data in TU-A

  // Init
  ejit_config_t cfg;
  ejit_default_config(&cfg);
  // Use default (async under taskpool) — drain after calls for reliable results
  int rc = ejit_init(&cfg);
  VERIFY(rc == 0, "ejit_init returned %d", rc);

  ejit_activate("cell", ci);

  //=== Test 1: static-only entry ===
  printf("\n--- static-only (boardType=0xB0) ---\n");
  uint32_t r1 = jit_board_check();
  VERIFY(r1 == 300, "jit_board_check() = %u (expected 300)", r1);

  //=== Test 2: cell-dependent entry ===
  printf("\n--- cell-dependent (cellType=0xBB) ---\n");
  uint32_t r2 = jit_cell_check(ci);
  VERIFY(r2 == 200, "jit_cell_check(%u) = %u (expected 200, cellType=0xBB)", ci, r2);

  //=== Test 3: chain call ===
  printf("\n--- chain call ---\n");
  uint32_t r3 = jit_chain_check(ci);
  VERIFY(r3 == 200, "jit_chain_check(%u) = %u (expected 200)", ci, r3);

  //=== Test 4: multi-period dependency ===
  printf("\n--- multi-period (static+cell) ---\n");
  uint32_t r4 = jit_priority(ci);
  // boardType=0xB0 → +100, cellType=0xBB → +200 → 300
  VERIFY(r4 == 300, "jit_priority(%u) = %u (expected 300)", ci, r4);

  //=== Cache stats ===
  ejit_drain_taskpool();
#ifdef EJIT_SRE_SHARED_TASKPOOL
  {
    ejit_taskpool_stats_t ts;
    memset(&ts, 0, sizeof(ts));
    ejit_taskpool_get_stats(&ts);
    printf("\n--- Taskpool stats ---\n");
    printf("  ready=%u hits=%llu compiles=%llu\n",
           ts.readyEntries, (unsigned long long)ts.cacheHits,
           (unsigned long long)ts.asyncCompiles);
    VERIFY(ts.asyncCompiles >= 2,
           "JIT compiles >= 2 (at least 2 cache keys), actual %llu",
           (unsigned long long)ts.asyncCompiles);
    VERIFY(ts.readyEntries >= 2,
           "JIT entries >= 2, actual %u", ts.readyEntries);
  }
#else
  {
    ejit_stats_t s1;
    ejit_get_stats(&s1);
    printf("\n--- Cache stats ---\n");
    printf("  entries=%zu misses=%zu hits=%zu\n", s1.entryCount, s1.misses, s1.hits);
    VERIFY(s1.misses >= 2, "JIT misses >= 2, actual %zu", s1.misses);
    VERIFY(s1.entryCount >= 2, "JIT entries >= 2, actual %zu", s1.entryCount);
  }
#endif

  //=== Second calls: verify JIT specialization is persistent ===
  printf("\n--- Second calls (should hit cache) ---\n");

  uint32_t r1b = jit_board_check();
  uint32_t r2b = jit_cell_check(ci);
  uint32_t r3b = jit_chain_check(ci);
  uint32_t r4b = jit_priority(ci);
  VERIFY(r1b == 300, "jit_board_check() 2nd = %u", r1b);
  VERIFY(r2b == 200, "jit_cell_check(%u) 2nd = %u", ci, r2b);
  VERIFY(r3b == 200, "jit_chain_check(%u) 2nd = %u", ci, r3b);
  VERIFY(r4b == 300, "jit_priority(%u) 2nd = %u", ci, r4b);

  ejit_drain_taskpool();
#ifdef EJIT_SRE_SHARED_TASKPOOL
  {
    ejit_taskpool_stats_t ts2;
    memset(&ts2, 0, sizeof(ts2));
    ejit_taskpool_get_stats(&ts2);
    VERIFY(ts2.cacheHits >= 2, "Cache hits >= 2, actual %llu",
           (unsigned long long)ts2.cacheHits);
    printf("\n  Taskpool: ready=%u hits=%llu compiles=%llu\n",
           ts2.readyEntries, (unsigned long long)ts2.cacheHits,
           (unsigned long long)ts2.asyncCompiles);
  }
#else
  {
    ejit_stats_t s2;
    ejit_get_stats(&s2);
    VERIFY(s2.hits >= 2, "Cache hits >= 2, actual %zu", s2.hits);
    printf("\n  Sync JIT: entries=%zu misses=%zu hits=%zu\n",
           s2.entryCount, s2.misses, s2.hits);
  }
#endif

  ejit_shutdown();

  printf("\n=== %s (%d failures) ===\n",
         g_failures == 0 ? "PASS" : "FAIL", g_failures);
  return g_failures == 0 ? 0 : 1;
}
