; RUN: opt -passes=ejit-register-bitcode -S %s 2>&1 | FileCheck %s

; #1: ejit_entry whose specialization closure reads no ejit_may_const field
; has no JIT specialization value -> warn.

; CHECK: EJit warning: ejit_entry function 'entry_plain' reads no ejit_may_const field in its specialization closure
; CHECK: EJit warning: ejit_entry function 'entry_external_only' reads no ejit_may_const field in its specialization closure

; #1 must NOT fire for entries that do read ejit_may_const, whether directly or
; via an internal helper that the closure covers.
; CHECK-NOT: EJit warning: ejit_entry function 'entry_direct' reads no ejit_may_const
; CHECK-NOT: EJit warning: ejit_entry function 'entry_via_helper' reads no ejit_may_const

@cell_data = global [16 x i32] zeroinitializer, !ejit.metadata !10

; No ejit_may_const read, no may_const-reading callee -> warn.
define void @entry_plain(i32 %c) !ejit.metadata !30 {
  %x = add i32 %c, 1
  ret void
}

; Direct ejit_may_const read -> no warning.
define void @entry_direct(i32 %c) !ejit.metadata !20 {
  %p = getelementptr [16 x i32], ptr @cell_data, i32 0, i32 %c
  %v = load i32, ptr %p, !ejit.may_const !{}
  ret void
}

; ejit_may_const read hidden in an internal helper. The closure covers it, so
; the entry still has specialization value -> no warning.
define void @entry_via_helper(i32 %c) !ejit.metadata !20 {
  call void @helper_reads_mc(i32 %c)
  ret void
}
define internal void @helper_reads_mc(i32 %c) {
  %p = getelementptr [16 x i32], ptr @cell_data, i32 0, i32 %c
  %v = load i32, ptr %p, !ejit.may_const !{}
  ret void
}

; Only calls an external function. The JIT cannot inline external calls, so
; they do not bring may_const value into this entry's specialization -> warn.
define void @entry_external_only(i32 %c) !ejit.metadata !30 {
  call void @external_fn(i32 %c)
  ret void
}
declare void @external_fn(i32)

!10 = distinct !{!{!"ejit_period_arr", !"cell", i32 16}, !{!"ejit_may_const_field", i32 0}}
!20 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}}
!30 = distinct !{!{!"ejit_entry"}}
