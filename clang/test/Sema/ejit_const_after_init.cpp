// RUN: %clang_cc1 -fsyntax-only -verify %s

namespace {
__attribute__((ejit_const_after_init)) int value;
// expected-error@-1 {{ejit_const_after_init variable 'value' must have external linkage}}
}

static __attribute__((ejit_const_after_init)) int first_static;
// expected-error@-1 {{ejit_const_after_init variable 'first_static' must have external linkage}}

static int declared_static_first;
extern __attribute__((ejit_const_after_init)) int declared_static_first;
// expected-error@-1 {{ejit_const_after_init variable 'declared_static_first' must have external linkage}}
// expected-note@-3 {{previous declaration is here}}

extern __attribute__((ejit_const_after_init)) int shared_value;
extern int shared_value;
