// RUN: %clang_cc1 -fsyntax-only -verify=off %s
// RUN: %clang_cc1 -fsyntax-only -Wembedded-jit-undeclared-period-dep -verify=on %s

// EmbeddedJIT: verify the -Wembedded-jit-undeclared-period-dep toggle for the
// "ejit_entry references ejit_period_arr without declaring the dependency"
// warning. Off by default (DefaultIgnore); enabled explicitly here.

struct S {
  int x;
};

__attribute__((ejit_period_arr("cell"))) struct S g_cells[16];
// on-note@-1 2 {{ejit_period_arr 'cell' defined here}}
__attribute__((ejit_period_arr("board"))) struct S g_board[8];
// on-note@-1 {{ejit_period_arr 'board' defined here}}
__attribute__((ejit_period_arr("cell"))) struct S g_cells_dup[4];

// Declared dependency -> no warning.
__attribute__((ejit_entry))
void ok_entry(__attribute__((ejit_period_arr_ind("cell"))) int idx) {
  g_cells[idx].x = idx;  // no warning: 'cell' declared via ejit_period_arr_ind
}

// Undeclared dependency -> warn once, at the first reference.
__attribute__((ejit_entry))
void bad_entry(int idx) {
  g_cells[idx].x = 1;  // on-warning {{function 'bad_entry' references ejit_period_arr 'cell' but does not declare a dependency on it via ejit_period_arr_ind}}
  g_cells[idx].x = 2;  // no warning: same period already reported
}

// Mixed: one declared, one not.
__attribute__((ejit_entry))
void mixed_entry(__attribute__((ejit_period_arr_ind("cell"))) int idx) {
  g_cells[idx].x = 1;  // no warning: declared
  g_board[idx].x = 2;  // on-warning {{function 'mixed_entry' references ejit_period_arr 'board' but does not declare a dependency on it via ejit_period_arr_ind}}
}

// Dedupe is by period name: a second global with the same period name does
// not warn again.
__attribute__((ejit_entry))
void dup_globals(int idx) {
  g_cells[idx].x = 1;      // on-warning {{function 'dup_globals' references ejit_period_arr 'cell' but does not declare a dependency on it via ejit_period_arr_ind}}
  g_cells_dup[idx].x = 1;  // no warning: same period name already reported
}

// sizeof/alignof operands are unevaluated -> no runtime dependency.
__attribute__((ejit_entry))
void sizeof_entry(void) {
  (void)sizeof(g_cells);  // no warning: unevaluated context
  (void)__alignof__(g_cells);  // no warning: unevaluated context
}

// Other unevaluated contexts (decltype, noexcept operands) are not runtime
// dependencies.
__attribute__((ejit_entry))
void unevaluated_entry(void) {
  decltype(g_cells) copy;          // no warning: decltype is unevaluated
  bool b = noexcept(g_cells[0].x); // no warning: noexcept is unevaluated
  (void)copy[0].x; (void)b;
}

// Non-entry functions are not checked.
void plain_fn(int idx) {
  g_cells[idx].x = 1;  // no warning: not ejit_entry
}

// off-no-diagnostics
