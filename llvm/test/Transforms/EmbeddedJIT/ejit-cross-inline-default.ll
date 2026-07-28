; RUN: opt -passes=ejit-register-bitcode -S %s | FileCheck %s
;
; Default mode (no -ejit-cross-inline): should still generate @__ejit_bitcode
; with the ejit_entry closure, not the full module.

; CHECK: @__ejit_bitcode = internal constant [{{.*}} x i8]
; CHECK: define internal void @ejit_auto_register()
; CHECK: call void @ejit_register_bitcode
; CHECK-NOT: @__ejit_cross_module

define void @helper() {
  ret void
}

define void @unreachable() {
  ret void
}

define void @ejit_entry1() !ejit.metadata !0 {
  call void @helper()
  ret void
}

!0 = distinct !{!{!"ejit_entry"}}
