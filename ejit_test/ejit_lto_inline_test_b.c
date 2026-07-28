/**
 * EJIT LTO 跨模块内联集成测试 — 第二 TU
 *
 * 与 ejit_lto_inline_test.c 配对。本 TU 包含被 ejit_entry 函数调用的
 * 子函数, 这些子函数访问主 TU 中标记了 ejit_may_const 的全局变量字段。
 *
 * LTO 下这些函数应被内联到主 TU 的 ejit_entry 中, 使 PASS6 可以识别
 * may_const load 并替换为运行时常量。
 */

#include <stdint.h>

#include "ejit_test_helpers.h"

//===-- 与主 TU 共享的结构体声明 (ABI 兼容) ---------------------------------===//

struct BoardCfg {
  uint32_t boardType;
  uint32_t revision;
  uint32_t runtimeCounter;
};

struct CellCfg {
  uint32_t cellType;
  uint32_t cellId;
  uint32_t trafficLoad;
};

extern struct BoardCfg g_boardCfg;
extern struct CellCfg  g_cellCfg[];

//===-- 子函数: 访问 may_const 字段 ----------------------------------------===//

// 读取 g_boardCfg.boardType (ejit_may_const)
// LTO 内联后, JIT 应将其替换为常量
uint32_t lookup_board_type(void)
{
  if (g_boardCfg.boardType == 0xB0)
    return 0xB0;
  return 0;
}

// 读取 g_cellCfg[idx].cellType (ejit_may_const)
// LTO 内联后, JIT 应特化常量并折叠分支
uint32_t lookup_cell_type(uint8_t idx)
{
  if (g_cellCfg[idx].cellType == 0xBB)
    return 0xBB;
  if (g_cellCfg[idx].cellType == 0xCC)
    return 0xCC;
  return 0;
}

// 同时依赖两个时间窗: boardType (static) + cellType (cell)
uint32_t compute_priority(uint8_t idx)
{
  uint32_t p = 0;
  if (g_boardCfg.boardType == 0xB0)
    p += 100;
  if (g_cellCfg[idx].cellType == 0xBB)
    p += 200;
  else if (g_cellCfg[idx].cellType == 0xCC)
    p += 300;
  return p;
}

// 多级调用: chain_call → lookup_cell_type
// 验证 LTO 能内联整个调用链
uint32_t chain_call(uint8_t idx)
{
  return lookup_cell_type(idx);
}

// 便利函数: 让主 TU 可以从外部设置 g_cellCfg
void b_seed_cell(uint8_t idx, uint32_t cellType)
{
  // 直接写入主 TU 的全局数组
  g_cellCfg[idx].cellType = cellType;
}
