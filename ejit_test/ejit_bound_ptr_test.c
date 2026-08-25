//===-- ejit_bound_ptr_test.c - Dimension-bound pointee snapshot test -----===//

#include <stdint.h>
#include <stdio.h>

#include "ejit_test_helpers.h"

typedef struct {
  uint32_t ejit_may_const algorithm;
  uint32_t ejit_may_const scale;
  uint32_t runtimeBias;
} CellRelated;

ejit_period_arr(cell) uint32_t g_cells[8];
ejit_period_arr(trp) uint32_t g_trps[8];

ejit_entry uint32_t bound_cell_config(ejit_period_arr_ind(cell)
                                          uint8_t cellIndex,
                                      ejit_period_arr_ind(trp) uint8_t trpIndex,
                                      EJIT_BOUND_PTR(cell)
                                          const CellRelated *cellRelated,
                                      uint32_t input) {
  if (cellRelated->algorithm == 7)
    return input * cellRelated->scale + cellRelated->runtimeBias + cellIndex +
           trpIndex;
  return input + cellRelated->runtimeBias;
}

static uint32_t call_with_stack_instance(uint8_t cellIndex, uint8_t trpIndex,
                                         uint32_t runtimeBias) {
  CellRelated Local = {7, 5, runtimeBias};
  return bound_cell_config(cellIndex, trpIndex, &Local, 10);
}

int main(void) {
  const uint8_t Cell = 3;
  const uint8_t Trp = 2;
  ejit_config_t Config;
  ejit_default_config(&Config);
  if (ejit_init(&Config) != EJIT_OK)
    return 1;
  if (ejit_activate("cell", Cell) != EJIT_OK ||
      ejit_activate("trp", Trp) != EJIT_OK)
    return 2;

  ejit_dump_func("bound_cell_config");
  uint32_t Aot = call_with_stack_instance(Cell, Trp, 100);
  ejit_drain_taskpool();

  // The first stack object is gone. The specialization must have consumed its
  // owned snapshot, while this call's dynamic field must still be read here.
  uint32_t Jit = call_with_stack_instance(Cell, Trp, 200);
  uint32_t ExpectedAot = 10 * 5 + 100 + Cell + Trp;
  uint32_t ExpectedJit = 10 * 5 + 200 + Cell + Trp;
  printf("AOT=%u expected=%u JIT=%u expected=%u\n", Aot, ExpectedAot, Jit,
         ExpectedJit);
  ejit_print_dumped("bound_cell_config");
  if (Aot != ExpectedAot || Jit != ExpectedJit) {
    printf("FAIL\n");
    return 3;
  }
  printf("PASS\n");
  return 0;
}
