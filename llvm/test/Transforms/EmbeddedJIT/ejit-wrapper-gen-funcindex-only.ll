; BENCHMARK ONLY / UNSAFE FOR GENERAL USE: the -ejit-wrapper-bench-funcindex-only
; hidden option makes 0/1/2-dim ejit_entry wrappers call thin funcIndex-only
; cache-hit entries. The 1D/2D forms retain scalar dimensions only for a true
; miss. Default (option OFF) wrapper IR is unchanged and uses fixed-dimension
; entries — this test pins both modes.
;
; RUN: opt -passes=ejit-wrapper-gen -S %s | FileCheck --check-prefix=DEFAULT %s
; RUN: opt -passes=ejit-wrapper-gen -ejit-wrapper-bench-funcindex-only -S %s \
; RUN:   | FileCheck --check-prefix=FUNCONLY %s

define i32 @zero_dim_entry(i32 %n) !ejit.metadata !0 {
entry:
  ret i32 %n
}

define i32 @one_dim_entry(i32 %cell) !ejit.metadata !1 {
entry:
  ret i32 %cell
}

define i32 @two_dim_entry(i32 %cell, i32 %trp) !ejit.metadata !2 {
entry:
  %sum = add i32 %cell, %trp
  ret i32 %sum
}

@cell_data = global i32 0, !ejit.metadata !10
@trp_data = global i32 0, !ejit.metadata !11

!0 = distinct !{!{!"ejit_entry"}}
!1 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}}
!2 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}, !{!"ejit_period_arr_ind", !"trp", i32 1}}
!10 = distinct !{!{!"ejit_period_arr", !"cell", i32 16}}
!11 = distinct !{!{!"ejit_period_arr", !"trp", i32 32}}

; Default: the 0D fixed-dimension entry is emitted; the funcIndex-only entry is
; never referenced (byte-for-byte the prior behavior).
; DEFAULT-LABEL: define i32 @zero_dim_entry(i32 %n)
; DEFAULT: jit_call:
; DEFAULT: call i32 @ejit_taskpool_compile_or_get_0d(i32 {{.*}}, ptr {{.*}}, ptr {{.*}})
; DEFAULT-NOT: @ejit_taskpool_compile_or_get_func_only
; DEFAULT-LABEL: define i32 @one_dim_entry(i32 %cell)
; DEFAULT: call i32 @ejit_taskpool_compile_or_get_1d(i32 {{.*}}, i32 {{.*}}, i32 {{.*}}, ptr {{.*}}, ptr {{.*}})
; DEFAULT-LABEL: define i32 @two_dim_entry(i32 %cell, i32 %trp)
; DEFAULT: call i32 @ejit_taskpool_compile_or_get_2d(i32 {{.*}}, i32 {{.*}}, i32 {{.*}}, i32 {{.*}}, i32 {{.*}}, ptr {{.*}}, ptr {{.*}})

; Option ON: the funcIndex-only entry is emitted instead; the 0D fixed entry is
; not referenced.
; FUNCONLY-LABEL: define i32 @zero_dim_entry(i32 %n)
; FUNCONLY: jit_call:
; FUNCONLY: call i32 @ejit_taskpool_compile_or_get_func_only(i32 {{.*}}, ptr {{.*}}, ptr {{.*}})
; FUNCONLY-NOT: @ejit_taskpool_compile_or_get_0d
; FUNCONLY-LABEL: define i32 @one_dim_entry(i32 %cell)
; FUNCONLY: call i32 @ejit_taskpool_compile_or_get_func_only_1d(i32 {{.*}}, i32 {{.*}}, i32 {{.*}}, ptr {{.*}}, ptr {{.*}})
; FUNCONLY-NOT: @ejit_taskpool_compile_or_get_1d
; FUNCONLY-LABEL: define i32 @two_dim_entry(i32 %cell, i32 %trp)
; FUNCONLY: call i32 @ejit_taskpool_compile_or_get_func_only_2d(i32 {{.*}}, i32 {{.*}}, i32 {{.*}}, i32 {{.*}}, i32 {{.*}}, ptr {{.*}}, ptr {{.*}})
; FUNCONLY-NOT: @ejit_taskpool_compile_or_get_2d
