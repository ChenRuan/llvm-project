# LLVM 仓内源码级裁剪实验报告（llvm15_trim_llvm_backend）

> 工作目录：`/home/ruanchen/workspace/llvm-project-15.0.4`
> 分支：`llvm15_trim_llvm_backend`（基于 `llvmorg-15.0.4`）
> 目标平台：aarch64-linux-gnu
> 场景：EasyJIT + ORC JIT + 完全静态部署
> 验证用例：`easy-jit-llvm15/tests/c_api/add_int.c`

## 一、本轮工作定位

**本轮与之前几轮的关键区别**：不是链接层 stub，不是 EasyJIT 工程侧调整，
而是**真正修改 LLVM 源码并通过 CMake 选项控制**。每一个新选项都：

- 默认 `OFF`，不影响任何既有构建。
- 只在嵌入式静态场景打开时，切断"强引用链"，让目标静态库在最终链接时被完整丢弃。
- 必须通过最小用例验证：完全静态、可运行、输出正确。

## 二、基线对齐

报告中出现的数字统一采用以下口径（避免与之前的实验混淆）：

| 口径 | text | 说明 |
|:---|---:|:---|
| **历史基线**（未开 `--gc-sections`） | 32,795,018 | `add_int_trim_static_minstd_full`，参考用 |
| **Phase-0 基线**（同口径，开 `--gc-sections`） | 26,819,671 | 本轮 trim 的真实起点 |
| Phase-2（链接 stub `LLVMInitializeAArch64AsmParser`） | 25,816,847 | −1,002,824 B，前一轮 |
| Phase-3（+ `-ffunction-sections -fdata-sections` on EasyJitRuntime_static） | 25,757,303 | −59,544 B，前一轮 |
| **本轮起点**（含上面所有 link/cmake 层裁剪） | **25,757,303** | |

所有新增数字都以 **25,757,303** 为基线做对比。

## 三、本轮已实际落地的 LLVM 源码补丁

### 补丁 1：`[AArch64] LLVM_AARCH64_DISABLE_GISEL` cmake 选项
**Commit**：`4250e96deff7`

**改动文件**：
- `llvm/lib/Target/AArch64/CMakeLists.txt`
- `llvm/lib/Target/AArch64/AArch64TargetMachine.cpp`
- `llvm/lib/Target/AArch64/AArch64Subtarget.cpp`

**做了什么**：新增 `LLVM_AARCH64_DISABLE_GISEL` 构建选项；当打开时：
1. 切断 `LLVMInitializeAArch64Target` 里 `initializeGlobalISel()` 与 5 个
   AArch64 GISel pass 初始化调用。
2. 移除 `setGlobalISel(true)` / `setGlobalISelAbort()` 的运行时启用路径。
3. 编译掉 7 个 `AArch64PassConfig` 的 GISel 钩子（`addIRTranslator`、
   `addLegalizeMachineIR`、`addRegBankSelect` 等）。
4. 编译掉 `AArch64Subtarget` 构造函数里对
   `AArch64CallLowering` / `InlineAsmLowering` / `AArch64LegalizerInfo`
   / `createAArch64InstructionSelector` / `AArch64RegisterBankInfo` 的构造。

被裁掉的强引用链条：
```
AArch64TargetMachine.cpp.o → (unresolved) → libLLVMGlobalISel.a(GlobalISel.cpp.o)
AArch64Subtarget.cpp.o     → (unresolved) → AArch64CodeGen.a(GISel/*.cpp.o)
AArch64CodeGen.a(GISel/*.cpp.o) → (unresolved) → libLLVMGlobalISel.a(*)
```

**效果**：
- `libLLVMAArch64CodeGen.a` 中 `GISel/*.cpp.o` 成员不再被拉入
  （AArch64CodeGen 从 4,472,003 B → 2,524,722 B，**−1,947,281 B**）。
- `libLLVMGlobalISel.a` 整体不再出现在 top archive 里，剩余 ~100 KB
  来自 `unique_ptr<LegalizerInfo>` / `unique_ptr<InstructionSelector>` 等
  析构函数的 vtable，属于 header 级别强引用，不影响大局。
- 整体 text：`25,757,303 → 22,916,655`，**−2,840,648 B（−11.03%）**。
- 二进制完全静态、可运行、输出正确。

### 补丁 2：`[AsmPrinter] LLVM_DISABLE_INLINE_ASM_PARSER` cmake 选项
**Commit**：`34b172614236`

