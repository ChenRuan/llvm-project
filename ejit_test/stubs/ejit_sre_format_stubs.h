//===-- ejit_sre_format_stubs.h - SRE buffer-formatting stubs -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Declarations for the SRE buffer-formatting stubs. See
// ejit_sre_format_stubs.cpp for the rationale and usage.
//
//===----------------------------------------------------------------------===//

#ifndef EJIT_TEST_STUBS_EJIT_SRE_FORMAT_STUBS_H
#define EJIT_TEST_STUBS_EJIT_SRE_FORMAT_STUBS_H

#include <cstdarg>
#include <cstddef>

extern "C" {

/// Self-contained mini vsnprintf: formats into \p buf (never allocates, always
/// NUL-terminates when \p size > 0). Returns the number of characters that
/// would have been written had the buffer been large enough (like the C
/// standard vsnprintf), so callers such as LLVM's format_object_base can size
/// their scratch buffer correctly.
int ejit_mini_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

/// snprintf built on ejit_mini_vsnprintf.
int ejit_mini_snprintf(char *buf, size_t size, const char *fmt, ...);

} // extern "C"

#endif // EJIT_TEST_STUBS_EJIT_SRE_FORMAT_STUBS_H
