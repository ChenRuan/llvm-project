#include <stddef.h>
#include <stdint.h>

#ifndef EJIT_PROBE_NAME
#define EJIT_PROBE_NAME ejit_ir_pipeline_loop_probe
#endif

// Models a medium-size business loop that survives specialization: the period
// and may_const decisions are already resolved, but the data processed by each
// iteration remains dynamic.
__attribute__((noinline)) uint32_t
EJIT_PROBE_NAME(const uint16_t *__restrict lhs,
                const uint16_t *__restrict rhs, size_t count) {
  uint32_t sum = 0;
  for (size_t i = 0; i < count; ++i)
    sum += (uint32_t)lhs[i] * 3u + (uint32_t)rhs[i] * 5u;
  return sum;
}
