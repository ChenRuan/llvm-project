/**
 * EmbeddedJIT New Attribute Macro Test
 *
 * Verifies all 6 new UPPER_CASE attribute macros compile and JIT correctly.
 * Uses the new naming convention exclusively (no legacy compat aliases).
 *
 *   EJIT_PERIOD_CONST       ← was ejit_may_const
 *   EJIT_IN_PERIOD(x)       ← was ejit_period(x)
 *   EJIT_IN_PERIOD_ARRAY(x) ← was ejit_period_arr(x)
 *   EJIT_DIM(x)             ← was ejit_period_arr_ind(x)
 *   EJIT_ENTRY              ← was ejit_entry (new UPPER_CASE form)
 *   EJIT_PERIOD_GUARD(x)    ← was ejit_period_lc(x)
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ejit_test_helpers.h"

//===-- Structs using EJIT_PERIOD_CONST -----------------------------------===//

struct BoardCfg {
    EJIT_PERIOD_CONST uint32_t boardType;
    uint32_t _pad;
};

struct CellCfg {
    EJIT_PERIOD_CONST uint32_t cellType;
    uint32_t traffic;
};

//===-- Globals using EJIT_IN_PERIOD / EJIT_IN_PERIOD_ARRAY ---------------===//

EJIT_IN_PERIOD(static)  struct BoardCfg g_board;
EJIT_IN_PERIOD_ARRAY(cell) struct CellCfg g_cells[8];

//===-- JIT functions using EJIT_ENTRY / EJIT_DIM / EJIT_PERIOD_GUARD -----===//

EJIT_ENTRY uint32_t check_cell(EJIT_DIM(cell) uint8_t ci) {
    if (g_cells[ci].cellType == 42) return 100;
    if (g_cells[ci].cellType == 99) return 200;
    return 0;
}

EJIT_PERIOD_GUARD(cell)
void update_cell(EJIT_DIM(cell) uint8_t ci) {
    g_cells[ci].cellType = 99;
}

//===-- Main --------------------------------------------------------------===//

int main(int argc, char **argv) {
    uint8_t ci = (uint8_t)(argc > 1 ? (uint8_t)(argv[1][0] - '0') : 0);
    int failures = 0;

    printf("=== EJIT New Attribute Test ===\n");
    printf("cellIdx=%u\n\n", ci);

    // Init
    ejit_config_t cfg;
    ejit_default_config(&cfg);
    int rc = (int)ejit_init(&cfg);
    if (rc != 0) { printf("FAIL: init\n"); return 1; }
    printf("--- init OK ---\n");

    // Test 1: JIT compile with EJIT_PERIOD_CONST
    printf("\n--- 1. EJIT_PERIOD_CONST in struct ---\n");
    g_cells[ci].cellType = 42;
    ejit_activate("cell", ci);
    ejit_drain_taskpool();
    uint32_t r = check_cell(ci);
    if (r != 100) { printf("  FAIL: check_cell=%u (expected 100)\n", r); failures++; }
    else printf("  OK: check_cell=%u\n", r);

    // Test 2: EJIT_PERIOD_GUARD with EJIT_DIM
    printf("\n--- 2. EJIT_PERIOD_GUARD + EJIT_DIM ---\n");
    update_cell(ci);
    ejit_drain_taskpool();
    r = check_cell(ci);
    if (r != 200) { printf("  FAIL: after guard, check_cell=%u (expected 200)\n", r); failures++; }
    else printf("  OK: after guard, check_cell=%u\n", r);

    // Test 3: EJIT_IN_PERIOD static always active
    printf("\n--- 3. EJIT_IN_PERIOD(static) ---\n");
    g_board.boardType = 7;
    // static period is always active — no explicit activate needed
    // This just verifies the attribute compiles and the global is accessible
    printf("  OK: boardCfg.boardType=%u\n", g_board.boardType);

    printf("\n=== Result: %d failures ===\n", failures);
    ejit_shutdown();
    return failures > 0 ? 1 : 0;
}
