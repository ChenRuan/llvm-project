source_filename = "/home/ruanchen/workspace/llvm-project-15.0.4/easy-jit-llvm15/tests/c_api/config_process_easyjit.c"
target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"
target triple = "aarch64-unknown-linux-gnu"

%struct.KeyInfo = type { i32, i32, i32 }
%struct.GroupConfig = type { i32, i8, i32, [16 x i32] }
%struct.ConfigRecord = type { i32, i8, i32, i32, i8, i8, [16 x i32], [8 x i32], %struct.NestedConfig, [32 x i32] }
%struct.NestedConfig = type { i32, i8, [8 x i32] }

@.str = private unnamed_addr constant [62 x i8] c"============================================================\0A\00", align 1
@.str.1 = private unnamed_addr constant [44 x i8] c"  EasyJIT C Snapshot + Raw Pointer Example\0A\00", align 1
@.str.2 = private unnamed_addr constant [36 x i8] c"  sizeof(ConfigRecord) = %zu bytes\0A\00", align 1
@.str.3 = private unnamed_addr constant [54 x i8] c"  LOOP_COUNT = %d   GROUP_MAX = %d   CONFIG_MAX = %d\0A\00", align 1
@.str.4 = private unnamed_addr constant [63 x i8] c"============================================================\0A\0A\00", align 1
@.str.5 = private unnamed_addr constant [16 x i8] c"EASYJIT_DUMP_IR\00", align 1
@.str.6 = private unnamed_addr constant [23 x i8] c"  IR dump prefix = %s\0A\00", align 1
@g_keys = external dso_local global [12 x %struct.KeyInfo], align 4
@.str.7 = private unnamed_addr constant [12 x i8] c"%s.key%d.ll\00", align 1
@stderr = external global ptr, align 8
@.str.8 = private unnamed_addr constant [35 x i8] c"set_dump_ir failed for key=%d: %s\0A\00", align 1
@g_configs = external dso_local global ptr, align 8
@.str.9 = private unnamed_addr constant [31 x i8] c"compile failed for key=%d: %s\0A\00", align 1
@.str.10 = private unnamed_addr constant [33 x i8] c"get_function_pointer failed: %s\0A\00", align 1
@.str.11 = private unnamed_addr constant [36 x i8] c"Warm-up (JIT all %d keys): %.4f ms\0A\00", align 1
@.str.12 = private unnamed_addr constant [8 x i8] c"sum=%d\0A\00", align 1
@.str.13 = private unnamed_addr constant [24 x i8] c"steady-state: %.6f sec\0A\00", align 1
@.str.14 = private unnamed_addr constant [25 x i8] c"total wall:   %.6f sec\0A\0A\00", align 1
@.str.15 = private unnamed_addr constant [15 x i8] c"malloc failed\0A\00", align 1
@g_groups = external dso_local global [4 x %struct.GroupConfig], align 4

; Function Attrs: nounwind uwtable
define dso_local i32 @process_config_jit(ptr noundef %0) #0 {
  %2 = getelementptr inbounds %struct.ConfigRecord, ptr %0, i32 0, i32 1
  %3 = load i8, ptr %2, align 4, !tbaa !6, !range !13
  %4 = trunc i8 %3 to i1
  br i1 %4, label %5, label %11

5:                                                ; preds = %1
  %6 = getelementptr inbounds %struct.ConfigRecord, ptr %0, i32 0, i32 2
  %7 = load i32, ptr %6, align 4, !tbaa !14
  %8 = getelementptr inbounds %struct.ConfigRecord, ptr %0, i32 0, i32 6
  %9 = load i32, ptr %8, align 4, !tbaa !15
  %10 = add nsw i32 %7, %9
  br label %15

11:                                               ; preds = %1
  %12 = getelementptr inbounds %struct.ConfigRecord, ptr %0, i32 0, i32 3
  %13 = load i32, ptr %12, align 4, !tbaa !16
  %14 = sub nsw i32 0, %13
  br label %15

15:                                               ; preds = %11, %5
  %16 = phi i32 [ %10, %5 ], [ %14, %11 ]
  %17 = getelementptr inbounds %struct.ConfigRecord, ptr %0, i32 0, i32 4
  %18 = load i8, ptr %17, align 4, !tbaa !17, !range !13
  %19 = trunc i8 %18 to i1
  br i1 %19, label %20, label %24

