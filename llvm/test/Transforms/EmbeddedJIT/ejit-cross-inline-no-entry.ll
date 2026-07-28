; RUN: opt -passes='default<O2>' -enable-ejit-bitcode -ejit-cross-inline -S %s | FileCheck %s
;
; Cross-inline mode embeds the full module regardless of whether ejit_entry
; functions exist (link-stage processing decides what to extract).

; CHECK: @__ejit_cross_module = internal constant [{{.*}} x i8] {{.*}}, section ".ejit_cross"
; CHECK-NOT: @__ejit_bitcode

define void @not_an_entry() {
  ret void
}
