; RUN: opt -passes=ejit-register-bitcode -ejit-cross-inline -S %s | FileCheck %s
;
; Verify cross-inline mode: PASS1 embeds the FULL module IR in .ejit_cross
; section instead of extracting ejit_entry closure into @__ejit_bitcode.

; CHECK: @__ejit_cross_module = internal constant [{{.*}} x i8] {{.*}}, section ".ejit_cross"
; CHECK-NOT: @__ejit_bitcode
; CHECK-NOT: ejit_auto_register

define void @helper_a() {
  ret void
}

define void @helper_b() {
  call void @helper_a()
  ret void
}

define void @ejit_entry1() !ejit.metadata !0 {
  call void @helper_b()
  ret void
}

define void @ejit_entry2() !ejit.metadata !1 {
  call void @helper_a()
  ret void
}

!0 = distinct !{!{!"ejit_entry"}}
!1 = distinct !{!{!"ejit_entry"}}
