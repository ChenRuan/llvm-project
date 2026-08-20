; RUN: rm -rf %t-dump && mkdir -p %t-dump
; RUN: opt -passes=ejit-register-bitcode -ejit-dump-bitcode-dir=%t-dump -S <%s >/dev/null
; RUN: opt -S %t-dump/*.bc | FileCheck --check-prefix=EXTRACTED %s
;
; AOT inlining runs before extraction is finalized. Definitions that survive
; remain available in the embedded bitcode, tagged with a TU-unique AOT symbol.
; The ORC loader later converts every non-active definition to that external
; declaration before creating the IRMaterializationUnit.
;
; Entry functions that carry !ejit.metadata !{!"ejit_entry"} must keep their
; external linkage so the JIT can look them up.

; noinline keeps these calls present after AOT pre-optimization.
define i32 @helper_add(i32 %a, i32 %b) #0 {
  %sum = add i32 %a, %b
  ret i32 %sum
}

; A second helper called by the first helper — transitive internalization.
define i32 @helper_mul(i32 %a, i32 %b) #0 {
  %prod = mul i32 %a, %b
  ret i32 %prod
}

; The entry function — must NOT be internalized.
define i32 @ejit_entry_calc(i32 %x) !ejit.metadata !2 {
  %a = call i32 @helper_add(i32 %x, i32 1)
  %b = call i32 @helper_mul(i32 %a, i32 2)
  ret i32 %b
}

; A declaration (no body) — must NOT be internalized (already a declaration).
declare void @external_lib_func()

; Entry function that calls an external declaration.
define void @ejit_entry_ext() !ejit.metadata !3 {
  call void @external_lib_func()
  ret void
}

; Helpers retain definitions for now and carry unique fallback names.
; EXTRACTED: define i32 @helper_add({{.*}} !ejit.aot_symbol ![[ADD:[0-9]+]]
; EXTRACTED: define i32 @helper_mul({{.*}} !ejit.aot_symbol ![[MUL:[0-9]+]]
; EXTRACTED-DAG: ![[ADD]] = !{!"__ejit_aot_{{[0-9a-f]+}}"}
; EXTRACTED-DAG: ![[MUL]] = !{!"__ejit_aot_{{[0-9a-f]+}}"}

; Entry functions must keep their original linkage (not internal).
; EXTRACTED: define{{.*}} i32 @ejit_entry_calc(
; EXTRACTED-NOT: define internal {{.*}}@ejit_entry_calc
; EXTRACTED: declare{{.*}} void @external_lib_func()
; EXTRACTED: define{{.*}} void @ejit_entry_ext(
; EXTRACTED-NOT: define internal {{.*}}@ejit_entry_ext

!0 = !{!"ejit_entry"}
!1 = !{!0}
!2 = !{!0}
!3 = !{!0}

attributes #0 = { noinline }
