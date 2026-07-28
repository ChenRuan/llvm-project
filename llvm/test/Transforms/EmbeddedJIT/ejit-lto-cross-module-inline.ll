; RUN: opt -passes='cgscc(devirt<4>(inline,function-attrs)),function(sroa,early-cse,instcombine,simplifycfg),ejit-register-bitcode' -S %s | FileCheck %s
;
; Verify that under ThinLTO-like pipeline (simplification before PASS1),
; cross-module imported child functions are inlined into ejit_entry BEFORE
; bitcode extraction, and !ejit.may_const metadata is preserved.
;
; This simulates the post-ThinLTO-import state: child_func is
; available_externally (imported from another TU) and should be inlined
; into jit_main by the simplification pipeline, then PASS1 extracts
; bitcode containing the already-inlined code with intact metadata.

target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128-Fn32"

%struct.Cfg = type { i32, i32 }

@g_cfg = dso_local global %struct.Cfg zeroinitializer, !ejit.metadata !10

; CHECK: @__ejit_bitcode = internal constant [{{.*}} x i8]

; CHECK-LABEL: define {{.*}}@jit_main(
; CHECK-NOT:   call {{.*}}@child_func
; CHECK:       load {{.*}}!ejit.may_const
define dso_local i32 @jit_main() #0 !ejit.metadata !20 {
entry:
  %call = call i32 @child_func()
  %cmp = icmp eq i32 %call, 204
  br i1 %cmp, label %ret_jit, label %ret_aot
ret_jit:
  ret i32 100
ret_aot:
  ret i32 1
}

; child_func: imported from another TU (available_externally).
; Accesses g_cfg.mode — this should be inlined into jit_main.
define available_externally i32 @child_func() #1 {
entry:
  %field = getelementptr inbounds %struct.Cfg, ptr @g_cfg, i32 0, i32 0
  %val = load i32, ptr %field, !ejit.may_const !30
  %cmp = icmp eq i32 %val, 204
  br i1 %cmp, label %ret_cc, label %ret_zero
ret_cc:
  ret i32 204
ret_zero:
  ret i32 0
}

!0 = !{!"ejit_entry"}
!1 = !{!"ejit_period", !"static"}
!2 = !{!"ejit_may_const_field", i32 0}

!10 = !{!1, !2}
!20 = !{!0}
!30 = !{}

attributes #0 = { "ejit_entry" }
attributes #1 = { nounwind }
