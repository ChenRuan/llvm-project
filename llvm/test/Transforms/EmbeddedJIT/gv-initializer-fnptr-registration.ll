; RUN: opt -passes=ejit-register-bitcode -S %s | FileCheck %s
;
; Test: When a function pointer is stored in a GlobalVariable's
; initializer (e.g., a constant struct array of fn_ptr entries), and the
; ejit_entry function loads the pointer and calls it indirectly, the
; function declaration must be registered via ejit_register_symbol so
; the JIT runtime can find it in userSymbols.
;
; Without this fix, CI->getCalledFunction() returns nullptr for indirect
; calls and the symbol is never registered, causing JIT link failures.

; --- Simple case: constant ptr holding a function pointer ---

declare i32 @indirect_target(i32)

@fn_ptr_storage = constant ptr @indirect_target

define i32 @ejit_entry_simple(i32 %x) !ejit.metadata !0 {
; CHECK-LABEL: define i32 @ejit_entry_simple
  %fp = load ptr, ptr @fn_ptr_storage
  %r = call i32 %fp(i32 %x)
  ret i32 %r
}

; --- Struct array case: table of {fn_ptr, int} entries ---

declare i32 @callback_a(i32)
declare i32 @callback_b(i32)
declare i32 @callback_unused(i32)

%struct.callback_entry = type { ptr, i32 }

@callback_table = constant [2 x %struct.callback_entry] [
  %struct.callback_entry { ptr @callback_a, i32 100 },
  %struct.callback_entry { ptr @callback_b, i32 200 }
]

define i32 @ejit_entry_table(i32 %idx) !ejit.metadata !1 {
; CHECK-LABEL: define i32 @ejit_entry_table
entry:
  %ext = zext i32 %idx to i64
  %gep = getelementptr inbounds [2 x %struct.callback_entry], ptr @callback_table, i64 0, i64 %ext
  %fn_ptr_field = getelementptr inbounds %struct.callback_entry, ptr %gep, i32 0, i32 0
  %arg_field = getelementptr inbounds %struct.callback_entry, ptr %gep, i32 0, i32 1
  %fn_ptr = load ptr, ptr %fn_ptr_field
  %arg_val = load i32, ptr %arg_field
  %result = call i32 %fn_ptr(i32 %arg_val)
  ret i32 %result
}

; The auto-register function must emit ejit_register_symbol for every
; external function declaration reachable from the closure, including
; those only referenced through GV initializers.
;
; CHECK: define internal void @ejit_auto_register()
; CHECK-DAG: call void @ejit_register_symbol({{.*}}@indirect_target)
; CHECK-DAG: call void @ejit_register_symbol({{.*}}@callback_a)
; CHECK-DAG: call void @ejit_register_symbol({{.*}}@callback_b)
; CHECK-NOT: @callback_unused

!0 = distinct !{!{!"ejit_entry"}}
!1 = distinct !{!{!"ejit_entry"}}
