//===-- ejit_sre_format_stubs.cpp - SRE buffer-formatting stubs -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Why this file exists
// --------------------
// EmbeddedJIT's on-demand ASM diagnostic dump (ejit_dump_func /
// ejit_print_dumped) drives the LLVM code emitter down the human-readable
// assembly path:
//
//     TargetMachine::addPassesToEmitFile(..., AssemblyFile)  ->  PM.run(M)
//       -> AsmPrinter::emitFunctionBody
//       -> AArch64AsmPrinter::emitInstruction
//       -> MCAsmStreamer::emitInstruction
//       -> AArch64InstPrinter::printInst
//       -> raw_ostream::operator<<(llvm::format_object_base const&)
//
// The final step writes formatted text into an in-memory
// raw_svector_ostream(AsmBuf). llvm::format_object_base implements that by
// calling the C library snprintf()/vsnprintf() to render each field into a
// scratch buffer. On SRE / bare-metal firmware those two functions are often
// missing or non-functional, so the call crashes (the observed SRE stack ends
// in raw_ostream::operator<<(format_object_base const&)).
//
// SRE_printf() alone cannot fix this: it prints to the device console and does
// NOT populate AsmBuf. The missing capability is *buffer* formatting
// (snprintf/vsnprintf-like), not console printing. This file provides exactly
// that, isolated from the rest of EmbeddedJIT.
//
// How to use it on SRE / in the lipo flow
// ---------------------------------------
// This is a standalone optional object. It is NOT compiled into libLLVMEJIT.a
// (so host builds keep using the real libc snprintf). When the SRE final image
// is linked with ASM dump enabled, add this object to the final link / lipo
// merge command so the emitter's snprintf/vsnprintf calls resolve here, e.g.:
//
//     clang++ --target=aarch64_be-... -std=c++17 -fno-exceptions -fno-rtti \
//         -c ejit_test/stubs/ejit_sre_format_stubs.cpp -o ejit_sre_format_stubs.o
//     <final-sre-link> ... ejit.o ejit_sre_format_stubs.o ...
//
// The libc-named definitions (::snprintf / ::vsnprintf) can be suppressed with
// -DEJIT_SRE_FORMAT_STUBS_NO_LIBC_NAMES (used by the host unit test so it does
// not clash with the platform libc). The core ejit_mini_* entry points are
// always available.
//
// Deliberate limitations
// ----------------------
// This mini formatter targets what LLVM's AsmPrinter / AArch64InstPrinter need
// for integer/pointer/text fields. It intentionally does NOT implement full
// floating-point formatting; %f/%F/%e/%g consume their double argument (to keep
// va_list alignment correct) and emit a coarse fixed-point approximation. It is
// a diagnostic aid, not a conformant libc.
//
// Freestanding-friendly: no heap, no std::string / iostream / locale / mutex /
// condition_variable / future, no exceptions / RTTI, no raw_ostream / errs() /
// outs().
//
//===----------------------------------------------------------------------===//

#include "ejit_sre_format_stubs.h"

#include <cstdarg>
#include <cstddef>
#include <cstdint>

namespace {

// Sink that respects the caller-provided capacity while counting the full
// (would-be) length, matching C99 vsnprintf return semantics.
struct OutBuf {
  char *buf;
  size_t size;   // total capacity of buf (including NUL slot)
  size_t pos;    // bytes written into buf so far (excluding NUL)
  size_t total;  // bytes that would be written if buf were unbounded