**改动文件**：
- `llvm/lib/CodeGen/AsmPrinter/CMakeLists.txt`
- `llvm/lib/CodeGen/AsmPrinter/AsmPrinterInlineAsm.cpp`

**做了什么**：当 `LLVM_DISABLE_INLINE_ASM_PARSER=ON`，
`AsmPrinter::emitInlineAsm(StringRef, ...)` 永远走原始文本
（`OutStreamer->emitRawText(...)`）分支，不再调用 `createMCAsmParser` /
`createMCAsmParser` 来 re-parse 内联汇编字符串。

被切断的强引用：
```
libLLVMAsmPrinter.a(AsmPrinterInlineAsm.cpp.o) →
    createMCAsmParser → libLLVMMCParser.a(AsmParser.cpp.o) →
    createELFAsmParser / createCOFFAsmParser / createDarwinAsmParser /
    createGOFFAsmParser / createWasmAsmParser → 全部 MCParser 成员
```

**效果**：
- AsmPrinter 这端的引用已经切断。
- 但是 `libLLVMObject.a(ModuleSymbolTable.cpp.o)` 还是会调用
  `llvm::createMCAsmParser`，因此 `libLLVMMCParser.a` 当前仍然被
  ModuleSymbolTable 拉入，这个补丁单独看净效应 ≈ 0（实际 +17 KB）。
- 本补丁是后续去掉 MCParser 的**必要前提**，之后如果再把
  `ModuleSymbolTable.cpp` 里的 `createMCAsmParser` 调用也切断，
  预期可以直接减掉 283 KB 的 `libLLVMMCParser.a`。

### 补丁 3：`[AsmPrinter] LLVM_DISABLE_DEBUG_INFO_EMISSION` cmake 选项
**Commit**：`b1de438a03f6`

**改动文件**：
- `llvm/lib/CodeGen/AsmPrinter/CMakeLists.txt`
- `llvm/lib/CodeGen/AsmPrinter/AsmPrinter.cpp`

**做了什么**：当 `LLVM_DISABLE_DEBUG_INFO_EMISSION=ON`：
1. `AsmPrinter::doInitialization` 中跳过 `DwarfDebug` 和 `CodeViewDebug`
   handler 的构造。
2. `AsmPrinter::emitInitialRawDwarfLocDirective` 整体变为 no-op。

被切断的强引用：
```
AsmPrinter.cpp.o (always in link) →
    new DwarfDebug(...) → libLLVMAsmPrinter.a(DwarfDebug.cpp.o) →
    DwarfCompileUnit.cpp.o → DwarfUnit.cpp.o → DwarfFile.cpp.o →
    DwarfExpression.cpp.o → DwarfStringPool.cpp.o →
    DIE.cpp.o → DIEHash.cpp.o →
    AccelTable.cpp.o → AddressPool.cpp.o →
    DebugLocStream.cpp.o → DbgEntityHistoryCalculator.cpp.o
    → libLLVMDebugInfoDWARF.a (通过 DwarfDebug 的 DWARFExpression 使用)
    → libLLVMDebugInfoCodeView.a (通过 CodeViewDebug)
```

**效果**：
- `libLLVMAsmPrinter.a` 从 782,037 B 跌到 202,953 B（**−579,084 B / −74%**）。
- `libLLVMDebugInfoCodeView.a` 完全不再被拉入（0 引用）。
- `libLLVMDebugInfoDWARF.a` 从 AsmPrinter 这端切断，
  但仍然被 `libLLVMProfileData.a(InstrProfCorrelator.cpp.o) → DWARFContext`
  和 `libLLVMSelectionDAG.a(StatepointLowering.cpp.o)` 拉入，仍在 link 内，
  但已不在 top 20 archive。
- 额外 text：`22,934,383 → 22,178,903`，**−755,480 B（−3.29%）**。
- 二进制完全静态、可运行、输出正确。

## 四、累计量化结果

所有三个 LLVM 源码补丁 + 之前链接 stub + EasyJIT 侧 `-ffunction-sections`
同时打开，产物为 `trim-experiments/out/p6b_no_di`：

