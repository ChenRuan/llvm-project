; Input for ejit-wrapper-gen-icache-period-arr-max.ll: identical to that test's
; module except the two "wide" arrays are declared SMALL (8) first and BIG (32)
; second.
;
; This is the FIRST-wins half of the matrix. Visiting in declaration order, a
; first-wins collectPeriodArraySizes() keeps 8 and wrongly approves the probe;
; a last-wins one happens to keep 32 and declines for the wrong reason. The
; sibling file has the opposite order and so catches last-wins. Only taking the
; maximum is correct for both.

define i32 @over_cap_entry(i32 %idx) !ejit.metadata !1 {
entry:
  %v = load i32, ptr @small
  ret i32 %v
}

@small = global i32 0, !ejit.metadata !11
@big   = global i32 0, !ejit.metadata !10

!1 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"wide", i32 0}}
!10 = distinct !{!{!"ejit_period_arr", !"wide", i32 32}}
!11 = distinct !{!{!"ejit_period_arr", !"wide", i32 8}}
