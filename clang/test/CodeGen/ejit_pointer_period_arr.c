// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -emit-llvm -o - %s | FileCheck %s
//
// EmbeddedJIT CodeGen for a pointer-type ejit_period_arr (v1.6 SPEC 2.2.2).
// A pointer-to-struct period array records size 0 in its metadata (the runtime
// reads the base via *(void**)&GV); its may_const field loads through the
// pointer indirection are still annotated.

struct CellConfig {
  __attribute__((ejit_may_const)) int cellType;
  int xx;
};

// CHECK-DAG: @g_cellPtr = {{.*}} !ejit.metadata ![[PTR_META:[0-9]+]]
__attribute__((ejit_period_arr("cell"))) struct CellConfig *g_cellPtr;

// CHECK-LABEL: define {{.*}}@read_ptr(
__attribute__((ejit_entry))
int read_ptr(__attribute__((ejit_period_arr_ind("cell"))) int ci) {
  // The base pointer is loaded from the global, then the struct field load
  // through the pointed-to element carries the may_const annotation.
  // CHECK: load ptr, ptr @g_cellPtr
  // CHECK: load i32, ptr {{.*}}, !ejit.may_const
  return g_cellPtr[ci].cellType;
}

// Pointer period array registers with size 0 (dynamic; user guarantees bounds).
// CHECK: call void @ejit_register_period_array(ptr {{[^,]+}}, ptr {{[^,]+}}, ptr @g_cellPtr, i64 0)

// Metadata: ejit_period_arr "cell" with size 0, plus the may_const field offset.
// CHECK-DAG: ![[PTR_META]] = distinct !{![[ARR:[0-9]+]], ![[MCF:[0-9]+]]}
// CHECK-DAG: ![[ARR]] = !{!"ejit_period_arr", !"cell", i32 0}
// CHECK-DAG: ![[MCF]] = !{!"ejit_may_const_field", i32 0}
