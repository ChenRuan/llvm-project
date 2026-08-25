; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -ejit-icache-split-dispatch-1d=false -S %s | FileCheck %s --check-prefix=DEFAULT
; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -ejit-icache-split-dispatch-1d=true -S %s | FileCheck %s --check-prefix=SPLIT
; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -ejit-icache-split-dispatch-1d=true -ejit-icache-direct-dispatch-pads=true -S %s | FileCheck %s --check-prefix=DIRECT
; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -ejit-icache-split-dispatch-1d=true -ejit-icache-direct-dispatch-pads=true -ejit-icache-last-pair-cache-2d=true -S %s | FileCheck %s --check-prefix=PAIR
; RUN: opt -passes=ejit-wrapper-gen -ejit-inline-cache -ejit-icache-section= -ejit-icache-split-dispatch-1d=true -ejit-icache-direct-dispatch-pads=true -ejit-icache-last-pair-cache-2d=true -S %s | llc -mtriple=aarch64-unknown-none-elf -O2 | FileCheck %s --check-prefix=PAIRASM

; The default remains the compact, single indirect callsite.
; DEFAULT-NOT: jit_icache_probe_0:
; DEFAULT-COUNT-4: jit_icache_dispatch:

; Eligible one-dimensional cell entries use a bit-test conditional branch tree.
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
; SPLIT-DAG: and i32 %cell, 8
; SPLIT-DAG: and i32 %cell, 4
; SPLIT-DAG: and i32 %cell, 2
; SPLIT-DAG: and i32 %cell, 1

; trp is the other supported one-dimensional lifecycle name.
; SPLIT-LABEL: define i32 @trp_entry(
; SPLIT: %ejit_split_in_range = icmp ult i32 %trp, 16
; SPLIT: jit_icache_dispatch_0:
; SPLIT: jit_icache_dispatch_15:

; Two-dimensional entries deliberately retain the existing compact dispatcher.
; SPLIT-LABEL: define i32 @two_dim_entry(
; SPLIT-NOT: jit_icache_probe_0:
; SPLIT: jit_icache_dispatch:

; Direct-pad mode removes the slot load and indirect call from eligible leaves.
; The AOT pad symbols initially tail-call the common miss function; runtime
; publication patches that one B instruction to the JIT body.
; DIRECT-DAG: @__ejit_icache_pad_table_cell_entry = private constant [17 x ptr]
; DIRECT-DAG: @__ejit_icache_pad_table_trp_entry = private constant [17 x ptr]
; DIRECT-DAG: @.ejit.registry.icache_pads = private constant {{.*}} i32 8, {{.*}} ptr @__ejit_icache_pad_table_cell_entry, i64 16
; DIRECT-LABEL: define i32 @cell_entry(
; DIRECT: %ejit_split_in_range = icmp ult i32 %cell, 16
; DIRECT-NOT: load atomic ptr
; DIRECT-COUNT-16: musttail call i32 @__ejit_icache_pad_cell_entry_
; DIRECT-LABEL: define i32 @trp_entry(
; DIRECT-NOT: load atomic ptr
; DIRECT-COUNT-16: musttail call i32 @__ejit_icache_pad_trp_entry_
; DIRECT-LABEL: define i32 @two_dim_entry(
; DIRECT: load atomic ptr
; DIRECT: musttail call i32 %ejit_ic_fn
; DIRECT-DAG: define internal i32 @__ejit_icache_pad_cell_entry_0({{.*}}) {{.*}}section ".text.ejit_pads" align 4
; DIRECT-DAG: musttail call i32 @cell_entry_miss(

; A two-dimensional cell/trp entry keeps one last descriptor in core-private
; data. A stable pair takes one key comparison and dispatches through its fixed
; AOT pad; only a changed pair executes both bounds checks and updates the key.
; PAIR-DAG: @__ejit_icache_pad_table_two_dim_entry = private constant [257 x ptr]
; PAIR-DAG: @__ejit_icache_pair_descs_two_dim_entry = private constant [256 x { i64, ptr }]
; PAIR-DAG: { i64 4294967296, ptr @__ejit_icache_pad_two_dim_entry_16 }
; PAIR-DAG: @__ejit_icache_last_pair_two_dim_entry = internal global ptr @__ejit_icache_pair_invalid_two_dim_entry, align 8
; PAIR-DAG: @.ejit.registry.icache_pads = private constant {{.*}} ptr @__ejit_icache_pad_table_two_dim_entry, i64 256
; PAIR-LABEL: define i32 @two_dim_entry(
; PAIR: %ejit_pair_index = add i32 {{.*}}, %trp
; PAIR: %ejit_pair_key = or i64
; PAIR: %ejit_cached_pair_desc = load atomic ptr, ptr @__ejit_icache_last_pair_two_dim_entry monotonic, align 8
; PAIR: %ejit_cached_pair_key = load i64
; PAIR: %ejit_pair_cache_hit = icmp eq i64 %ejit_pair_key, %ejit_cached_pair_key
; PAIR: jit_pair_cache_hit:
; PAIR: %ejit_cached_pad = load ptr
; PAIR: musttail call i32 %ejit_cached_pad
; PAIR: jit_pair_cache_cold:
; PAIR: icmp ult i32 %cell, 16
; PAIR: icmp ult i32 %trp, 16
; PAIR: jit_pair_cache_update:
; PAIR: store atomic ptr %ejit_pair_desc, ptr @__ejit_icache_last_pair_two_dim_entry monotonic, align 8
; PAIR: musttail call i32 %ejit_pair_pad

; The stable-pair machine path has one key compare, not a 16x16 selector.
; PAIRASM-LABEL: two_dim_entry:
; PAIRASM: ldr x[[DESC:[0-9]+]], [x{{[0-9]+}}, {{.*}}__ejit_icache_last_pair_two_dim_entry]
; PAIRASM: ldr x[[OLDKEY:[0-9]+]], [x[[DESC]]]
; PAIRASM: cmp x{{[0-9]+}}, x[[OLDKEY]]
; PAIRASM-NEXT: b.ne
; PAIRASM: ldr x[[TARGET:[0-9]+]], [x[[DESC]], #8]
; PAIRASM: br x[[TARGET]]

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
