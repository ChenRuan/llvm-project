// RUN: %clang_cc1 -fsyntax-only -verify %s

typedef unsigned long long u64;
typedef enum Mode { ModeA, ModeB } Mode;

__attribute__((ejit_const_after_init)) _Bool g_bool;
__attribute__((ejit_const_after_init)) char g_char;
__attribute__((ejit_const_after_init)) unsigned short g_short;
__attribute__((ejit_const_after_init)) int g_int;
__attribute__((ejit_const_after_init)) u64 g_u64;
__attribute__((ejit_const_after_init)) __int128 g_i128;
__attribute__((ejit_const_after_init)) Mode g_mode;
__attribute__((ejit_const_after_init)) float g_float;
__attribute__((ejit_const_after_init)) double g_double;
__attribute__((ejit_const_after_init)) long double g_long_double;
extern __attribute__((ejit_const_after_init)) unsigned g_extern;
extern unsigned g_extern;

static __attribute__((ejit_const_after_init)) unsigned g_static;
// expected-error@-1 {{ejit_const_after_init variable 'g_static' must have external linkage}}
static int g_static_then_extern;
extern __attribute__((ejit_const_after_init)) int g_static_then_extern;
// expected-error@-1 {{ejit_const_after_init variable 'g_static_then_extern' must have external linkage}}
// expected-note@-3 {{previous declaration is here}}

void local_rejected(void) {
  static __attribute__((ejit_const_after_init)) int local;
  // expected-warning@-1 {{'ejit_const_after_init' attribute only applies to file-scope variable declarations}}
}

__attribute__((ejit_const_after_init)) int *g_ptr;
// expected-error@-1 {{ejit_const_after_init variable 'g_ptr' must have integral, enumeration, or real floating type; 'int *' is not supported}}
__attribute__((ejit_const_after_init)) int g_array[2];
// expected-error@-1 {{ejit_const_after_init variable 'g_array' must have integral, enumeration, or real floating type; 'int[2]' is not supported}}
struct S { int x; };
__attribute__((ejit_const_after_init)) struct S g_record;
// expected-error@-1 {{ejit_const_after_init variable 'g_record' must have integral, enumeration, or real floating type; 'struct S' is not supported}}
__attribute__((ejit_const_after_init)) _Complex float g_complex;
// expected-error@-1 {{ejit_const_after_init variable 'g_complex' must have integral, enumeration, or real floating type; '_Complex float' is not supported}}
__attribute__((ejit_const_after_init)) volatile int g_volatile;
// expected-error@-1 {{ejit_const_after_init variable 'g_volatile' cannot be volatile, atomic, or thread-local}}
__attribute__((ejit_const_after_init)) _Atomic(int) g_atomic;
// expected-error@-1 {{ejit_const_after_init variable 'g_atomic' cannot be volatile, atomic, or thread-local}}
__thread __attribute__((ejit_const_after_init)) int g_tls;
// expected-error@-1 {{ejit_const_after_init variable 'g_tls' cannot be volatile, atomic, or thread-local}}

__attribute__((ejit_const_after_init, ejit_period("static"))) int g_both_a;
// expected-error@-1 {{'ejit_period' and 'ejit_const_after_init' attributes are not compatible}}
// expected-note@-2 {{conflicting attribute is here}}
__attribute__((ejit_period("static"), ejit_const_after_init)) int g_both_b;
// expected-error@-1 {{'ejit_const_after_init' and 'ejit_period' attributes are not compatible}}
// expected-note@-2 {{conflicting attribute is here}}

extern int g_period_then_const __attribute__((ejit_period("cell")));
// expected-note@-1 {{conflicting attribute is here}}
extern int g_period_then_const __attribute__((ejit_const_after_init));
// expected-error@-1 {{'ejit_const_after_init' and 'ejit_period' attributes are not compatible}}

extern int g_const_then_period __attribute__((ejit_const_after_init));
// expected-note@-1 {{conflicting attribute is here}}
extern int g_const_then_period __attribute__((ejit_period("cell")));
// expected-error@-1 {{'ejit_period' and 'ejit_const_after_init' attributes are not compatible}}

extern struct S *g_period_arr_then_const
    __attribute__((ejit_period_arr("cell")));
extern struct S *g_period_arr_then_const
    __attribute__((ejit_const_after_init));
// expected-error@-1 {{must have integral, enumeration, or real floating type}}

extern __attribute__((ejit_const_after_init)) struct S *g_const_then_period_arr;
// expected-error@-1 {{must have integral, enumeration, or real floating type}}
extern struct S *g_const_then_period_arr
    __attribute__((ejit_period_arr("cell")));
