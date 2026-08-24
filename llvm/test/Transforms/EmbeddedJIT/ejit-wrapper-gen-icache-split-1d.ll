; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -ejit-icache-split-dispatch-1d=false -S %s | FileCheck %s --check-prefix=DEFAULT
; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -ejit-icache-split-dispatch-1d=true -S %s | FileCheck %s --check-prefix=SPLIT

; The default remains the compact, single indirect callsite.
; DEFAULT-NOT: jit_icache_probe_0:
; DEFAULT-COUNT-4: jit_icache_dispatch:

; Eligible one-dimensional cell entries use a direct conditional branch tree.
; Values outside [0, 15] bypass the inline table and take the existing miss path.
; Every in-range instance has a distinct constant slot load and indirect call PC.
; SPLIT-LABEL: define i32 @cell_entry(
; SPLIT: %ejit_split_in_range = icmp ult i32 %cell, 16
; SPLIT: br i1 %ejit_split_in_range, label %jit_icache_select_0_16, label %jit_miss
; SPLIT-NOT: switch
; SPLIT: jit_icache_probe_0:
; SPLIT: load atomic ptr, ptr @__ejit_icache_fn_cell_entry monotonic
; SPLIT: jit_icache_dispatch_0:
; SPLIT-COUNT-16: musttail call i32 %ejit_ic_fn_

; trp is the other supported one-dimensional lifecycle name.
; SPLIT-LABEL: define i32 @trp_entry(
; SPLIT: %ejit_split_in_range = icmp ult i32 %trp, 16
; SPLIT: jit_icache_dispatch_0:
; SPLIT: jit_icache_dispatch_15:

; Two-dimensional entries deliberately retain the existing compact dispatcher.
; SPLIT-LABEL: define i32 @two_dim_entry(
; SPLIT-NOT: jit_icache_probe_0:
; SPLIT: jit_icache_dispatch:

; Other one-dimensional lifecycle names also retain the compact dispatcher.
; SPLIT-LABEL: define i32 @other_entry(
; SPLIT-NOT: jit_icache_probe_0:
; SPLIT: jit_icache_dispatch:

define i32 @cell_entry(i32 %cell, i32 %value) !ejit.metadata !0 {
entry:
  %r = add i32 %value, 1
  ret i32 %r
}

define i32 @trp_entry(i32 %trp) !ejit.metadata !1 {
entry:
  ret i32 %trp
}

define i32 @two_dim_entry(i32 %cell, i32 %trp) !ejit.metadata !2 {
entry:
  %r = add i32 %cell, %trp
  ret i32 %r
}

define i32 @other_entry(i32 %slot) !ejit.metadata !3 {
entry:
  ret i32 %slot
}

@cell_data = global i32 0, !ejit.metadata !10
@trp_data = global i32 0, !ejit.metadata !11
@other_data = global i32 0, !ejit.metadata !12

!0 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}}
!1 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"trp", i32 0}}
!2 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}, !{!"ejit_period_arr_ind", !"trp", i32 1}}
!3 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"slot", i32 0}}
!10 = distinct !{!{!"ejit_period_arr", !"cell", i32 16}}
!11 = distinct !{!{!"ejit_period_arr", !"trp", i32 16}}
!12 = distinct !{!{!"ejit_period_arr", !"slot", i32 16}}
