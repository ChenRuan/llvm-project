; RUN: opt -passes=ejit-register-bitcode \
; RUN:     -ejit-report-mayconst \
; RUN:     -ejit-warn-no-specialization=false -ejit-warn-unused-dim=false \
; RUN:     -S %s 2>&1 | FileCheck %s

; Option A: an info-level (non-gating) report of per-ejit_entry ejit_may_const
; read counts over the specialization closure: K total, J inside loops.

; A single may_const read inside a self-loop -> 1 total, 1 in loops.
; CHECK: EJit info: ejit_entry function 'entry_loop': 1 ejit_may_const read (1 in loops)

; A single may_const read in straight-line code -> 1 total, 0 in loops.
; CHECK: EJit info: ejit_entry function 'entry_noloop': 1 ejit_may_const read (0 in loops)

; No may_const read -> 0 total, 0 in loops.
; CHECK: EJit info: ejit_entry function 'entry_plain': 0 ejit_may_const reads (0 in loops)

@cell_data = global [16 x i32] zeroinitializer, !ejit.metadata !10

define void @entry_loop(i32 %c) !ejit.metadata !20 {
entry:
  br label %loop
loop:
  %i = phi i32 [0, %entry], [%i.next, %loop]
  %p = getelementptr [16 x i32], ptr @cell_data, i32 0, i32 %c
  %v = load i32, ptr %p, !ejit.may_const !{}
  %i.next = add i32 %i, 1
  %cond = icmp slt i32 %i.next, 10
  br i1 %cond, label %loop, label %exit
exit:
  ret void
}

define void @entry_noloop(i32 %c) !ejit.metadata !20 {
  %p = getelementptr [16 x i32], ptr @cell_data, i32 0, i32 %c
  %v = load i32, ptr %p, !ejit.may_const !{}
  ret void
}

define void @entry_plain(i32 %c) !ejit.metadata !30 {
  %x = add i32 %c, 1
  ret void
}

!10 = distinct !{!{!"ejit_period_arr", !"cell", i32 16}, !{!"ejit_may_const_field", i32 0}}
!20 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}}
!30 = distinct !{!{!"ejit_entry"}}