20:                                               ; preds = %15
  %21 = getelementptr inbounds %struct.ConfigRecord, ptr %0, i32 0, i32 7
  %22 = load i32, ptr %21, align 4, !tbaa !15
  %23 = add nsw i32 %16, %22
  br label %28

24:                                               ; preds = %15
  %25 = getelementptr inbounds %struct.ConfigRecord, ptr %0, i32 0, i32 8
  %26 = load i32, ptr %25, align 4, !tbaa !18
  %27 = add nsw i32 %16, %26
  br label %28

28:                                               ; preds = %24, %20
  %29 = phi i32 [ %23, %20 ], [ %27, %24 ]
  %30 = getelementptr inbounds %struct.ConfigRecord, ptr %0, i32 0, i32 8
  %31 = getelementptr inbounds %struct.NestedConfig, ptr %30, i32 0, i32 1
  %32 = load i8, ptr %31, align 4, !tbaa !19, !range !13
  %33 = trunc i8 %32 to i1
  br i1 %33, label %34, label %38

34:                                               ; preds = %28
  %35 = getelementptr inbounds %struct.NestedConfig, ptr %30, i32 0, i32 2
  %36 = load i32, ptr %35, align 4, !tbaa !15
  %37 = add nsw i32 %29, %36
  br label %42

38:                                               ; preds = %28
  %39 = getelementptr inbounds %struct.ConfigRecord, ptr %0, i32 0, i32 9
  %40 = load i32, ptr %39, align 4, !tbaa !15
  %41 = add nsw i32 %29, %40
  br label %42

42:                                               ; preds = %38, %34
  %43 = phi i32 [ %37, %34 ], [ %41, %38 ]
  ret i32 %43
}

; Function Attrs: argmemonly nocallback nofree nosync nounwind willreturn
declare void @llvm.lifetime.start.p0(i64 immarg, ptr nocapture) #1

; Function Attrs: argmemonly nocallback nofree nosync nounwind willreturn
declare void @llvm.lifetime.end.p0(i64 immarg, ptr nocapture) #1

; Function Attrs: nounwind uwtable
define private i32 @main(i32 noundef %0, ptr noundef %1) #0 {
  %3 = alloca [12 x ptr], align 8
  %4 = alloca [12 x ptr], align 8
  %5 = alloca ptr, align 8
  %6 = alloca ptr, align 8
  %7 = alloca ptr, align 8
  %8 = alloca [256 x i8], align 1
  call void @llvm.lifetime.start.p0(i64 96, ptr %3) #11
  call void @llvm.lifetime.start.p0(i64 96, ptr %4) #11
  %9 = call i32 (ptr, ...) @printf(ptr noundef @.str)
  %10 = call i32 (ptr, ...) @printf(ptr noundef @.str.1)
  %11 = call i32 (ptr, ...) @printf(ptr noundef @.str.2, i64 noundef 284)
  %12 = call i32 (ptr, ...) @printf(ptr noundef @.str.3, i32 noundef 200000000, i32 noundef 4, i32 noundef 12)
  %13 = call i32 (ptr, ...) @printf(ptr noundef @.str.4)
  call void @init_configs()
  call void @init_groups()
  call void @prepare_keys()
  %14 = icmp sgt i32 %0, 1
  br i1 %14, label %15, label %24

15:                                               ; preds = %2
  %16 = getelementptr inbounds ptr, ptr %1, i64 1
  %17 = load ptr, ptr %16, align 8, !tbaa !20
  %18 = icmp ne ptr %17, null
  br i1 %18, label %19, label %24

19:                                               ; preds = %15
  %20 = load i8, ptr %17, align 1, !tbaa !22
  %21 = zext i8 %20 to i32
  %22 = icmp ne i32 %21, 0
  br i1 %22, label %23, label %24

23:                                               ; preds = %19
  br label %26

24:                                               ; preds = %19, %15, %2
  %25 = call ptr @getenv(ptr noundef @.str.5) #11
  br label %26

26:                                               ; preds = %24, %23
  %27 = phi ptr [ %17, %23 ], [ %25, %24 ]
  %28 = icmp ne ptr %27, null
  br i1 %28, label %29, label %35

29:                                               ; preds = %26
  %30 = load i8, ptr %27, align 1, !tbaa !22
  %31 = zext i8 %30 to i32
  %32 = icmp ne i32 %31, 0
  br i1 %32, label %33, label %35

33:                                               ; preds = %29
  %34 = call i32 (ptr, ...) @printf(ptr noundef @.str.6, ptr noundef %27)
  br label %35

