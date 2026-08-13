// RUN: %clang_cc1 -emit-llvm -o - %s | FileCheck %s
// EmbeddedJIT: a function declared with ejit_entry / ejit_period_lc in a
// prototype but defined without the attribute is not JIT-specialized: no
// !ejit.metadata is emitted and the definition stays a plain AOT function.
// The warning is covered in clang/test/Sema/ejit_attr_missing_on_definition.cpp.

// Header prototypes.
__attribute__((ejit_entry)) int entry_fn(void);
__attribute__((ejit_period_lc("static")))
void lc_fn(__attribute__((ejit_period_arr_ind("static"))) int idx);

// Definitions without the attributes: no metadata, no JIT wrapper.
int entry_fn(void) { return 1; }
// CHECK-LABEL: define {{.*}} @entry_fn
// CHECK-NOT: !ejit.metadata
// CHECK: ret i32 1

void lc_fn(int idx) {}
// CHECK-LABEL: define {{.*}} @lc_fn
// CHECK-NOT: !ejit.metadata
// CHECK: ret void

// Control: attribute on the definition still enables JIT specialization.
__attribute__((ejit_entry)) int jit_fn(void) { return 2; }
// CHECK-LABEL: define {{.*}} @jit_fn
// CHECK: !ejit.metadata
