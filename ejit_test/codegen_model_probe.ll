; Probe the AArch64 code-model choices used by EmbeddedJIT.
;
; This file intentionally keeps external declarations non-dso-local, matching
; EJitOrcEngine::loadBitcodeModule after it prepares a module for JIT codegen.

target datalayout = "E-m:e-p270:32:32-p271:32:32-p272:64:64-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "aarch64_be-unknown-linux-gnu"

@external_value = external global i64, align 8
@external_function_pointer = external global ptr, align 8
@local_table = internal constant [8 x i64] [
  i64 11, i64 23, i64 37, i64 53, i64 71, i64 89, i64 107, i64 127
], align 8

declare i64 @external_helper(i64)

define i64 @probe_external_global(i64 %x) {
entry:
  %v = load i64, ptr @external_value, align 8
  %sum = add i64 %v, %x
  ret i64 %sum
}

define i64 @probe_external_call(i64 %x) {
entry:
  %r = tail call i64 @external_helper(i64 %x)
  ret i64 %r
}

define i64 @probe_function_pointer(i64 %x) {
entry:
  %fp = load ptr, ptr @external_function_pointer, align 8
  %r = tail call i64 %fp(i64 %x)
  ret i64 %r
}

define i64 @probe_local_constant(i64 %x) {
entry:
  %idx = and i64 %x, 7
  %p = getelementptr inbounds [8 x i64], ptr @local_table, i64 0, i64 %idx
  %v = load i64, ptr %p, align 8
  ret i64 %v
}

define i32 @probe_switch(i32 %x) {
entry:
  switch i32 %x, label %default [
    i32 0, label %case0
    i32 1, label %case1
    i32 2, label %case2
    i32 3, label %case3
    i32 4, label %case4
    i32 5, label %case5
  ]

case0:
  ret i32 101
case1:
  ret i32 211
case2:
  ret i32 307
case3:
  ret i32 401
case4:
  ret i32 503
case5:
  ret i32 601
default:
  ret i32 -1
}
