// Ordinary cross-TU helpers retained by -fejit-cross-jit-helpers.

#include <stdint.h>

struct LocalHelperCfg {
  uint32_t mode;
  uint32_t bias;
  uint32_t runtimeCounter;
};

extern struct LocalHelperCfg g_localHelperCfg[];

uint32_t jit_local_helper_c(uint8_t cell) {
  return g_localHelperCfg[cell].mode * 10u + g_localHelperCfg[cell].bias;
}

uint32_t jit_local_helper_b(uint8_t cell) {
  return jit_local_helper_c(cell) + (uint32_t)cell + 7u;
}
