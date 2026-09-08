// RUN: %clang_cc1 -emit-llvm -o - %s | FileCheck %s
// RUN: %clang_cc1 -O2 -emit-llvm -o - %s | FileCheck %s --check-prefix=PIPE
// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang_cc1 -O2 -emit-llvm -mllvm -ejit-dump-bitcode-dir=%t -o %t/aot.ll %s
// RUN: FileCheck %s --check-prefix=AOT < %t/aot.ll
// RUN: opt -S %t/*.bc -o - | FileCheck %s --check-prefix=EXTRACTED
// RUN: %clang_cc1 -O2 -emit-llvm -o - %S/Inputs/ejit_const_after_init_external_def.c | FileCheck %s --check-prefix=XTU-DEF

typedef enum Mode { ModeA = -1, ModeB = 7 } Mode;

// CHECK-DAG: @g_value = global i32 0, align 4, !ejit.metadata ![[META:[0-9]+]]
__attribute__((ejit_const_after_init)) unsigned g_value;
// CHECK-DAG: @g_mode = global i32 0, align 4, !ejit.metadata ![[MODE_META:[0-9]+]]
__attribute__((ejit_const_after_init)) Mode g_mode;
// CHECK-DAG: @g_wide = global i128 0, align 16, !ejit.metadata ![[WIDE_META:[0-9]+]]
__attribute__((ejit_const_after_init)) __int128 g_wide;
// CHECK-DAG: @g_double = global double 0.000000e+00, align 8, !ejit.metadata ![[DOUBLE_META:[0-9]+]]
__attribute__((ejit_const_after_init)) double g_double;
// CHECK-DAG: @g_plain = global i32 0
unsigned g_plain;
// CHECK-DAG: @g_external = external global i32, align 4, !ejit.metadata ![[EXTERNAL_META:[0-9]+]]
extern __attribute__((ejit_const_after_init)) unsigned g_external;

// XTU-DEF: @g_external = global i32 0, {{.*}}!ejit.metadata
// XTU-DEF-LABEL: define{{.*}} void @set_external(i32 noundef %value)
// XTU-DEF: store i32 {{.*}}, ptr @g_external, align 4
// XTU-DEF: call void @ejit_register_static_var({{.*}}@g_external)

// CHECK-LABEL: define{{.*}} i32 @read_value()
// CHECK: load i32, ptr @g_value, align 4, !ejit.may_const ![[LOAD:[0-9]+]]
unsigned read_value(void) { return g_value; }

// CHECK-LABEL: define{{.*}} i32 @read_plain()
// CHECK: load i32, ptr @g_plain, align 4{{$}}
unsigned read_plain(void) { return g_plain; }

// PIPE: @.ejit.registry.period = {{.*}} section ".ejit_period"
// PIPE: define{{.*}} i32 @jit_read_value()
// PIPE: load i32, ptr @g_value, align 4, {{.*}}!ejit.may_const
// PIPE: call void @ejit_register_static_var({{.*}}@g_value)
// PIPE: call void @ejit_register_static_var({{.*}}@g_external)
__attribute__((ejit_entry))
unsigned jit_read_value(void) {
  return (g_value == 17 ? 123 : 456) + g_plain;
}
__attribute__((ejit_entry)) unsigned jit_read_external(void) {
  return g_external;
}

// The attribute does not make the AOT global constant or remove writes.
// CHECK-LABEL: define{{.*}} void @application_init(i32 noundef %v)
// CHECK: [[VALUE:%.*]] = load i32, ptr %v.addr, align 4
// CHECK: store i32 [[VALUE]], ptr @g_value, align 4
void application_init(unsigned v) { g_value = v; }

// AOT-LABEL: define{{.*}} void @application_init(i32 noundef %v)
// AOT: store i32 {{.*}}, ptr @g_value, align 4

// EXTRACTED: @g_value = external global i32
// EXTRACTED-LABEL: define{{.*}} i32 @jit_read_value()
// EXTRACTED: load i32, ptr @g_value, align 4, {{.*}}!ejit.may_const
// EXTRACTED-NOT: load i32, ptr @g_plain{{.*}}!ejit.may_const
// EXTRACTED: load i32, ptr @g_plain, align 4, !tbaa

// CHECK-DAG: ![[META]] = distinct !{![[MARKER:[0-9]+]], ![[STATIC:[0-9]+]]}
// CHECK-DAG: ![[MODE_META]] = distinct !{![[MARKER]], ![[STATIC]]}
// CHECK-DAG: ![[WIDE_META]] = distinct !{![[MARKER]], ![[STATIC]]}
// CHECK-DAG: ![[DOUBLE_META]] = distinct !{![[MARKER]], ![[STATIC]]}
// CHECK-DAG: ![[EXTERNAL_META]] = distinct !{![[MARKER]], ![[STATIC]]}
// CHECK-DAG: ![[MARKER]] = !{!"ejit_const_after_init"}
// CHECK-DAG: ![[STATIC]] = !{!"ejit_period", !"static"}
// CHECK-DAG: ![[LOAD]] = !{}
