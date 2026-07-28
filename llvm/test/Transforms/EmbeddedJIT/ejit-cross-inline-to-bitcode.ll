; RUN: opt -passes=ejit-register-bitcode -ejit-cross-inline -S %s | FileCheck --check-prefix=STAGE1 %s
; RUN: opt -passes=ejit-register-bitcode -S %s | FileCheck --check-prefix=DEFAULT %s
;
; End-to-end test: Stage 1 (cross-inline) vs default mode.
;
; Stage 1: embeds full module in .ejit_cross, no @__ejit_bitcode.
; Default: extracts closure into @__ejit_bitcode with ejit_auto_register.

target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128-Fn32"

%struct.Cfg = type { i32, i32 }

@g_cfg = dso_local global %struct.Cfg zeroinitializer, !ejit.metadata !10

; STAGE1: @__ejit_cross_module = internal constant [{{.*}} x i8] {{.*}}, section ".ejit_cross"
; STAGE1-NOT: @__ejit_bitcode

; DEFAULT: @__ejit_bitcode = internal constant [{{.*}} x i8]
; DEFAULT: call void @ejit_register_bitcode

define void @jit_entry() !ejit.metadata !20 {
entry:
  %f = getelementptr %struct.Cfg, ptr @g_cfg, i32 0, i32 0
  %v = load i32, ptr %f, !ejit.may_const !30
  %c = icmp eq i32 %v, 204
  br i1 %c, label %yes, label %no
yes:
  ret void
no:
  ret void
}

!10 = !{!{!"ejit_period", !"static"}, !{!"ejit_may_const_field", i32 0}}
!20 = !{!{!"ejit_entry"}}
!30 = !{}
