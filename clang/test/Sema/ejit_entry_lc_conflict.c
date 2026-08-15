// RUN: %clang_cc1 -fsyntax-only -verify %s

// C mode: the ejit_entry / ejit_period_lc conflict check applies on the C
// path of ActOnFunctionDeclarator / CheckFunctionDeclaration too, not only
// the C++ one (see ejit_attr_missing_on_definition.cpp).

// --- Error: both attributes on one declarator ---

__attribute__((ejit_entry, ejit_period_lc("static")))
int f(__attribute__((ejit_period_arr_ind("static"))) int idx) { return idx; }
// expected-error@-2 {{'ejit_entry' and 'ejit_period_lc' cannot be combined on function 'f'}}
// expected-note@-3 {{conflicting attribute is here}}

// --- Error: assembled across declarations (entry on the prototype, lc on
//     the redeclaration) ---

__attribute__((ejit_entry))
int g(__attribute__((ejit_period_arr_ind("static"))) int idx);
// expected-note@-2 {{conflicting attribute is here}}
__attribute__((ejit_period_lc("static")))
int g(__attribute__((ejit_period_arr_ind("static"))) int idx);
// expected-error@-2 {{'ejit_entry' and 'ejit_period_lc' cannot be combined on function 'g'}}

// --- OK: the ejit_period_arr_ind may live on an earlier declaration's
//     parameter; it is propagated by the merge before the no-index check ---

void h(__attribute__((ejit_period_arr_ind("cell"))) int idx);
__attribute__((ejit_period_lc("cell"))) void h(int idx) {}

// --- Error: NO declaration provides a matching ejit_period_arr_ind ---

void missing_idx(__attribute__((ejit_period_arr_ind("other"))) int idx);
__attribute__((ejit_period_lc("cell"))) void missing_idx(int idx);
// expected-error@-1 {{ejit_period_lc(cell) requires a corresponding ejit_period_arr_ind(cell) parameter}}
