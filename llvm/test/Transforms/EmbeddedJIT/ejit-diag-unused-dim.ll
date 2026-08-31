; RUN: opt -passes=ejit-register-bitcode -S %s 2>&1 | FileCheck %s
; RUN: opt -passes=ejit-register-bitcode -ejit-warn-unused-dim=false -S %s 2>&1 | FileCheck %s --check-prefix=OFF

; #2: an ejit_entry that declares ejit_period_arr_ind(P) but whose
; specialization closure neither reads that parameter nor indexes an
; ejit_period_arr(P), and has no ejit_bound_ptr bound to P, has an unused
; specialization dimension.

; The classic shape: the cell dimension's parameter is dead and no closure
; function touches cell_data (only trp_data, the other dimension).
; CHECK: EJit warning: ejit_entry function 'entry_unused_cell' declares ejit_period_arr_ind('cell') but its specialization closure neither reads that parameter nor indexes an ejit_period_arr('cell')

; A dimension that is used in ANY form must NOT warn: parameter read (even as
; a plain value - the runtime substitutes the index parameter with a constant
; and folds every use), period array referenced, or bound to an
; ejit_bound_ptr parameter (EJitWrapperGen hard-requires a matching
; dimension, so removing it is a compile error).
; CHECK-NOT: declares ejit_period_arr_ind('trp')
; CHECK-NOT: ejit_entry function 'entry_used' declares ejit_period_arr_ind
; CHECK-NOT: ejit_entry function 'entry_value_use' declares ejit_period_arr_ind
; CHECK-NOT: ejit_entry function 'entry_bound_ptr' declares ejit_period_arr_ind
; CHECK-NOT: ejit_entry function 'entry_const_index' declares ejit_period_arr_ind

; With the flag off, no #2 warning at all.
; OFF-NOT: declares ejit_period_arr_ind

@cell_data = global [16 x i32] zeroinitializer, !ejit.metadata !10
@trp_data = global [8 x i32] zeroinitializer, !ejit.metadata !11

; Declares cell + trp, but only indexes trp_data and never reads %c -> the
; cell dim is unused. (Still has specialization value via trp, so #1 does not
; fire either.)
define i32 @entry_unused_cell(i32 %c, i32 %t) !ejit.metadata !21 {
  %p = getelementptr [8 x i32], ptr @trp_data, i32 0, i32 %t
  %v = load i32, ptr %p, !ejit.may_const !{}
  ret i32 %v
}

; Declares cell and indexes cell_data with %c -> dim used, no warning.
define i32 @entry_used(i32 %c) !ejit.metadata !20 {
  %p = getelementptr [16 x i32], ptr @cell_data, i32 0, i32 %c
  %v = load i32, ptr %p, !ejit.may_const !{}
  ret i32 %v
}

; Both dims' parameters are read as plain values; neither period array is
; referenced. The runtime folds the substituted index constants into the
; arithmetic, so both dims are used -> no warning (regression test for the
; false positive this warning used to emit on value-only uses).
define i32 @entry_value_use(i32 %c, i32 %t) !ejit.metadata !21 {
  %p = getelementptr [8 x i32], ptr @trp_data, i32 0, i32 1
  %m = load i32, ptr %p, !ejit.may_const !{}
  %x = add i32 %c, %t
  %s = add i32 %x, %m
  ret i32 %s
}

; The cell dim's parameter is dead and cell_data is never referenced, but an
; ejit_bound_ptr parameter is bound to cell: EJitWrapperGen requires exactly
; one matching ejit_period_arr_ind, so the dim cannot be removed -> no
; warning.
define i32 @entry_bound_ptr(i32 %c, ptr %p) !ejit.metadata !22 {
  %v = load i32, ptr %p, !ejit.may_const !{}
  ret i32 %v
}

; The cell dim's parameter is dead, but the closure still reads cell_data at
; a constant index: the folded may_const values are refreshed by recompiling
; when the cell period advances, so the dim stays load-bearing -> no warning.
define i32 @entry_const_index(i32 %c) !ejit.metadata !20 {
  %p = getelementptr [16 x i32], ptr @cell_data, i32 0, i32 2
  %v = load i32, ptr %p, !ejit.may_const !{}
  ret i32 %v
}

!10 = distinct !{!{!"ejit_period_arr", !"cell", i32 16}, !{!"ejit_may_const_field", i32 0}}
!11 = distinct !{!{!"ejit_period_arr", !"trp", i32 8}, !{!"ejit_may_const_field", i32 0}}
!20 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}}
!21 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}, !{!"ejit_period_arr_ind", !"trp", i32 1}}
!22 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}, !{!"ejit_bound_ptr", !"cell", i32 1, i64 4}}
