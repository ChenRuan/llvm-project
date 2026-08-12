; A period may own SEVERAL ejit_period_arr globals, activated as a group, so its
; valid instance ids run up to the LARGEST of them. collectPeriodArraySizes()
; must therefore aggregate a maximum, not keep whichever global the module
; happens to list last.
;
; If it keeps the last one, the answer depends on declaration order: with sizes
; 32 and 8 under one period, visiting 8 last records 8, dimsProvablyInRange()
; approves the entry, and the wrapper emits a probe whose GEP is bounded by
; EJIT_ICACHE_DIM_SIZE (16) even though the lifecycle can legally produce
; identity 31. That GEP is inbounds on a 16-cell table and lands outside it --
; in the shared section, in whatever object comes next -- and the probe branches
; through it before the slow path's range check is ever reachable.
;
; Both orders must therefore decline the probe. The entry is still wrapped; the
; taskpool serves it, which is the documented degradation.
;
; The two orders catch different wrong implementations: BIG-first (this file)
; catches last-wins, SMALL-first (the Inputs sibling) catches first-wins.
;
; --implicit-check-not spans the whole module, so the over-cap cell table must
; not appear anywhere -- as a global, a GEP operand, or a registration argument.

; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -S %s \
; RUN:   | FileCheck %s --check-prefix=BIGFIRST \
; RUN:       --implicit-check-not=__ejit_icache_fn_over_cap_entry
; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -S \
; RUN:       %S/Inputs/ejit-icache-period-arr-max-smallfirst.ll \
; RUN:   | FileCheck %s --check-prefix=SMALLFIRST \
; RUN:       --implicit-check-not=__ejit_icache_fn_over_cap_entry

; --- Declared 32 then 8: the order that made a last-wins implementation record
; --- 8 and emit the probe. An entry under a period whose arrays ALL fit still
; --- gets its table, so the max aggregation does not just disable everything. ---
; BIGFIRST: @__ejit_icache_fn_in_cap_entry = internal global [16 x ptr] zeroinitializer

; --- over_cap_entry: no probe, straight to the taskpool. ---
; BIGFIRST-LABEL: define i32 @over_cap_entry(
; BIGFIRST-NOT: jit_icache_dispatch
; BIGFIRST: call {{.*}}@ejit_taskpool_compile_or_get

; --- in_cap_entry: probe present, indexing its own 16-cell table. ---
; BIGFIRST-LABEL: define i32 @in_cap_entry(
; BIGFIRST: getelementptr {{.*}} ptr @__ejit_icache_fn_in_cap_entry, i32 0, i32 {{.*}}

; --- Declared 8 then 32: identical outcome. ---
; SMALLFIRST-LABEL: define i32 @over_cap_entry(
; SMALLFIRST-NOT: jit_icache_dispatch
; SMALLFIRST: call {{.*}}@ejit_taskpool_compile_or_get

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
