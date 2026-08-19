; -ejit-inline-cache (ON): emits an inline probe DIRECTLY in the ejit_entry
; wrapper - the first dimension identity claims the per-function
; @__ejit_icache_fn_<name> representative object. Before its pointer is filled, other
; identities reach AOT without compile_or_get. Once filled, every identity loads
; and calls the representative specialization from that same cell. The hit path
; (jit_icache_dispatch) calls the cached specialization directly with NO
; ejit_icache_try call and NO ejit_taskpool_release_read. The load is atomic
; because the table is shared across cores (a peer's period toggle zeroes cells
; in place); monotonic is the weakest order that makes that a defined race, and
; lowers to the same LDR a plain load would. There is NO freshness check on the
; hit path - draining the shared cells IS the invalidation. numDims=0 is a
; scalar cell (no GEP). Default (OFF): the original 4-block wrapper, no icache.
; Idempotent: running the pass twice does not duplicate the probe.

; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -S %s | FileCheck %s --check-prefix=ICACHE
; RUN: opt -passes=ejit-wrapper-gen -S %s | FileCheck %s --check-prefix=NOICACHE
; RUN: opt -passes=ejit-wrapper-gen,ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -S %s | FileCheck %s --check-prefix=IDEM
; --- -ejit-inline-cache + -ejit-dispatcher-cluster + -ejit-missfn-cold ---
; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -ejit-dispatcher-cluster -ejit-missfn-cold -S %s | FileCheck %s --check-prefix=OPT
; --- -ejit-wrapper-timing + -ejit-inline-cache: the hit path emits trace calls
;     AFTER the specialization call, so it must NOT be musttail (musttail must
;     immediately precede a ret). Verify the module is valid (opt verifies by
;     default) and the hit path is a plain call. Regression for the broken-IR
;     boot crash where codegen silently dropped the ejit_entry function. ---
; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -ejit-wrapper-timing -disable-output %s
; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -ejit-wrapper-timing -S %s | FileCheck %s --check-prefix=TIMING
; --- -ejit-icache-section: the cell table goes into the inter-core SHARED
;     section, which is what lets a deactivate on one core zero the cells every
;     other core probes. Nothing else about the wrapper changes. ---
; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section=.mc_shared -S %s | FileCheck %s --check-prefix=SECTION

; --- icache globals: fixed {fnPtr, miss-only identity key} objects. ---
; ICACHE-DAG: @__ejit_icache_fn_zero_dim_entry = internal global { ptr, i64 } zeroinitializer, align 8
; ICACHE-DAG: @__ejit_icache_fn_one_dim_entry = internal global { ptr, i64 } zeroinitializer, align 8
; ICACHE-DAG: @__ejit_icache_fn_two_dim_entry = internal global { ptr, i64 } zeroinitializer, align 8
; ICACHE-NOT: __ejit_icache_version_

