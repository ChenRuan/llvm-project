; RUN: opt -passes=ejit-register-bitcode -ejit-cross-inline -S %s | FileCheck %s
;
; Verify cross-inline mode handles edge cases:
; 1. Normal metadata preserved
; 2. Empty or malformed metadata nodes don't crash

target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128-Fn32"

%struct.Safe = type { i32, i32 }

@g_safe = dso_local global %struct.Safe zeroinitializer, !ejit.metadata !10

; CHECK: @__ejit_cross_module = internal constant [{{.*}} x i8] {{.*}}, section ".ejit_cross"

define void @test_normal() !ejit.metadata !20 {
entry:
  %f = getelementptr %struct.Safe, ptr @g_safe, i32 0, i32 0
  %v = load i32, ptr %f, !ejit.may_const !30
  ret void
}

; Well-formed metadata
!10 = !{!{!"ejit_period", !"static"}, !{!"ejit_may_const_field", i32 0}}

; ejit_entry tag
!20 = !{!{!"ejit_entry"}}

; per-load may_const
!30 = !{}