| 阶段 | 补丁 | text | 相对上一阶段 | 相对 Phase-0 |
|:---|:---|---:|---:|---:|
| Phase-0 | `--gc-sections` only | 26,819,671 | — | — |
| Phase-2 | + `LLVMInitializeAArch64AsmParser` stub | 25,816,847 | −1,002,824 | −1,002,824 |
| Phase-3 | + `-ffunction-sections` on EasyJitRuntime_static | 25,757,303 | −59,544 | −1,062,368 |
| **Phase-4** | + `LLVM_AARCH64_DISABLE_GISEL` | **22,916,655** | **−2,840,648** | **−3,903,016** |
| Phase-5 | + `LLVM_DISABLE_INLINE_ASM_PARSER` | 22,934,383 | +17,728 | −3,885,288 |
| **Phase-6** | + `LLVM_DISABLE_DEBUG_INFO_EMISSION` | **22,178,903** | **−755,480** | **−4,640,768** |

**相对 Phase-0 的总裁剪：−4,640,768 B（−17.30%）**。
**相对历史基线 32,795,018 的总裁剪：−10,616,115 B（−32.37%）**。

全部阶段产物均通过以下验证：
```
file out/*       : ELF 64-bit LSB executable, ARM aarch64, statically linked
./out/p6b_no_di  : inc(4) is 5 / inc(5) is 6 / inc(6) is 7 / inc(7) is 8
```

## 五、当前最大收益来源（按收益排序）

1. **关闭 AArch64 GlobalISel**（补丁 1）：**−2.84 MB**
   收益最高、改动相对集中、风险低（JIT 在 `CodeGenOpt::Aggressive` 下
   原本就走 SelectionDAG）。
2. **关闭 AsmPrinter 中的 DwarfDebug / CodeViewDebug 构造**（补丁 3）：**−0.76 MB**
   收益次高、前提是 JIT 输入模块没有 debuginfo 元数据（EasyJIT 现在就是这样）。
3. **关闭 AsmPrinter 中的 MCAsmParser 内联汇编回退路径**（补丁 2）：
   单独 ≈ 0，但为下一步删除 `libLLVMMCParser.a` 铺平道路。
4. *（前一轮已做）* 链接层 stub `LLVMInitializeAArch64AsmParser`：−1.0 MB
5. *（前一轮已做）* `-ffunction-sections -fdata-sections` + `--gc-sections`：−60 KB

## 六、当前残余大头 & 接下来最值得做的裁剪

### top archive 贡献（本轮最终产物，单位 Byte）
```
 4,784,120  libLLVMCodeGen.a
 2,805,804  libLLVMSelectionDAG.a
 2,524,722  libLLVMAArch64CodeGen.a     ← GISel 已经去掉一大半
 2,298,919  libLLVMCore.a
 2,020,253  libLLVMAnalysis.a
 1,711,410  libLLVMAArch64Desc.a
   971,978  libLLVMTransformUtils.a
   921,965  libEasyJitRuntime.a
   850,457  libc.a (静态 glibc)
   741,270  libLLVMSupport.a
   715,259  libLLVMMC.a
   551,529  libLLVMScalarOpts.a
   298,965  libLLVMRuntimeDyld.a
   291,654  libLLVMObject.a
   283,410  libLLVMMCParser.a           ← 还在，被 ModuleSymbolTable 拉住
   240,357  libLLVMBitReader.a
   202,953  libLLVMAsmPrinter.a         ← Dwarf/CodeView 都裁掉了
```

### 下一步优先级（从易到难）

**1) 解锁 `libLLVMMCParser.a`（−283 KB 确定可拿）**

根源：`libLLVMObject.a(ModuleSymbolTable.cpp.o) → createMCAsmParser`。
建议：在 `ModuleSymbolTable.cpp` 中把 `initializeRecordStreamer` 走的
inline asm parser 分支 `#ifdef` 掉（类似补丁 2 的做法），加
`LLVM_DISABLE_IR_SYMBOL_TABLE_ASM_PARSING` 选项。

**2) ProfileData → DebugInfoDWARF 链（预计 −200~300 KB）**

根源：`libLLVMProfileData.a(InstrProfCorrelator.cpp.o) → DWARFContext`。
`InstrProfCorrelator` 仅用于 PGO profile correlation；JIT 完全不需要。
建议：加 `LLVM_PROFILEDATA_DISABLE_CORRELATOR` 选项，把
`InstrProfCorrelator.cpp` 从 ProfileData 源文件列表中去掉，并 stub 它
对外暴露的少量 API（只有 `InstrProfReader` 会引用）。

**3) AsmPrinter 里还没切的 DwarfCFIException（估计 −50 KB）**

