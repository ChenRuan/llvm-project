#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

uint64_t external_value;

uint64_t external_helper(uint64_t x) { return x + 1; }

uint64_t (*external_function_pointer)(uint64_t) = external_helper;

extern uint64_t probe_local_constant(uint64_t x);
extern int32_t probe_switch(int32_t x);

static inline uint64_t read_counter(void) {
  uint64_t value;
  __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(value) : : "memory");
  return value;
}

int main(void) {
  const uint64_t iterations = UINT64_C(20000000);
  uint64_t sum = 0;

  for (unsigned round = 0; round != 7; ++round) {
    uint64_t begin = read_counter();
    for (uint64_t i = 0; i != iterations; ++i)
      sum += probe_local_constant(i);
    uint64_t middle = read_counter();
    for (uint64_t i = 0; i != iterations; ++i)
      sum += (uint32_t)probe_switch((int32_t)(i & 7));
    uint64_t end = read_counter();

    printf("round=%u table_ticks_per_call=%.4f switch_ticks_per_call=%.4f "
           "sum=%" PRIu64 "\n",
           round, (double)(middle - begin) / (double)iterations,
           (double)(end - middle) / (double)iterations, (uint64_t)sum);
  }
  return 0;
}