35:                                               ; preds = %33, %29, %26
  %36 = call i64 @clock() #11
  br label %37

37:                                               ; preds = %105, %35
  %38 = phi i32 [ 0, %35 ], [ %106, %105 ]
  %39 = phi i32 [ 0, %35 ], [ %103, %105 ]
  %40 = icmp slt i32 %38, 12
  br i1 %40, label %42, label %41

41:                                               ; preds = %37
  br label %107

42:                                               ; preds = %37
  call void @llvm.lifetime.start.p0(i64 8, ptr %5) #11
  store ptr null, ptr %5, align 8, !tbaa !20
  call void @llvm.lifetime.start.p0(i64 8, ptr %6) #11
  store ptr null, ptr %6, align 8, !tbaa !20
  call void @llvm.lifetime.start.p0(i64 8, ptr %7) #11
  store ptr null, ptr %7, align 8, !tbaa !20
  call void @llvm.lifetime.start.p0(i64 256, ptr %8) #11
  %43 = sext i32 %38 to i64
  %44 = getelementptr inbounds [12 x %struct.KeyInfo], ptr @g_keys, i64 0, i64 %43
  %45 = load i32, ptr %44, align 4, !tbaa !23
  %46 = getelementptr inbounds %struct.KeyInfo, ptr %44, i32 0, i32 1
  %47 = load i32, ptr %46, align 4, !tbaa !25
  call void @update_config(i32 noundef %45, i32 noundef %47)
  %48 = load i32, ptr %44, align 4, !tbaa !23
  %49 = call ptr @get_config(i32 noundef %48)
  %50 = call i32 @easyjit_context_create(ptr noundef %5)
  %51 = load ptr, ptr %5, align 8, !tbaa !20
  %52 = call i32 @easyjit_context_set_snapshot(ptr noundef %51, ptr noundef %49, i64 noundef 284)
  %53 = load ptr, ptr %5, align 8, !tbaa !20
  %54 = call i32 @easyjit_context_set_opt_level(ptr noundef %53, i32 noundef 3, i32 noundef 0)
  br i1 %28, label %55, label %73

55:                                               ; preds = %42
  %56 = load i8, ptr %27, align 1, !tbaa !22
  %57 = zext i8 %56 to i32
  %58 = icmp ne i32 %57, 0
  br i1 %58, label %59, label %73

59:                                               ; preds = %55
  %60 = getelementptr inbounds %struct.KeyInfo, ptr %44, i32 0, i32 2
  %61 = load i32, ptr %60, align 4, !tbaa !26
  %62 = call i32 (ptr, i64, ptr, ...) @snprintf(ptr noundef %8, i64 noundef 256, ptr noundef @.str.7, ptr noundef %27, i32 noundef %61) #11
  %63 = load ptr, ptr %5, align 8, !tbaa !20
  %64 = call i32 @easyjit_context_set_dump_ir(ptr noundef %63, ptr noundef %8)
  %65 = icmp ne i32 %64, 0
  br i1 %65, label %66, label %73

66:                                               ; preds = %59
  %67 = load ptr, ptr @stderr, align 8, !tbaa !20
  %68 = load i32, ptr %60, align 4, !tbaa !26
  %69 = call ptr @easyjit_get_last_error()
  %70 = call i32 (ptr, ptr, ...) @fprintf(ptr noundef %67, ptr noundef @.str.8, i32 noundef %68, ptr noundef %69)
  %71 = load ptr, ptr %5, align 8, !tbaa !20
  call void @easyjit_context_destroy(ptr noundef %71)
  %72 = load ptr, ptr @g_configs, align 8, !tbaa !20
  call void @free(ptr noundef %72) #11
  br label %101

73:                                               ; preds = %59, %55, %42
  %74 = load ptr, ptr %5, align 8, !tbaa !20
  %75 = call i32 @easyjit_compile(ptr noundef @process_config_jit, ptr noundef %74, ptr noundef %6)
  %76 = icmp ne i32 %75, 0
  br i1 %76, label %77, label %85