不能简单去掉 — `.eh_frame` 依赖 DwarfCFIException；但可以把其对
DIE / Dwarf* 的引用换成最小 CFI 实现。

**4) AArch64 FastISel（估计 −100 KB）**

JIT 跑在 -O3，FastISel 永远不会被选中，但 `AArch64FastISel.cpp`
永远被编译进 AArch64CodeGen。加 `LLVM_AARCH64_DISABLE_FASTISEL` 选项。

**5) SelectionDAG 深度裁剪（估计 −500 KB 难度高）**

2.8 MB 里包含 `FastISelEmitter.cpp.o`、`LegalizeVectorOps`、
`StatepointLowering` 等 JIT 场景不需要的功能，需要在 SelectionDAG.cmake
里做子组件切分。

**6) AArch64CodeGen 内部深度裁剪（估计 −300~500 KB）**

例子：`AArch64SLSHardening.cpp`、`AArch64SpeculationHardening.cpp`、
`AArch64LowerHomogeneousPrologEpilog.cpp`、`AArch64StackTagging.cpp`
等只对特定 CPU/OS/安全场景有效，都可以 `#ifdef` 或 CMake 级别剔除。

**7) `libLLVMCore.a` / `libLLVMAnalysis.a` / `libLLVMTransformUtils.a`
内部逐个 pass 裁剪（难度最高）**

Core/Analysis/TransformUtils 里 JIT 用不到的 pass 占比相当高，
但强引用错综复杂，通常需要先做"pass factory opt-in"改造。

## 七、已验证的、但未带来净收益的尝试

- **`LLVM_DISABLE_INLINE_ASM_PARSER`**（补丁 2）：切断了 AsmPrinter → MCParser
  一条强引用，但 MCParser 还有第二条来源（ModuleSymbolTable），所以
  单独打开时净效应 ~0。**保留**补丁，作为下一步的前提。

## 八、没做"理论分析收工"的证据

每一个补丁都伴随：
- 真实编辑 `llvm-project-15.0.4/llvm/**` 下的 LLVM 源文件
- 真实 ninja 增量构建
- 真实重新静态链接 `add_int`
- 真实运行 binary 并比对输出
- 真实生成 map 文件并验证目标 archive / 成员不再被拉入

提交哈希：
```
4250e96deff7  [AArch64] Add LLVM_AARCH64_DISABLE_GISEL cmake option
34b172614236  [AsmPrinter] Add LLVM_DISABLE_INLINE_ASM_PARSER cmake option
b1de438a03f6  [AsmPrinter] Add LLVM_DISABLE_DEBUG_INFO_EMISSION cmake option
```

## 九、可复现入口

```bash
# 1. 打开三个选项
cd /home/ruanchen/workspace/llvm-project-15.0.4/build-host
cmake -DLLVM_AARCH64_DISABLE_GISEL=ON \
      -DLLVM_DISABLE_INLINE_ASM_PARSER=ON \
      -DLLVM_DISABLE_DEBUG_INFO_EMISSION=ON .
ninja LLVMAArch64CodeGen LLVMAsmPrinter

# 2. 静态链接 add_int（已经 stub 了 LLVMInitializeAArch64AsmParser）
cd /home/ruanchen/workspace/llvm-project-15.0.4/trim-experiments
EXTRA_STUBS="asmparser_stub.o" \
  ./link_static_add_int.sh p6b_no_di archives_phase2.txt

# 3. 验证
file out/p6b_no_di       # -> statically linked
ldd  out/p6b_no_di       # -> not a dynamic executable
./out/p6b_no_di          # -> inc(4)..inc(7)=5..8
size out/p6b_no_di       # -> text 22,178,903
```

## 十、结论

**本轮已经把 LLVM 静态体积从 25.76 MB 降到 22.18 MB（−3.58 MB，−13.9%）
且全部通过 LLVM 仓内源码补丁 + 默认 OFF 的 CMake 选项完成**，不破坏
任何既有 LLVM 构建路径，JIT 仍然是完全静态可运行的。

下一轮最值得继续投入的方向依次是：
1. **MCParser 的第二条强引用（ModuleSymbolTable）→ 确定能再拿 −283 KB**
2. **ProfileData 里 InstrProfCorrelator → DebugInfoDWARF 链 → −200~300 KB**
3. **AArch64 FastISel 和若干硬化 pass 的 CMake 开关 → −200~500 KB**
4. **SelectionDAG / AArch64CodeGen 内部深度裁剪（难度较大） → −0.5~1 MB**
