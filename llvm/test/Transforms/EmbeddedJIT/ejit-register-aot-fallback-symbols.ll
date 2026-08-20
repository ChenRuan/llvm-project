; RUN: opt -passes=ejit-register-bitcode -S %s | FileCheck %s
;
; Every definition that survives AOT inlining is registered under the same
; TU-unique name stored in the embedded bitcode. This includes static helpers
; and other entries, allowing the JIT loader to externalize every non-active
; body without name collisions across translation units.

; CHECK-DAG: c"__ejit_aot_{{[0-9a-f]+}}\00"
; CHECK-DAG: call void @ejit_register_symbol(ptr {{.*}}, ptr @static_helper)
; CHECK-DAG: call void @ejit_register_symbol(ptr {{.*}}, ptr @__ejit_aot_{{[0-9a-f]+}}.body)
; CHECK: define internal i32 @__ejit_aot_{{[0-9a-f]+}}.body(i32 %x)

define internal i32 @static_helper(i32 %x) #0 {
  %r = mul i32 %x, 3
  ret i32 %r
}

define i32 @function_entry(i32 %x) #0 !ejit.metadata !0 {
  %r = call i32 @static_helper(i32 %x)
  ret i32 %r
}

attributes #0 = { noinline }
!0 = distinct !{!{!"ejit_entry"}}