77:                                               ; preds = %73
  %78 = load ptr, ptr @stderr, align 8, !tbaa !20
  %79 = getelementptr inbounds %struct.KeyInfo, ptr %44, i32 0, i32 2
  %80 = load i32, ptr %79, align 4, !tbaa !26
  %81 = call ptr @easyjit_get_last_error()
  %82 = call i32 (ptr, ptr, ...) @fprintf(ptr noundef %78, ptr noundef @.str.9, i32 noundef %80, ptr noundef %81)
  %83 = load ptr, ptr %5, align 8, !tbaa !20
  call void @easyjit_context_destroy(ptr noundef %83)
  %84 = load ptr, ptr @g_configs, align 8, !tbaa !20
  call void @free(ptr noundef %84) #11
  br label %101

85:                                               ; preds = %73
  %86 = load ptr, ptr %5, align 8, !tbaa !20
  call void @easyjit_context_destroy(ptr noundef %86)
  %87 = load ptr, ptr %6, align 8, !tbaa !20
  %88 = call i32 @easyjit_get_function_pointer(ptr noundef %87, ptr noundef %7)
  %89 = icmp ne i32 %88, 0
  br i1 %89, label %90, label %96

90:                                               ; preds = %85
  %91 = load ptr, ptr @stderr, align 8, !tbaa !20
  %92 = call ptr @easyjit_get_last_error()
  %93 = call i32 (ptr, ptr, ...) @fprintf(ptr noundef %91, ptr noundef @.str.10, ptr noundef %92)
  %94 = load ptr, ptr %6, align 8, !tbaa !20
  call void @easyjit_function_destroy(ptr noundef %94)
  %95 = load ptr, ptr @g_configs, align 8, !tbaa !20
  call void @free(ptr noundef %95) #11
  br label %101

96:                                               ; preds = %85
  %97 = load ptr, ptr %6, align 8, !tbaa !20
  %98 = getelementptr inbounds [12 x ptr], ptr %3, i64 0, i64 %43
  store ptr %97, ptr %98, align 8, !tbaa !20
  %99 = load ptr, ptr %7, align 8, !tbaa !20
  %100 = getelementptr inbounds [12 x ptr], ptr %4, i64 0, i64 %43
  store ptr %99, ptr %100, align 8, !tbaa !20
  br label %101

101:                                              ; preds = %96, %90, %77, %66
  %102 = phi i32 [ 1, %66 ], [ 1, %77 ], [ 1, %90 ], [ 0, %96 ]
  %103 = phi i32 [ 1, %66 ], [ 1, %77 ], [ 1, %90 ], [ %39, %96 ]
  call void @llvm.lifetime.end.p0(i64 256, ptr %8) #11
  call void @llvm.lifetime.end.p0(i64 8, ptr %7) #11
  call void @llvm.lifetime.end.p0(i64 8, ptr %6) #11
  call void @llvm.lifetime.end.p0(i64 8, ptr %5) #11
  %104 = icmp eq i32 %102, 0
  br i1 %104, label %105, label %107

105:                                              ; preds = %101
  %106 = add nsw i32 %38, 1
  br label %37, !llvm.loop !27

107:                                              ; preds = %101, %41
  %108 = phi i32 [ %102, %101 ], [ 2, %41 ]
  %109 = phi i32 [ %103, %101 ], [ %39, %41 ]
  %110 = icmp eq i32 %108, 2
  br i1 %110, label %111, label %153

111:                                              ; preds = %107
  %112 = call i64 @clock() #11
  %113 = sub nsw i64 %112, %36
  %114 = sitofp i64 %113 to double
  %115 = fmul double %114, 1.000000e+03
  %116 = fdiv double %115, 1.000000e+06
  %117 = call i32 (ptr, ...) @printf(ptr noundef @.str.11, i32 noundef 12, double noundef %116)
  %118 = call i64 @clock() #11
  br label %119

119:                                              ; preds = %134, %111
  %120 = phi i32 [ 0, %111 ], [ %141, %134 ]
  %121 = phi i32 [ 0, %111 ], [ %142, %134 ]
  %122 = icmp slt i32 %121, 200000000
  br i1 %122, label %134, label %123

123:                                              ; preds = %119
  %124 = call i64 @clock() #11
  %125 = call i32 (ptr, ...) @printf(ptr noundef @.str.12, i32 noundef %120)
  %126 = sub nsw i64 %124, %118
  %127 = sitofp i64 %126 to double
  %128 = fdiv double %127, 1.000000e+06
  %129 = call i32 (ptr, ...) @printf(ptr noundef @.str.13, double noundef %128)
  %130 = sub nsw i64 %124, %36
  %131 = sitofp i64 %130 to double
  %132 = fdiv double %131, 1.000000e+06
  %133 = call i32 (ptr, ...) @printf(ptr noundef @.str.14, double noundef %132)
  br label %143