  void put(char c) {
    if (buf && size > 0 && pos + 1 < size)
      buf[pos++] = c;
    ++total;
  }
};

size_t mini_strlen(const char *s) {
  size_t n = 0;
  while (s[n])
    ++n;
  return n;
}

enum Length { LEN_NONE, LEN_L, LEN_LL, LEN_Z };

struct Spec {
  bool leftAlign = false;
  bool zeroPad = false;
  bool altForm = false; // '#'
  bool plusSign = false;
  bool spaceSign = false;
  int width = 0;
  int prec = -1; // negative => unspecified
  Length length = LEN_NONE;
};

void pad(OutBuf &ob, char c, int n) {
  for (int i = 0; i < n; ++i)
    ob.put(c);
}

// Emit an unsigned magnitude in the given base, applying an optional textual
// prefix (sign, "0x", ...) and width/zero-padding rules.
void emit_unsigned(OutBuf &ob, unsigned long long val, unsigned base,
                   bool upper, const Spec &s, const char *prefix) {
  char tmp[64];
  int n = 0;
  const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  if (val == 0) {
    tmp[n++] = '0';
  } else {
    while (val > 0) {
      tmp[n++] = digits[val % base];
      val /= base;
    }
  }
  // Precision on integers = minimum number of digits (zero-fill).
  int precZeros = 0;
  if (s.prec >= 0 && s.prec > n)
    precZeros = s.prec - n;

  int prefixLen = prefix ? (int)mini_strlen(prefix) : 0;
  int bodyLen = prefixLen + precZeros + n;
  int spaces = s.width > bodyLen ? s.width - bodyLen : 0;

  // When zero-padding (and no explicit precision) the zeros go between the
  // prefix and the digits; otherwise pad with spaces on the correct side.
  bool zeroFill = s.zeroPad && !s.leftAlign && s.prec < 0;

  if (!s.leftAlign && !zeroFill)
    pad(ob, ' ', spaces);
  for (int i = 0; i < prefixLen; ++i)
    ob.put(prefix[i]);
  if (zeroFill)
    pad(ob, '0', spaces);
  pad(ob, '0', precZeros);
  for (int i = n - 1; i >= 0; --i)
    ob.put(tmp[i]);
  if (s.leftAlign)
    pad(ob, ' ', spaces);
}

void emit_signed(OutBuf &ob, long long val, const Spec &s) {
  bool neg = val < 0;
  // Two's-complement safe magnitude (handles LLONG_MIN without UB).
  unsigned long long mag =
      neg ? (~static_cast<unsigned long long>(val) + 1ULL)
          : static_cast<unsigned long long>(val);
  const char *prefix = neg ? "-" : (s.plusSign ? "+" : (s.spaceSign ? " " : ""));
  emit_unsigned(ob, mag, 10, false, s, prefix);
}

// Coarse fixed-point rendering of a double. Diagnostic-grade only.
void emit_double(OutBuf &ob, double val, const Spec &s) {
  // Guard the integer cast below: (unsigned long long)val is UB for NaN,
  // Inf, or values >= 2^64. The ASM diagnostic path rarely formats doubles,
  // but it is reachable, so fall back to a textual marker instead of UB.
  if (val != val) { // NaN
    for (const char *p = "nan"; *p; ++p)
      ob.put(*p);
    return;
  }
  if (val == val && (val > 1.8446744073709552e+19 || val < -1.8446744073709552e+19)) {
    // |val| >= ~2^64 (ULLONG_MAX): print "inf"-style marker to avoid UB.
    const char *p = val < 0 ? "-inf" : "inf";
    for (; *p; ++p)
      ob.put(*p);
    return;
  }
  const char *sign = "";
  if (val < 0) {
    sign = "-";
    val = -val;
  } else if (s.plusSign) {
    sign = "+";
  } else if (s.spaceSign) {
    sign = " ";
  }
  int prec = s.prec >= 0 ? s.prec : 6;
  unsigned long long intPart = (unsigned long long)val;
  double frac = val - (double)intPart;

  for (const char *p = sign; *p; ++p)
    ob.put(*p);
  // Integer part.
  {
    char tmp[32];
    int n = 0;
    if (intPart == 0) {
      tmp[n++] = '0';
    } else {
      while (intPart > 0) {
        tmp[n++] = (char)('0' + (int)(intPart % 10ULL));
        intPart /= 10ULL;
      }
    }
    for (int i = n - 1; i >= 0; --i)
      ob.put(tmp[i]);
  }
  if (prec > 0) {
    ob.put('.');
    for (int i = 0; i < prec; ++i) {
      frac *= 10.0;
      int d = (int)frac;
      if (d < 0)
        d = 0;
      if (d > 9)
        d = 9;
      ob.put((char)('0' + d));
      frac -= (double)d;
    }
  }
}

int parse_int(const char *&f) {
  int v = 0;
  while (*f >= '0' && *f <= '9') {
    v = v * 10 + (*f - '0');
    ++f;
  }
  return v;
}

} // namespace

