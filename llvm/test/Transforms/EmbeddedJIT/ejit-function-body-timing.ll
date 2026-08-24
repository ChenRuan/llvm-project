; RUN: opt -passes=ejit-wrapper-gen -S %s | FileCheck %s --check-prefix=OFF
; RUN: opt -passes=ejit-wrapper-gen -ejit-function-body-timing -S %s | FileCheck %s --check-prefix=JIT
; RUN: opt -passes=ejit-wrapper-gen -ejit-function-body-timing -S %s | FileCheck %s --check-prefix=AOT
; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -ejit-function-body-timing -S %s | FileCheck %s --check-prefix=ICACHE

; OFF-NOT: @ejit_function_body_cycles_record

; The JIT interval starts after lookup/dispatch and ends immediately after the
; selected function call. Read-token release and aggregation are outside it.
; JIT-LABEL: define i32 @timed_body(
; JIT-LABEL: jit_dispatch:
; JIT: %ejit_jit_body_begin = call i64 @ejit_taskpool_trace_now()
; JIT-NEXT: {{.*}}call i32 %ejit_fn
; JIT-NEXT: %ejit_body_end = call i64 @ejit_taskpool_trace_now()
; JIT-NEXT: call void @ejit_function_body_cycles_record({{.*}}i32 1, i64 %ejit_jit_body_begin, i64 %ejit_body_end)
; JIT: call void @ejit_taskpool_release_read

; The original AOT CFG is bracketed independently. Both return sites report an
; AOT sample, including fallback caused by an invalid funcIndex or cache miss.
; AOT-LABEL: aot.positive:
; AOT: %ejit_body_end{{.*}} = call i64 @ejit_taskpool_trace_now()
; AOT-NEXT: call void @ejit_function_body_cycles_record({{.*}}i32 0, i64 %ejit_aot_body_begin, i64 %ejit_body_end{{.*}})
; AOT-NEXT: ret i32
; AOT-LABEL: aot.nonpositive:
; AOT: %ejit_body_end{{.*}} = call i64 @ejit_taskpool_trace_now()
; AOT-NEXT: call void @ejit_function_body_cycles_record({{.*}}i32 0, i64 %ejit_aot_body_begin, i64 %ejit_body_end{{.*}})
; AOT-NEXT: ret i32
; AOT-LABEL: jit_fallback:
; AOT: %ejit_aot_body_begin = call i64 @ejit_taskpool_trace_now()

; Inline-cache JIT hits use the same JIT path label and are no longer musttail
; while instrumentation is enabled, because the end timestamp follows the call.
; ICACHE-LABEL: define i32 @timed_body(
; ICACHE-LABEL: jit_icache_dispatch:
; ICACHE: %ejit_jit_body_begin = call i64 @ejit_taskpool_trace_now()
; ICACHE-NOT: musttail
; ICACHE-NEXT: {{.*}}call i32 %ejit_ic_fn
; ICACHE-NEXT: %ejit_body_end = call i64 @ejit_taskpool_trace_now()
; ICACHE-NEXT: call void @ejit_function_body_cycles_record({{.*}}i32 1, i64 %ejit_jit_body_begin, i64 %ejit_body_end)
; ICACHE-NEXT: ret i32
; ICACHE-LABEL: define internal i32 @timed_body_miss(
; ICACHE-LABEL: miss_fallback:
; ICACHE: %ejit_aot_body_begin = call i64 @ejit_taskpool_trace_now()

define i32 @timed_body(i32 %cell, i32 %value) !ejit.metadata !0 {
entry:
  %positive = icmp sgt i32 %value, 0
  br i1 %positive, label %aot.positive, label %aot.nonpositive

aot.positive:
  %sum = add i32 %value, 7
  ret i32 %sum

aot.nonpositive:
  %neg = sub i32 0, %value
  ret i32 %neg
}

@data = global [16 x i32] zeroinitializer, !ejit.metadata !10

!0 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}}
!10 = distinct !{!{!"ejit_period_arr", !"cell", i32 16}}