; --- 0D entry: scalar slot, direct plain load (NO GEP).  Without extra flags
;     the dispatcher stays in default .text (no section attribute). ---
; ICACHE-LABEL: define i32 @zero_dim_entry(
; ICACHE-NOT: section
; ICACHE-NOT: ejit_icache_try
; ICACHE: getelementptr inbounds { ptr, i64 }, ptr @__ejit_icache_fn_zero_dim_entry, i32 0, i32 0
; ICACHE-NEXT: load atomic ptr, ptr %ejit_ic_slot monotonic, align 8
; ICACHE-LABEL: jit_icache_dispatch:
; ICACHE-NOT: call void @ejit_taskpool_release_read
; ICACHE: call {{.*}} %ejit_ic_fn
; ICACHE: ret

; --- 1D entry: hit path ignores the current dim and reads the fixed slot. ---
; ICACHE-LABEL: define i32 @one_dim_entry(
; ICACHE-NOT: section
; ICACHE-NOT: ejit_icache_try
; ICACHE-NOT: ejit_icache_stored_version
; ICACHE: getelementptr inbounds { ptr, i64 }, ptr @__ejit_icache_fn_one_dim_entry, i32 0, i32 0
; ICACHE: load atomic ptr, ptr %ejit_ic_slot monotonic, align 8
; ICACHE-LABEL: jit_icache_dispatch:
; ICACHE-NOT: call void @ejit_taskpool_release_read
; ICACHE: call {{.*}} %ejit_ic_fn
; ICACHE: ret

; --- 2D entry: same fixed-slot load; no dimension-dependent GEP. ---
; ICACHE-LABEL: define i32 @two_dim_entry(
; ICACHE-NOT: section
; ICACHE: getelementptr inbounds { ptr, i64 }, ptr @__ejit_icache_fn_two_dim_entry, i32 0, i32 0

; --- Slow path asks the runtime to admit the first identity. A different
;     identity branches to miss_fallback before compile_or_get. ---
; ICACHE-LABEL: define internal i32 @one_dim_entry_miss(
; ICACHE: call i32 @ejit_icache_claim(i32 %ejit_funcidx, i64 %ejit_icache_claim_key)
; ICACHE: br i1 {{.*}}, label %miss_call, label %miss_fallback
; ICACHE: call i32 @ejit_taskpool_compile_or_get_1d

; --- OPT (dispatcher-cluster + missfn-cold ON): section and cold present ---
; OPT-LABEL: define i32 @zero_dim_entry(
; OPT-SAME: section ".text.ejit_dispatch"
; OPT-LABEL: define i32 @one_dim_entry(
; OPT-SAME: section ".text.ejit_dispatch"
; OPT-LABEL: define i32 @two_dim_entry(
; OPT-SAME: section ".text.ejit_dispatch"
; OPT-LABEL: define internal i32 @zero_dim_entry_miss(
; OPT-SAME: #[[MISS_ATTRS:[0-9]+]]
; OPT-LABEL: define internal i32 @one_dim_entry_miss(
; OPT-SAME: #[[MISS_ATTRS]]
; OPT-LABEL: define internal i32 @two_dim_entry_miss(
; OPT-SAME: #[[MISS_ATTRS]]
; OPT-DAG: attributes #[[MISS_ATTRS]] = { cold noinline }

; --- registration uses representative-mode discriminator 5. ---
; ICACHE-DAG: call void @ejit_register_icache_slot({{.*}} @__ejit_icache_fn_zero_dim_entry, i32 5)
; ICACHE-DAG: call void @ejit_register_icache_slot({{.*}} @__ejit_icache_fn_one_dim_entry, i32 5)
; ICACHE-DAG: call void @ejit_register_icache_slot({{.*}} @__ejit_icache_fn_two_dim_entry, i32 5)

; --- -ejit-icache-section (default .mc_shared, pinned here so the test does not
;     depend on the CMake value): every cell table lands in the inter-core
;     shared section and the probe is unchanged (one monotonic load + null
;     check, no freshness compare). The other RUN lines pin it EMPTY to check
;     the plain-.bss shape. ---
; SECTION-DAG: @__ejit_icache_fn_zero_dim_entry = internal global { ptr, i64 } zeroinitializer, section ".mc_shared", align 8
; SECTION-DAG: @__ejit_icache_fn_one_dim_entry = internal global { ptr, i64 } zeroinitializer, section ".mc_shared", align 8
; SECTION-DAG: @__ejit_icache_fn_two_dim_entry = internal global { ptr, i64 } zeroinitializer, section ".mc_shared", align 8
; SECTION-LABEL: define i32 @one_dim_entry(
; SECTION: load atomic ptr, ptr {{.*}} monotonic, align 8
; SECTION-LABEL: jit_icache_dispatch:
; SECTION-NOT: load
; SECTION: call {{.*}} %ejit_ic_fn

; --- Default (flag OFF): no icache anywhere; original compile_or_get path. ---
; NOICACHE-LABEL: define i32 @one_dim_entry(
; NOICACHE-NOT: __ejit_icache_fn
; NOICACHE-NOT: ejit_register_icache_slot
; NOICACHE: call i32 @ejit_taskpool_compile_or_get_1d(i32 {{.*}}, i32 {{.*}}, i32 {{.*}}, ptr {{.*}}, ptr {{.*}})

; --- Idempotent: two passes emit each probe exactly once. ---
; IDEM-LABEL: define i32 @one_dim_entry(
; IDEM-COUNT-1: load atomic ptr, ptr %ejit_ic_slot monotonic, align 8

; --- timing hit path: plain call (NOT musttail) + trace + ret. The miss path
;     stays musttail (no trailing calls), so we only forbid musttail within the
;     hit block. ---
; TIMING-LABEL: define i32 @zero_dim_entry(
; TIMING-LABEL: jit_icache_dispatch:
; TIMING-NOT: musttail
; TIMING: call i32 %ejit_ic_fn
; TIMING: call i64 @ejit_taskpool_trace_now
; TIMING: call void @ejit_taskpool_trace_wrapper
; TIMING: ret

define i32 @zero_dim_entry(i32 %x) !ejit.metadata !0 {
entry:
  %v1 = load i32, ptr @data
  ret i32 0
}

define i32 @one_dim_entry(i32 %idx1) !ejit.metadata !1 {
entry:
  %v1 = load i32, ptr @data
  ret i32 0
}

define i32 @two_dim_entry(i32 %a, i32 %b) !ejit.metadata !2 {
entry:
  %v1 = load i32, ptr @data
  %v2 = load i32, ptr @data2
  ret i32 0
}

@data = global i32 0, !ejit.metadata !10
@data2 = global i32 0, !ejit.metadata !11

!0 = distinct !{!{!"ejit_entry"}}
!1 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}}
!2 = distinct !{!{!"ejit_entry"}, !{!"ejit_period_arr_ind", !"cell", i32 0}, !{!"ejit_period_arr_ind", !"trp", i32 1}}
!10 = distinct !{!{!"ejit_period_arr", !"cell", i32 16}}
!11 = distinct !{!{!"ejit_period_arr", !"trp", i32 16}}