extern "C" int ejit_mini_vsnprintf(char *buf, size_t size, const char *fmt,
                                   va_list ap) {
  OutBuf ob{buf, size, 0, 0};

  for (const char *f = fmt; *f; ++f) {
    if (*f != '%') {
      ob.put(*f);
      continue;
    }
    ++f; // consume '%'
    if (*f == '%') {
      ob.put('%');
      continue;
    }

    Spec s;
    // Flags.
    for (;; ++f) {
      if (*f == '-')
        s.leftAlign = true;
      else if (*f == '0')
        s.zeroPad = true;
      else if (*f == '#')
        s.altForm = true;
      else if (*f == '+')
        s.plusSign = true;
      else if (*f == ' ')
        s.spaceSign = true;
      else
        break;
    }
    // Width (digits or '*').
    if (*f == '*') {
      s.width = va_arg(ap, int);
      if (s.width < 0) {
        s.leftAlign = true;
        s.width = -s.width;
      }
      ++f;
    } else {
      s.width = parse_int(f);
    }
    // Precision.
    if (*f == '.') {
      ++f;
      if (*f == '*') {
        s.prec = va_arg(ap, int);
        ++f;
      } else {
        s.prec = parse_int(f);
      }
      if (s.prec < 0)
        s.prec = -1;
    }
    // Length modifiers.
    if (*f == 'l') {
      ++f;
      if (*f == 'l') {
        s.length = LEN_LL;
        ++f;
      } else {
        s.length = LEN_L;
      }
    } else if (*f == 'z') {
      s.length = LEN_Z;
      ++f;
    } else if (*f == 'h') {
      ++f;
      if (*f == 'h')
        ++f; // 'hh' -> promoted to int anyway
    } else if (*f == 'j' || *f == 't') {
      s.length = LEN_LL;
      ++f;
    }

    char conv = *f;
    switch (conv) {
    case 'd':
    case 'i': {
      long long v;
      if (s.length == LEN_LL)
        v = va_arg(ap, long long);
      else if (s.length == LEN_L)
        v = (long long)va_arg(ap, long);
      else if (s.length == LEN_Z)
        v = (long long)va_arg(ap, long);
      else
        v = (long long)va_arg(ap, int);
      emit_signed(ob, v, s);
      break;
    }
    case 'u':
    case 'x':
    case 'X':
    case 'o': {
      unsigned long long v;
      if (s.length == LEN_LL)
        v = va_arg(ap, unsigned long long);
      else if (s.length == LEN_L)
        v = va_arg(ap, unsigned long);
      else if (s.length == LEN_Z)
        v = (unsigned long long)va_arg(ap, size_t);
      else
        v = (unsigned long long)va_arg(ap, unsigned int);
      unsigned base = (conv == 'o') ? 8 : (conv == 'u') ? 10 : 16;
      bool upper = (conv == 'X');
      const char *prefix = "";
      if (s.altForm && v != 0) {
        if (conv == 'x')
          prefix = "0x";
        else if (conv == 'X')
          prefix = "0X";
        else if (conv == 'o')
          prefix = "0";
      }
      emit_unsigned(ob, v, base, upper, s, prefix);
      break;
    }
    case 'p': {
      void *ptr = va_arg(ap, void *);
      uintptr_t v = (uintptr_t)ptr;
      Spec ps = s;
      emit_unsigned(ob, (unsigned long long)v, 16, false, ps, "0x");
      break;
    }
    case 'c': {
      char c = (char)va_arg(ap, int);
      int spaces = s.width > 1 ? s.width - 1 : 0;
      if (!s.leftAlign)
        pad(ob, ' ', spaces);
      ob.put(c);
      if (s.leftAlign)
        pad(ob, ' ', spaces);
      break;
    }
    case 's': {
      const char *str = va_arg(ap, const char *);
      if (!str)
        str = "(null)";
      int len = 0;
      if (s.prec >= 0) {
        while (len < s.prec && str[len])
          ++len;
      } else {
        len = (int)mini_strlen(str);
      }
      int spaces = s.width > len ? s.width - len : 0;
      if (!s.leftAlign)
        pad(ob, ' ', spaces);
      for (int i = 0; i < len; ++i)
        ob.put(str[i]);
      if (s.leftAlign)
        pad(ob, ' ', spaces);
      break;
    }
    case 'f':
    case 'F':
    case 'e':
    case 'E':
    case 'g':
    case 'G': {
      double d = va_arg(ap, double);
      emit_double(ob, d, s);
      break;
    }
    case '\0':
      // Trailing '%' at end of string: emit literally and stop.
      ob.put('%');
      --f; // let the loop's ++f land on the NUL and terminate
      break;
    default:
      // Unknown specifier: emit verbatim so nothing is silently dropped.
      ob.put('%');
      ob.put(conv);
      break;
    }
  }

  if (ob.buf && ob.size > 0) {
    size_t term = ob.pos < ob.size ? ob.pos : ob.size - 1;
    ob.buf[term] = '\0';
  }
  return (int)ob.total;
}

extern "C" int ejit_mini_snprintf(char *buf, size_t size, const char *fmt,
                                  ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = ejit_mini_vsnprintf(buf, size, fmt, ap);
  va_end(ap);
  return r;
}

//===----------------------------------------------------------------------===//
// libc-named entry points that LLVM's format_object_base actually calls.
// Suppressed under EJIT_SRE_FORMAT_STUBS_NO_LIBC_NAMES (host unit test) to
// avoid clashing with the platform libc.
//===----------------------------------------------------------------------===//
#ifndef EJIT_SRE_FORMAT_STUBS_NO_LIBC_NAMES

extern "C" int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
  return ejit_mini_vsnprintf(buf, size, fmt, ap);
}

extern "C" int snprintf(char *buf, size_t size, const char *fmt, ...) {
  // Route through vsnprintf so a single mini implementation backs both entry
  // points. Variadic args cannot be forwarded to another variadic function.
  va_list ap;
  va_start(ap, fmt);
  int r = vsnprintf(buf, size, fmt, ap);
  va_end(ap);
  return r;
}

#endif // EJIT_SRE_FORMAT_STUBS_NO_LIBC_NAMES
