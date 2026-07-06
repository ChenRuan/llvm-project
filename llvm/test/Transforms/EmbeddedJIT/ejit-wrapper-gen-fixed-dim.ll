; Default (flag OFF): the wrapper keeps the generic variable-dimension entry.
; RUN: opt -passes=ejit-wrapper-gen -S %s \
; RUN:   | FileCheck %s --check-prefix=GENERIC
; Flag ON: entries with <= 4 dims call the matching fixed-dimension fast path.
; RUN: opt -passes=ejit-wrapper-gen -ejit-wrapper-fixed-dim-entry -S %s \
; RUN:   | FileCheck %s --check-prefix=FIXED

; GENERIC: jit_call:
; GENERIC: call i32 @ejit_taskpool_compile_or_get(i32 {{.*}}, ptr {{.*}}, i32 2, ptr {{.*}}, ptr {{.*}})
; GENERIC-NOT: @ejit_taskpool_compile_or_get_2d

; The fixed 2D entry receives the dim identity as scalar (dimType, instanceId)
; args instead of a stack EJitDimPair array + numDims.
; FIXED: jit_call:
; FIXED: call i32 @ejit_taskpool_compile_or_get_2d(i32 {{.*}}, i32 {{.*}}, i32 {{.*}}, i32 {{.*}}, i32 {{.*}}, ptr {{.*}}, ptr {{.*}})
; FIXED-NOT: call i32 @ejit_taskpool_compile_or_get(i32
; FIXED-NOT: alloca [4 x { i32, i32 }]

define i32 @multi_dim_entry(i32 %idx1, i32 %idx2) !ejit.metadata !0 {
entry:
  %v1 = load i32, ptr @data
  %v2 = load i32, ptr @data2
  ret i32 0
}

@data = global i32 0, !ejit.metadata !10
@data2 = global i32 0, !ejit.metadata !11

!0 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}, !{!"ejit_period_arr_ind", !"trp", i32 1}}
!10 = distinct !{!{!"ejit_period_arr", !"cell", i32 16}}
!11 = distinct !{!{!"ejit_period_arr", !"trp", i32 32}}
