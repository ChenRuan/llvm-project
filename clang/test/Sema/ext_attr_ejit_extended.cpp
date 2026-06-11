// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fsyntax-only -verify=expected %s
//
// EmbeddedJIT attribute semantics — extended coverage beyond ext_attr_ejit.cpp.
// Focuses on cases with stable, EJIT-specific diagnostic text: pointer-type
// ejit_period_arr (v1.6), array-size limit, pointer-to-non-struct, and
// ejit_entry recursion. A fixed -triple keeps struct layout deterministic.

struct CellConfig {
  __attribute__((ejit_may_const)) int cellType;
  int xx;
};

// === Correct: pointer-to-struct ejit_period_arr (v1.6) — no diagnostic ===
__attribute__((ejit_period_arr("cell"))) struct CellConfig *g_cellPtr;

// === Correct: array exactly at the limit (100) — no diagnostic ===
__attribute__((ejit_period_arr("cell"))) struct CellConfig g_cells100[100];

// === Error: array larger than the limit (>100) ===
__attribute__((ejit_period_arr("cell"))) struct CellConfig g_cellsBig[101];
// expected-error@-1 {{ejit_period_arr array 'g_cellsBig' has size 101, which exceeds the maximum of 100}}

// === Error: ejit_period_arr on pointer-to-non-struct ===
__attribute__((ejit_period_arr("cell"))) int *g_intPtr;
// expected-error@-1 {{ejit_period_arr attribute requires an array type; 'g_intPtr' is not an array}}

// === Error: ejit_period_arr on plain scalar ===
__attribute__((ejit_period_arr("cell"))) int g_scalar;
// expected-error@-1 {{ejit_period_arr attribute requires an array type; 'g_scalar' is not an array}}

// NOTE: ejit_entry self-recursion is NOT diagnosed for an ordinary function
// definition: handleEjitEntryAttr() only runs its RecursiveCallVisitor when
// FD->hasBody(), but the attribute is processed before the body is parsed, so
// the check does not fire here. This is a known limitation (recorded in the
// SPEC audit), so this file does not assert a recursion diagnostic.
int helper(int x);
__attribute__((ejit_entry))
int ok_entry(__attribute__((ejit_period_arr_ind("cell"))) int ci) {
  return helper(ci);
}
