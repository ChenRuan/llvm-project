source_filename = "/home/ruanchen/workspace/llvm-project-15.0.4/easy-jit-llvm15/tests/c_api/partial_struct_binding.c"
target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "aarch64-unknown-linux-gnu"

%struct.PartialConfig = type { i32, i32 }

; Function Attrs: argmemonly nocallback nofree nounwind willreturn
declare void @llvm.memcpy.p0.p0.i64(ptr noalias nocapture writeonly, ptr noalias nocapture readonly, i64, i1 immarg) #0

; Function Attrs: nounwind uwtable
define i32 @eval_partial_config(ptr noundef %0, i32 noundef %1) #1 {
  %3 = alloca %struct.PartialConfig, align 8
  call void @llvm.memcpy.p0.p0.i64(ptr align 8 %3, ptr align 8 %0, i64 8, i1 false)
  %bound.field.gep = getelementptr %struct.PartialConfig, ptr %3, i32 0, i32 1
  store i32 7, ptr %bound.field.gep, align 4
  %4 = getelementptr inbounds %struct.PartialConfig, ptr %3, i32 0, i32 1
  %5 = load i32, ptr %4, align 4, !tbaa !7
  %6 = icmp ne i32 %5, 0
  br i1 %6, label %7, label %11

7:                                                ; preds = %2
  %8 = load i32, ptr %3, align 4, !tbaa !12
  %9 = add nsw i32 %1, %8
  %10 = add nsw i32 %9, %5
  br label %.exit

11:                                               ; preds = %2
  %12 = load i32, ptr %3, align 4, !tbaa !12
  %13 = sub nsw i32 %1, %12
  br label %.exit

.exit:                                            ; preds = %7, %11
  %14 = phi i32 [ %10, %7 ], [ %13, %11 ]
  ret i32 %14
}

attributes #0 = { argmemonly nocallback nofree nounwind willreturn }
attributes #1 = { nounwind uwtable "frame-pointer"="non-leaf" "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="generic" "target-features"="+neon,+outline-atomics,+v8a" }

!llvm.module.flags = !{!0, !1, !2, !3, !4}
!llvm.ident = !{!5}
!easy\3A\3Ajit = !{!6}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 7, !"PIC Level", i32 2}
!2 = !{i32 7, !"PIE Level", i32 2}
!3 = !{i32 7, !"uwtable", i32 2}
!4 = !{i32 7, !"frame-pointer", i32 1}
!5 = !{!"clang version 15.0.7 (https://github.com/llvm/llvm-project.git 8dfdcc7b7bf66834a761bd8de445840ef68e4d1a)"}
!6 = !{!"entry", !"eval_partial_config"}
!7 = !{!8, !9, i64 4}
!8 = !{!"", !9, i64 0, !9, i64 4}
!9 = !{!"int", !10, i64 0}
!10 = !{!"omnipotent char", !11, i64 0}
!11 = !{!"Simple C/C++ TBAA"}
!12 = !{!8, !9, i64 0}