134:                                              ; preds = %119
  %135 = srem i32 %121, 12
  %136 = srem i32 %121, 4
  call void @update_config(i32 noundef %135, i32 noundef %136)
  %137 = sext i32 %135 to i64
  %138 = getelementptr inbounds [12 x ptr], ptr %4, i64 0, i64 %137
  %139 = load ptr, ptr %138, align 8, !tbaa !20
  %140 = call i32 %139()
  %141 = add nsw i32 %120, %140
  %142 = add nsw i32 %121, 1
  br label %119, !llvm.loop !29

143:                                              ; preds = %148, %123
  %144 = phi i32 [ 0, %123 ], [ %152, %148 ]
  %145 = icmp slt i32 %144, 12
  br i1 %145, label %148, label %146

146:                                              ; preds = %143
  %147 = load ptr, ptr @g_configs, align 8, !tbaa !20
  call void @free(ptr noundef %147) #11
  br label %153

148:                                              ; preds = %143
  %149 = sext i32 %144 to i64
  %150 = getelementptr inbounds [12 x ptr], ptr %3, i64 0, i64 %149
  %151 = load ptr, ptr %150, align 8, !tbaa !20
  call void @easyjit_function_destroy(ptr noundef %151)
  %152 = add nsw i32 %144, 1
  br label %143, !llvm.loop !30

153:                                              ; preds = %146, %107
  %154 = phi i32 [ 0, %146 ], [ %109, %107 ]
  call void @llvm.lifetime.end.p0(i64 96, ptr %4) #11
  call void @llvm.lifetime.end.p0(i64 96, ptr %3) #11
  ret i32 %154
}

; Function Attrs: nofree nounwind
declare noundef i32 @printf(ptr nocapture noundef readonly, ...) #2

; Function Attrs: nounwind uwtable
define private void @init_configs() #0 {
  %1 = call noalias ptr @malloc(i64 noundef 3408) #12
  store ptr %1, ptr @g_configs, align 8, !tbaa !20
  %2 = icmp ne ptr %1, null
  br i1 %2, label %6, label %3

3:                                                ; preds = %0
  %4 = load ptr, ptr @stderr, align 8, !tbaa !20
  %5 = call i32 (ptr, ptr, ...) @fprintf(ptr noundef %4, ptr noundef @.str.15)
  call void @exit(i32 noundef 1) #13
  unreachable

6:                                                ; preds = %0
  call void @llvm.memset.p0.i64(ptr align 4 %1, i8 0, i64 3408, i1 false)
  br label %7

7:                                                ; preds = %11, %6
  %8 = phi i32 [ 0, %6 ], [ %26, %11 ]
  %9 = icmp slt i32 %8, 12
  br i1 %9, label %11, label %10

10:                                               ; preds = %7
  ret void

11:                                               ; preds = %7
  %12 = load ptr, ptr @g_configs, align 8, !tbaa !20
  %13 = sext i32 %8 to i64
  %14 = getelementptr inbounds %struct.ConfigRecord, ptr %12, i64 %13
  store i32 %8, ptr %14, align 4, !tbaa !31
  %15 = load ptr, ptr @g_configs, align 8, !tbaa !20
  %16 = getelementptr inbounds %struct.ConfigRecord, ptr %15, i64 %13
  %17 = getelementptr inbounds %struct.ConfigRecord, ptr %16, i32 0, i32 1
  store i8 1, ptr %17, align 4, !tbaa !6
  %18 = srem i32 %8, 3
  %19 = load ptr, ptr @g_configs, align 8, !tbaa !20
  %20 = getelementptr inbounds %struct.ConfigRecord, ptr %19, i64 %13
  %21 = getelementptr inbounds %struct.ConfigRecord, ptr %20, i32 0, i32 2
  store i32 %18, ptr %21, align 4, !tbaa !14
  %22 = mul nsw i32 %8, 2
  %23 = load ptr, ptr @g_configs, align 8, !tbaa !20
  %24 = getelementptr inbounds %struct.ConfigRecord, ptr %23, i64 %13
  %25 = getelementptr inbounds %struct.ConfigRecord, ptr %24, i32 0, i32 3
  store i32 %22, ptr %25, align 4, !tbaa !16
  %26 = add nsw i32 %8, 1
  br label %7, !llvm.loop !32
}

; Function Attrs: nounwind uwtable
define private void @init_groups() #0 {
  br label %1

