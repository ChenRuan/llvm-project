source_filename = "/home/ruanchen/workspace/llvm-project-15.0.4/easy-jit-llvm15/tests/c_api/global_partial_snapshot.c"
target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "aarch64-unknown-linux-gnu"

%struct.PartialGlobalConfig = type { i32, i32 }

@g_partial_cfg = external dso_local global %struct.PartialGlobalConfig, align 4

; Function Attrs: nounwind uwtable
define i32 @eval_global_partial_c(i32 noundef %0) #0 {
  %2 = load i32, ptr @g_partial_cfg, align 4, !tbaa !7
  %3 = icmp ne i32 %2, 0
  %4 = load i32, ptr getelementptr inbounds (%struct.PartialGlobalConfig, ptr @g_partial_cfg, i32 0, i32 1), align 4
  %5 = add nsw i32 %0, %4
  %6 = sub nsw i32 %0, 99
  %7 = select i1 %3, i32 %5, i32 %6
  ret i32 %7
}

attributes #0 = { nounwind uwtable "frame-pointer"="non-leaf" "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="generic" "target-features"="+neon,+outline-atomics,+v8a" }

!llvm.module.flags = !{!0, !1, !2, !3, !4}
!llvm.ident = !{!5}
!easy\3A\3Ajit = !{!6}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 7, !"PIC Level", i32 2}
!2 = !{i32 7, !"PIE Level", i32 2}
!3 = !{i32 7, !"uwtable", i32 2}
!4 = !{i32 7, !"frame-pointer", i32 1}
!5 = !{!"clang version 15.0.7 (https://github.com/llvm/llvm-project.git 8dfdcc7b7bf66834a761bd8de445840ef68e4d1a)"}
!6 = !{!"entry", !"eval_global_partial_c"}
!7 = !{!8, !9, i64 0}
!8 = !{!"", !9, i64 0, !9, i64 4}
!9 = !{!"int", !10, i64 0}
!10 = !{!"omnipotent char", !11, i64 0}
!11 = !{!"Simple C/C++ TBAA"}
