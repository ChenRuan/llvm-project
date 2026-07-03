//===-- ejit_sre_format_stubs_test.cpp - mini vsnprintf unit test ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Host-side unit test for the bundled mini formatter. It exercises the
// ejit_mini_snprintf() core directly (rather than the libc-named ::snprintf
// wrapper) so it can link cleanly against the platform libc.
//
// Build/run:
//   clang++ -std=c++17 -fno-exceptions -fno-rtti \
//       -DEJIT_SRE_FORMAT_STUBS_NO_LIBC_NAMES \
//       ejit_test/stubs/ejit_sre_format_stubs.cpp \
//       ejit_test/stubs/ejit_sre_format_stubs_test.cpp \
//       -o /tmp/ejit_sre_format_stubs_test
//   /tmp/ejit_sre_format_stubs_test
//
//===----------------------------------------------------------------------===//

#include "ejit_sre_format_stubs.h"

#include <cstdio>
#include <cstring>

static int g_failures = 0;

static void check(const char *expected, const char *got, const char *what) {
  bool ok = std::strcmp(expected, got) == 0;
  std::printf("[%s] %-28s expected=\"%s\" got=\"%s\"\n", ok ? "PASS" : "FAIL",
              what, expected, got);
  if (!ok)
    ++g_failures;
}

int main() {
  char b[128];

  ejit_mini_snprintf(b, sizeof(b), "x=%08x", 0x12);
  check("x=00000012", b, "%08x");

  ejit_mini_snprintf(b, sizeof(b), "p=%p", (void *)0xdeadbeef);
  check("p=0xdeadbeef", b, "%p");

  ejit_mini_snprintf(b, sizeof(b), "s=%s c=%c d=%d u=%u ll=%016llx", "hi", 'Z',
                     -42, 42u, (unsigned long long)0x1234ABCDULL);
  check("s=hi c=Z d=-42 u=42 ll=000000001234abcd", b, "combined");

  ejit_mini_snprintf(b, sizeof(b), "%% %d %i", 7, -7);
  check("% 7 -7", b, "percent/d/i");

  ejit_mini_snprintf(b, sizeof(b), "%5d|%-5d|%05d", 42, 42, 42);
  check("   42|42   |00042", b, "width/left/zero");

  ejit_mini_snprintf(b, sizeof(b), "%x %X %#x", 255, 255, 255);
  check("ff FF 0xff", b, "hex forms");

  ejit_mini_snprintf(b, sizeof(b), "%lu %llu %zu", 1000UL, 1000000000000ULL,
                     (size_t)4096);
  check("1000 1000000000000 4096", b, "length modifiers");

  // Truncation: return value is the would-be length; buffer stays terminated.
  char small[4];
  int n = ejit_mini_snprintf(small, sizeof(small), "%s", "abcdef");
  bool trunc_ok = (n == 6) && (std::strcmp(small, "abc") == 0);
  std::printf("[%s] %-28s ret=%d buf=\"%s\"\n", trunc_ok ? "PASS" : "FAIL",
              "truncation", n, small);
  if (!trunc_ok)
    ++g_failures;

  // size==0 must not touch the buffer but still returns the would-be length.
  char untouched = 'Q';
  int n0 = ejit_mini_snprintf(&untouched, 0, "%d", 12345);
  bool zero_ok = (n0 == 5) && (untouched == 'Q');
  std::printf("[%s] %-28s ret=%d\n", zero_ok ? "PASS" : "FAIL", "size==0", n0);
  if (!zero_ok)
    ++g_failures;

  if (g_failures == 0) {
    std::printf("ALL PASS\n");
    return 0;
  }
  std::printf("%d FAILURE(S)\n", g_failures);
  return 1;
}