1:                                                ; preds = %19, %0
  %2 = phi i32 [ 0, %0 ], [ %20, %19 ]
  %3 = icmp slt i32 %2, 4
  br i1 %3, label %5, label %4

4:                                                ; preds = %1
  ret void

5:                                                ; preds = %1
  %6 = mul nsw i32 %2, 10
  %7 = add nsw i32 100, %6
  %8 = sext i32 %2 to i64
  %9 = getelementptr inbounds [4 x %struct.GroupConfig], ptr @g_groups, i64 0, i64 %8
  store i32 %7, ptr %9, align 4, !tbaa !33
  %10 = srem i32 %2, 2
  %11 = icmp eq i32 %10, 0
  %12 = getelementptr inbounds %struct.GroupConfig, ptr %9, i32 0, i32 1
  %13 = zext i1 %11 to i8
  store i8 %13, ptr %12, align 4, !tbaa !35
  %14 = mul nsw i32 %2, 5
  %15 = getelementptr inbounds %struct.GroupConfig, ptr %9, i32 0, i32 2
  store i32 %14, ptr %15, align 4, !tbaa !36
  br label %16

16:                                               ; preds = %21, %5
  %17 = phi i32 [ 0, %5 ], [ %26, %21 ]
  %18 = icmp slt i32 %17, 16
  br i1 %18, label %21, label %19

19:                                               ; preds = %16
  %20 = add nsw i32 %2, 1
  br label %1, !llvm.loop !37

21:                                               ; preds = %16
  %22 = mul nsw i32 %2, %17
  %23 = getelementptr inbounds %struct.GroupConfig, ptr %9, i32 0, i32 3
  %24 = sext i32 %17 to i64
  %25 = getelementptr inbounds [16 x i32], ptr %23, i64 0, i64 %24
  store i32 %22, ptr %25, align 4, !tbaa !15
  %26 = add nsw i32 %17, 1
  br label %16, !llvm.loop !38
}

; Function Attrs: nounwind uwtable
define private void @prepare_keys() #0 {
  br label %1

1:                                                ; preds = %5, %0
  %2 = phi i32 [ 0, %0 ], [ %13, %5 ]
  %3 = icmp slt i32 %2, 12
  br i1 %3, label %5, label %4

4:                                                ; preds = %1
  ret void

5:                                                ; preds = %1
  %6 = sext i32 %2 to i64
  %7 = getelementptr inbounds [12 x %struct.KeyInfo], ptr @g_keys, i64 0, i64 %6
  store i32 %2, ptr %7, align 4, !tbaa !23
  %8 = srem i32 %2, 4
  %9 = getelementptr inbounds %struct.KeyInfo, ptr %7, i32 0, i32 1
  store i32 %8, ptr %9, align 4, !tbaa !25
  %10 = mul nsw i32 %2, 13
  %11 = add nsw i32 %10, %8
  %12 = getelementptr inbounds %struct.KeyInfo, ptr %7, i32 0, i32 2
  store i32 %11, ptr %12, align 4, !tbaa !26
  %13 = add nsw i32 %2, 1
  br label %1, !llvm.loop !39
}

; Function Attrs: nofree nounwind readonly
declare noundef ptr @getenv(ptr nocapture noundef) #3

; Function Attrs: nounwind
declare i64 @clock() #4

