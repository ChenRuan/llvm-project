#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

enum { ElementCount = 4096 };

uint32_t ejit_probe_scalar(const uint16_t *, const uint16_t *, size_t);
uint32_t ejit_probe_vector(const uint16_t *, const uint16_t *, size_t);

static uint16_t lhs[ElementCount];
static uint16_t rhs[ElementCount];
static volatile uint32_t sink;

static inline uint64_t readCycles(void) {
#if defined(__aarch64__)
  uint64_t value;
  __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(value) :: "memory");
  return value;
#else
#error This benchmark requires native AArch64
#endif
}

static uint64_t measure(uint32_t (*fn)(const uint16_t *, const uint16_t *,
                                      size_t),
                        size_t count, unsigned rounds) {
  for (unsigned i = 0; i != 100; ++i)
    sink ^= fn(lhs, rhs, count);

  uint64_t begin = readCycles();
  for (unsigned i = 0; i != rounds; ++i)
    sink ^= fn(lhs, rhs, count);
  uint64_t end = readCycles();
  return end - begin;
}

int main(void) {
  for (unsigned i = 0; i != ElementCount; ++i) {
    lhs[i] = (uint16_t)((i * 17u + 3u) & 1023u);
    rhs[i] = (uint16_t)((i * 29u + 7u) & 1023u);
  }

  uint32_t scalarResult = ejit_probe_scalar(lhs, rhs, ElementCount);
  uint32_t vectorResult = ejit_probe_vector(lhs, rhs, ElementCount);
  if (scalarResult != vectorResult) {
    fprintf(stderr, "result mismatch: scalar=%u vector=%u\n", scalarResult,
            vectorResult);
    return 1;
  }

  const size_t counts[] = {4, 8, 16, 32, 64, 256, 1024, 4096};
  for (unsigned i = 0; i != sizeof(counts) / sizeof(counts[0]); ++i) {
    size_t count = counts[i];
    unsigned rounds = count <= 64 ? 1000000u : 200000u;
    if (count >= 1024)
      rounds = 20000u;
    uint64_t scalar = measure(ejit_probe_scalar, count, rounds);
    uint64_t vector = measure(ejit_probe_vector, count, rounds);
    printf("count=%4zu scalar=%8.3f vector=%8.3f ticks/call speedup=%5.2fx\n",
           count, (double)scalar / rounds, (double)vector / rounds,
           (double)scalar / vector);
  }
  printf("sink=%u\n", sink);
  return 0;
}
