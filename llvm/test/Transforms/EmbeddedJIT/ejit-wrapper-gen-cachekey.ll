; RUN: opt -passes=ejit-wrapper-gen -S %s | FileCheck %s
;
; Dimension packing coverage. With the unified taskpool wrapper, dimensions are
; packed into alloca'd {dimType, instanceId} pairs rather than a single cacheKey.
; These cases pin down the argument-handling for common and corner widths, and
; that the metadata-list ORDER (not the function-argument index) controls which
; slot each dimension fills.

; --- i8 dim (canonical uint8_t): trunc+zext pair collapses to a single zext.
;     No shift needed for positional ordering — each dim gets its own slot. ---
; CHECK-LABEL: define void @two_dim_i8(i8 %cell, i8 %trp)
; CHECK: jit_entry:
; CHECK: alloca [4 x { i32, i32 }]
; CHECK: alloca ptr
; CHECK: alloca i32
; CHECK: load i32, ptr @__ejit_funcidx_two_dim_i8
; CHECK: icmp ne i32 {{.*}}, -1
; CHECK: br i1 {{.*}}, label %jit_call, label %jit_fallback
; CHECK: jit_call:
; CHECK: getelementptr {{.*}} [4 x { i32, i32 }], ptr {{.*}}, i32 0, i32 0
; CHECK: load i32, ptr @__ejit_dimtype_cell
; CHECK: store i32 {{.*}}, ptr
; CHECK: zext i8 %cell to i32
; CHECK: store i32 {{.*}}, ptr
; CHECK: getelementptr {{.*}} i32 0, i32 1
; CHECK: load i32, ptr @__ejit_dimtype_trp
; CHECK: store i32 {{.*}}, ptr
; CHECK: zext i8 %trp to i32
; CHECK: store i32 {{.*}}, ptr
; CHECK: call i32 @ejit_taskpool_compile_or_get(i32 {{.*}}, ptr {{.*}}, i32 2, ptr {{.*}}, ptr {{.*}})
; CHECK: jit_dispatch:
; CHECK: call void @ejit_taskpool_release_read
define void @two_dim_i8(i8 %cell, i8 %trp) !ejit.metadata !0 {
entry:
  ret void
}

; --- single dim attached to argument index 1 (not 0): it is still the first
;     (and only) dimension in the list, so it must fill slot 0 and read %cell
;     (arg 1), not the ignored arg 0. ---
; CHECK-LABEL: define void @dim_on_arg1(i32 %ignored, i8 %cell)
; CHECK: jit_entry:
; CHECK: alloca [4 x { i32, i32 }]
; CHECK: load i32, ptr @__ejit_funcidx_dim_on_arg1
; CHECK: jit_call:
; CHECK: getelementptr {{.*}}, i32 0, i32 0
; CHECK: load i32, ptr @__ejit_dimtype_cell
; CHECK: zext i8 %cell to i32
; CHECK: call i32 @ejit_taskpool_compile_or_get(i32 {{.*}}, ptr {{.*}}, i32 1, ptr {{.*}}, ptr {{.*}})
; CHECK: jit_dispatch:
; CHECK: call void @ejit_taskpool_release_read
define void @dim_on_arg1(i32 %ignored, i8 %cell) !ejit.metadata !1 {
entry:
  ret void
}

; --- i64 dim: wider-than-i8 index must be truncated to i32 before storage
;     in the dim pair, so a value > 2^32 cannot overflow the slot. ---
; CHECK-LABEL: define void @dim_i64(i64 %cell)
; CHECK: jit_entry:
; CHECK: alloca [4 x { i32, i32 }]
; CHECK: load i32, ptr @__ejit_funcidx_dim_i64
; CHECK: jit_call:
; CHECK: trunc i64 %cell to i32
; CHECK: call i32 @ejit_taskpool_compile_or_get(i32 {{.*}}, ptr {{.*}}, i32 1, ptr {{.*}}, ptr {{.*}})
; CHECK: jit_dispatch:
; CHECK: call void @ejit_taskpool_release_read
define void @dim_i64(i64 %cell) !ejit.metadata !2 {
entry:
  ret void
}

!0 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}, !{!"ejit_period_arr_ind", !"trp", i32 1}}
!1 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 1}}
!2 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}}