; Function Attrs: nounwind uwtable
define private void @update_config(i32 noundef %0, i32 noundef %1) #0 {
  %3 = call ptr @get_config(i32 noundef %0)
  %4 = sext i32 %1 to i64
  %5 = getelementptr inbounds [4 x %struct.GroupConfig], ptr @g_groups, i64 0, i64 %4
  %6 = load i32, ptr %5, align 4, !tbaa !33
  %7 = getelementptr inbounds %struct.ConfigRecord, ptr %3, i32 0, i32 2
  store i32 %6, ptr %7, align 4, !tbaa !14
  %8 = getelementptr inbounds %struct.GroupConfig, ptr %5, i32 0, i32 1
  %9 = load i8, ptr %8, align 4, !tbaa !35, !range !13
  %10 = trunc i8 %9 to i1
  %11 = getelementptr inbounds %struct.ConfigRecord, ptr %3, i32 0, i32 1
  %12 = zext i1 %10 to i8
  store i8 %12, ptr %11, align 4, !tbaa !6
  %13 = getelementptr inbounds %struct.GroupConfig, ptr %5, i32 0, i32 2
  %14 = load i32, ptr %13, align 4, !tbaa !36
  %15 = getelementptr inbounds %struct.ConfigRecord, ptr %3, i32 0, i32 3
  store i32 %14, ptr %15, align 4, !tbaa !16
  %16 = getelementptr inbounds %struct.GroupConfig, ptr %5, i32 0, i32 3
  %17 = load i32, ptr %16, align 4, !tbaa !15
  %18 = getelementptr inbounds %struct.ConfigRecord, ptr %3, i32 0, i32 6
  store i32 %17, ptr %18, align 4, !tbaa !15
  %19 = getelementptr inbounds [16 x i32], ptr %16, i64 0, i64 1
  %20 = load i32, ptr %19, align 4, !tbaa !15
  %21 = getelementptr inbounds [16 x i32], ptr %18, i64 0, i64 1
  store i32 %20, ptr %21, align 4, !tbaa !15
  %22 = getelementptr inbounds [16 x i32], ptr %16, i64 0, i64 2
  %23 = load i32, ptr %22, align 4, !tbaa !15
  %24 = getelementptr inbounds [16 x i32], ptr %18, i64 0, i64 2
  store i32 %23, ptr %24, align 4, !tbaa !15
  %25 = getelementptr inbounds [16 x i32], ptr %16, i64 0, i64 3
  %26 = load i32, ptr %25, align 4, !tbaa !15
  %27 = getelementptr inbounds [16 x i32], ptr %18, i64 0, i64 3
  store i32 %26, ptr %27, align 4, !tbaa !15
  %28 = getelementptr inbounds [16 x i32], ptr %16, i64 0, i64 4
  %29 = load i32, ptr %28, align 4, !tbaa !15
  %30 = getelementptr inbounds %struct.ConfigRecord, ptr %3, i32 0, i32 8
  store i32 %29, ptr %30, align 4, !tbaa !18
  %31 = getelementptr inbounds [16 x i32], ptr %16, i64 0, i64 5
  %32 = load i32, ptr %31, align 4, !tbaa !15
  %33 = srem i32 %32, 2
  %34 = icmp ne i32 %33, 0
  %35 = getelementptr inbounds %struct.NestedConfig, ptr %30, i32 0, i32 1
  %36 = zext i1 %34 to i8
  store i8 %36, ptr %35, align 4, !tbaa !19
  %37 = getelementptr inbounds [16 x i32], ptr %16, i64 0, i64 6
  %38 = load i32, ptr %37, align 4, !tbaa !15
  %39 = getelementptr inbounds %struct.NestedConfig, ptr %30, i32 0, i32 2
  store i32 %38, ptr %39, align 4, !tbaa !15
  ret void
}

; Function Attrs: inlinehint nounwind uwtable
define private ptr @get_config(i32 noundef %0) #5 {
  %2 = load ptr, ptr @g_configs, align 8, !tbaa !20
  %3 = sext i32 %0 to i64
  %4 = getelementptr inbounds %struct.ConfigRecord, ptr %2, i64 %3
  ret ptr %4
}

declare i32 @easyjit_context_create(ptr noundef) #6

declare i32 @easyjit_context_set_snapshot(ptr noundef, ptr noundef, i64 noundef) #6

declare i32 @easyjit_context_set_opt_level(ptr noundef, i32 noundef, i32 noundef) #6

; Function Attrs: nofree nounwind
declare noundef i32 @snprintf(ptr noalias nocapture noundef writeonly, i64 noundef, ptr nocapture noundef readonly, ...) #2

declare i32 @easyjit_context_set_dump_ir(ptr noundef, ptr noundef) #6

; Function Attrs: nofree nounwind
declare noundef i32 @fprintf(ptr nocapture noundef, ptr nocapture noundef readonly, ...) #2

declare ptr @easyjit_get_last_error() #6

declare void @easyjit_context_destroy(ptr noundef) #6

; Function Attrs: inaccessiblemem_or_argmemonly mustprogress nounwind willreturn allockind("free")
declare void @free(ptr allocptr nocapture noundef) #7

declare i32 @easyjit_compile(ptr noundef, ptr noundef, ptr noundef) #6

declare i32 @easyjit_get_function_pointer(ptr noundef, ptr noundef) #6

declare void @easyjit_function_destroy(ptr noundef) #6

; Function Attrs: inaccessiblememonly mustprogress nofree nounwind willreturn allockind("alloc,uninitialized") allocsize(0)
declare noalias noundef ptr @malloc(i64 noundef) #8

