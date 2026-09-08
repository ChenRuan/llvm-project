; RUN: rm -rf %t-dump && mkdir -p %t-dump
; RUN: opt -passes=ejit-register-bitcode -ejit-dump-bitcode-dir=%t-dump -S <%s >/dev/null
; RUN: opt -S %t-dump/*.bc | FileCheck %s
;
; Extraction must restore may_const on a whole-scalar load from the dedicated
; one-operand marker. It must not bless volatile or differently-sized loads.

@g_after_init = global i32 0, !ejit.metadata !2

define i32 @entry() !ejit.metadata !5 {
entry:
  %whole = load i32, ptr @g_after_init
  %narrow = load i16, ptr @g_after_init
  %narrow.ext = zext i16 %narrow to i32
  %volatile = load volatile i32, ptr @g_after_init
  %sum0 = add i32 %whole, %narrow.ext
  %sum1 = add i32 %sum0, %volatile
  ret i32 %sum1
}

; CHECK: define i32 @entry()
; CHECK: load i32, ptr @g_after_init, {{.*}}!ejit.may_const ![[EMPTY:[0-9]+]]
; CHECK: load i16, ptr @g_after_init, align 2{{$}}
; CHECK: load volatile i32, ptr @g_after_init, align 4{{$}}
; CHECK: ![[EMPTY]] = !{}

!0 = !{!"ejit_const_after_init"}
!1 = !{!"ejit_period", !"static"}
!2 = distinct !{!0, !1}
!4 = !{!"ejit_entry"}
!5 = !{!4}
