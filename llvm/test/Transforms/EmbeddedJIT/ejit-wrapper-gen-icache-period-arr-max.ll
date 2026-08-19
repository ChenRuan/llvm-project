; Representative mode no longer indexes a [D]^numDims table on the hit path.
; Period arrays larger than the former D=16 limit are therefore safe and still
; receive the fixed {fnPtr, identityKey} object, independent of declaration
; order. This keeps the old regression input to prove that the size-based
; all-or-nothing degradation is gone.

; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -S %s \
; RUN:   | FileCheck %s --check-prefix=BIGFIRST
; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -S \
; RUN:       %S/Inputs/ejit-icache-period-arr-max-smallfirst.ll \
; RUN:   | FileCheck %s --check-prefix=SMALLFIRST

; BIGFIRST-DAG: @__ejit_icache_fn_over_cap_entry = internal global { ptr, i64 } zeroinitializer
; BIGFIRST-DAG: @__ejit_icache_fn_in_cap_entry = internal global { ptr, i64 } zeroinitializer

; BIGFIRST-LABEL: define i32 @over_cap_entry(
; BIGFIRST: load atomic ptr, ptr %ejit_ic_slot monotonic, align 8
; BIGFIRST-LABEL: jit_icache_dispatch:

; BIGFIRST-LABEL: define i32 @in_cap_entry(
; BIGFIRST: load atomic ptr, ptr %ejit_ic_slot monotonic, align 8

; --- Declared 8 then 32: identical outcome. ---
; SMALLFIRST-LABEL: define i32 @over_cap_entry(
; SMALLFIRST: load atomic ptr, ptr %ejit_ic_slot monotonic, align 8

define i32 @over_cap_entry(i32 %idx) !ejit.metadata !1 {
entry:
  %v = load i32, ptr @big
  ret i32 %v
}

define i32 @in_cap_entry(i32 %idx) !ejit.metadata !2 {
entry:
  %v = load i32, ptr @okA
  ret i32 %v
}

; "wide" owns two arrays: 32 entries and 8 entries, declared large-first here.
@big   = global i32 0, !ejit.metadata !10
@small = global i32 0, !ejit.metadata !11
; "narrow" owns two arrays, both within EJIT_ICACHE_DIM_SIZE.
@okA = global i32 0, !ejit.metadata !12
@okB = global i32 0, !ejit.metadata !13

!1 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"wide", i32 0}}
!2 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"narrow", i32 0}}
!10 = distinct !{!{!"ejit_period_arr", !"wide", i32 32}}
!11 = distinct !{!{!"ejit_period_arr", !"wide", i32 8}}
!12 = distinct !{!{!"ejit_period_arr", !"narrow", i32 8}}
!13 = distinct !{!{!"ejit_period_arr", !"narrow", i32 16}}