; Function Attrs: noreturn nounwind
declare void @exit(i32 noundef) #9

; Function Attrs: argmemonly nocallback nofree nounwind willreturn writeonly
declare void @llvm.memset.p0.i64(ptr nocapture writeonly, i8, i64, i1 immarg) #10

attributes #0 = { nounwind uwtable "frame-pointer"="non-leaf" "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="generic" "target-features"="+neon,+outline-atomics,+v8a" }
attributes #1 = { argmemonly nocallback nofree nosync nounwind willreturn }
attributes #2 = { nofree nounwind "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="generic" "target-features"="+neon,+outline-atomics,+v8a" }
attributes #3 = { nofree nounwind readonly "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="generic" "target-features"="+neon,+outline-atomics,+v8a" }
attributes #4 = { nounwind "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="generic" "target-features"="+neon,+outline-atomics,+v8a" }
attributes #5 = { inlinehint nounwind uwtable "frame-pointer"="non-leaf" "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="generic" "target-features"="+neon,+outline-atomics,+v8a" }
attributes #6 = { "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="generic" "target-features"="+neon,+outline-atomics,+v8a" }
attributes #7 = { inaccessiblemem_or_argmemonly mustprogress nounwind willreturn allockind("free") "alloc-family"="malloc" "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="generic" "target-features"="+neon,+outline-atomics,+v8a" }
attributes #8 = { inaccessiblememonly mustprogress nofree nounwind willreturn allockind("alloc,uninitialized") allocsize(0) "alloc-family"="malloc" "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="generic" "target-features"="+neon,+outline-atomics,+v8a" }
attributes #9 = { noreturn nounwind "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="generic" "target-features"="+neon,+outline-atomics,+v8a" }
attributes #10 = { argmemonly nocallback nofree nounwind willreturn writeonly }
attributes #11 = { nounwind }
attributes #12 = { nounwind allocsize(0) }
attributes #13 = { noreturn nounwind }

!llvm.module.flags = !{!0, !1, !2, !3, !4}
!llvm.ident = !{!5}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 7, !"PIC Level", i32 2}
!2 = !{i32 7, !"PIE Level", i32 2}
!3 = !{i32 7, !"uwtable", i32 2}
!4 = !{i32 7, !"frame-pointer", i32 1}
!5 = !{!"clang version 15.0.7 (https://github.com/llvm/llvm-project.git 8dfdcc7b7bf66834a761bd8de445840ef68e4d1a)"}
!6 = !{!7, !11, i64 4}
!7 = !{!"", !8, i64 0, !11, i64 4, !8, i64 8, !8, i64 12, !11, i64 16, !11, i64 17, !9, i64 20, !9, i64 84, !12, i64 116, !9, i64 156}
!8 = !{!"int", !9, i64 0}
!9 = !{!"omnipotent char", !10, i64 0}
!10 = !{!"Simple C/C++ TBAA"}
!11 = !{!"_Bool", !9, i64 0}
!12 = !{!"", !8, i64 0, !11, i64 4, !9, i64 8}
!13 = !{i8 0, i8 2}
!14 = !{!7, !8, i64 8}
!15 = !{!8, !8, i64 0}
!16 = !{!7, !8, i64 12}
!17 = !{!7, !11, i64 16}
!18 = !{!7, !8, i64 116}
!19 = !{!7, !11, i64 120}
!20 = !{!21, !21, i64 0}
!21 = !{!"any pointer", !9, i64 0}
!22 = !{!9, !9, i64 0}
!23 = !{!24, !8, i64 0}
!24 = !{!"", !8, i64 0, !8, i64 4, !8, i64 8}
!25 = !{!24, !8, i64 4}
!26 = !{!24, !8, i64 8}
!27 = distinct !{!27, !28}
!28 = !{!"llvm.loop.mustprogress"}
!29 = distinct !{!29, !28}
!30 = distinct !{!30, !28}
!31 = !{!7, !8, i64 0}
!32 = distinct !{!32, !28}
!33 = !{!34, !8, i64 0}
!34 = !{!"", !8, i64 0, !11, i64 4, !8, i64 8, !9, i64 12}
!35 = !{!34, !11, i64 4}
!36 = !{!34, !8, i64 8}
!37 = distinct !{!37, !28}
!38 = distinct !{!38, !28}
!39 = distinct !{!39, !28}
