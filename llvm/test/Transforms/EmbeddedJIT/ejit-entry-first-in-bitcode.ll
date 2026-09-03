; RUN: rm -rf %t-dump && mkdir -p %t-dump
; RUN: opt -passes=ejit-register-bitcode -ejit-dump-bitcode-dir=%t-dump -S <%s >/dev/null
; RUN: opt -S %t-dump/*.bc | FileCheck --check-prefix=EXTRACTED %s
;
; The extracted bitcode must list every ejit_entry definition BEFORE any other
; definition, keeping the entries in their original relative order.
;
; Why: the JIT TargetMachine leaves function-sections off, so codegen emits all
; functions into one .text in module order with no inter-function padding on
; AArch64, while EJitCodePoolManager::allocateCode floors each allocation's
; alignment at minCodeAlign (64B, or the seal page size under 4K sealing). Only
; the first function of .text inherits that alignment. A specialization compiles
; one entry and the JIT deletes the other entries' bodies, so putting the
; entries first makes the compiled entry start on a cache-line boundary instead
; of at an arbitrary 4-byte offset.
;
; Note the source order below: a helper is defined first, and the two entries
; are interleaved with a second helper.

define i32 @helper_add(i32 %a, i32 %b) {
  %sum = add i32 %a, %b
  ret i32 %sum
}

define i32 @entry_first(i32 %x) !ejit.metadata !2 {
  %a = call i32 @helper_add(i32 %x, i32 1)
  ret i32 %a
}

define i32 @helper_mul(i32 %a, i32 %b) {
  %prod = mul i32 %a, %b
  ret i32 %prod
}

define i32 @entry_second(i32 %x) !ejit.metadata !3 {
  %b = call i32 @helper_mul(i32 %x, i32 2)
  ret i32 %b
}

; The leading CHECK-NOT spans from the start of the file to the entry_first
; match: no helper may be defined ahead of the first entry. Then both entries
; appear in their original relative order, with the helpers after them.
; EXTRACTED-NOT:  define{{.*}}@helper_
; EXTRACTED:      define{{.*}} i32 @entry_first(
; EXTRACTED:      define{{.*}} i32 @entry_second(
; EXTRACTED:      define internal i32 @helper_

!0 = !{!"ejit_entry"}
!2 = !{!0}
!3 = !{!0}
