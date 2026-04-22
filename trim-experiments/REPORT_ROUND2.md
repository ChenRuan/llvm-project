# LLVM 源码裁剪 —— 第二轮报告

分支：`llvm15_trim_llvm_backend`（基于 `llvmorg-15.0.4`）
工作目录：`/home/ruanchen/workspace/llvm-project-15.0.4`
验证用例：`easy-jit-llvm15/tests/c_api/add_int.c`，全静态链接，`inc(4)..inc(7)` 全部正确。

## 1. 本轮 commit 清单

本轮在已有 3 个补丁基础上新增 4 个补丁，全部已 commit：

```
878849cfe40b  [AArch64] Add LLVM_AARCH64_DISABLE_HARDENING_PASSES cmake option   (P7)
c4958bc87b47  [AArch64] Add LLVM_AARCH64_DISABLE_FASTISEL cmake option           (P6)
410fcf8138e0  [ProfileData] Add LLVM_PROFILEDATA_DISABLE_CORRELATOR cmake option (P5)
b7aacf7ebd11  [Object] Add LLVM_DISABLE_IR_SYMBOL_TABLE_ASM_PARSING cmake option (P4)
-----------------------------（第一轮）---------------------------------
b1de438a03f6  [AsmPrinter] Add LLVM_DISABLE_DEBUG_INFO_EMISSION cmake option
34b172614236  [AsmPrinter] Add LLVM_DISABLE_INLINE_ASM_PARSER cmake option
4250e96deff7  [AArch64] Add LLVM_AARCH64_DISABLE_GISEL cmake option
```

## 2. 尺寸演进（text 口径）

| 阶段 | 补丁 | text (B) | Δ 上一步 | Δ Phase-0 |
|:---|:---|---:|---:|---:|
| Phase-0 | 仅 `--gc-sections` 基线 | 26,819,671 | — | — |
| Phase-3 | 第一轮结束（P1-P3） | 22,178,903 | — | −4,640,768 |
| **P4** | `LLVM_DISABLE_IR_SYMBOL_TABLE_ASM_PARSING` | **21,852,607** | **−326,296** | −4,967,064 |
| P5 | `LLVM_PROFILEDATA_DISABLE_CORRELATOR` | 21,852,607 | 0（text） | −4,967,064 |
| **P6** | `LLVM_AARCH64_DISABLE_FASTISEL` | **21,690,303** | **−162,304** | −5,129,368 |
| **P7** | `LLVM_AARCH64_DISABLE_HARDENING_PASSES` | **21,530,131** | **−160,172** | **−5,289,540** |

本轮新增净收益：**−648,772 B（−2.95%）**
累计相对 Phase-0：**−5,289,540 B（−19.72%）**
累计相对历史 32.80 MB 大盘：**约 −34.4%**

P5 在 text 口径看似 0 收益，实则把 `libLLVMDebugInfoDWARF.a` 从最终链接列表里**完全移除**（`archives_phase2.txt` 37 → `archives_phase3.txt` 36），DWARF 相关 `.o` 成员全部不再被拉入；对构建时间和 archive footprint 有收益，对 gc 后的 text 无影响（因为原本就已被节级 gc 掉了）。

## 3. 各补丁细节

### P4 · `LLVM_DISABLE_IR_SYMBOL_TABLE_ASM_PARSING`

**问题**：`libLLVMMCParser.a` 还被 `libLLVMObject.a(ModuleSymbolTable.cpp.o) → createMCAsmParser` 拉入，P2 单独并未关闭这条链。

**定位**：`llvm/lib/Object/ModuleSymbolTable.cpp::initializeRecordStreamer` 在 module-level inline asm 非空时创建 MCAsmParser。JIT/ORC + add_int 场景下 module-level inline asm 为空，该路径恒不触发，但符号仍被引用。

**改动**：
- `llvm/lib/Object/CMakeLists.txt`：新增 `option(LLVM_DISABLE_IR_SYMBOL_TABLE_ASM_PARSING …)`，开启时 `add_compile_definitions`。
- `llvm/lib/Object/ModuleSymbolTable.cpp`：MCParser 头文件与 `initializeRecordStreamer` 主体整体包在 `#ifndef`，开启时 stub 版本直接 `return`。

**验证**：
- `libLLVMMCParser.a` 引用数 973 → 1（仅剩 archive LOAD 记录本身）。
- `ModuleSymbolTable.cpp` 仍在，`CollectAsmSymbols/CollectAsmSymvers` API 签名不变。
- text：22,178,903 → 21,852,607（**−326,296 B**）。

### P5 · `LLVM_PROFILEDATA_DISABLE_CORRELATOR`

**问题**：`libLLVMDebugInfoDWARF.a` 被 `libLLVMProfileData.a(InstrProfCorrelator.cpp.o)` 拖入（DwarfInstrProfCorrelator → DWARFContext/DWARFDie/DWARFExpression）。

**定位**：`InstrProfCorrelator` 只有在 profile 文件显式使用 `useDebugInfoCorrelate()` 格式时才会被构造，JIT 不可能走到。

**改动**：
- `llvm/lib/ProfileData/CMakeLists.txt`：新增 `option(LLVM_PROFILEDATA_DISABLE_CORRELATOR …)`，使用 `target_compile_definitions(... PRIVATE ...)`。
- `llvm/lib/ProfileData/InstrProfCorrelator.cpp`：头文件组 + 几乎全部实现体包在 `#ifndef`；`#else` 分支仅保留必须的静态成员定义与工厂函数 stub（返回 `instrprof_error::unable_to_correlate_profile`）。头文件 `InstrProfCorrelator.h` 不动，外部 API 稳定。

**验证**：
- `libLLVMDebugInfoDWARF.a` 从 `archives_phase2.txt` 移除后依然链接成功（`archives_phase3.txt` = 36 个 archive）。
- 二进制 text 不变（原本就被 section-level gc 掉了），但链接 footprint 变化明显。
- 运行结果正确。

### P6 · `LLVM_AARCH64_DISABLE_FASTISEL`

**问题**：`AArch64FastISel.cpp` 约 5100 行，只在 `-O0` FastISel 路径被使用。JIT 默认 `-O2/-O3` 走 SelectionDAG，不会进入该路径；但 `AArch64TargetLowering::createFastISel` 强引用导致 `AArch64FastISel.cpp.o` 被拉入。

**改动**：
- `llvm/lib/Target/AArch64/CMakeLists.txt`：新增 `option(LLVM_AARCH64_DISABLE_FASTISEL …)`。
- `llvm/lib/Target/AArch64/AArch64ISelLowering.cpp::createFastISel`：开启时直接 `return nullptr`，不调 `AArch64::createFastISel`。

**验证**：
- map 中 `AArch64FastISel.cpp.o` 不再被拉入。
- text：21,852,607 → 21,690,303（**−162,304 B**）。
- 运行正确。

### P7 · `LLVM_AARCH64_DISABLE_HARDENING_PASSES`

**问题**：以下 5 个 hardening/MTE/speculation 相关 pass 恒被 addPass / initialize：

- `AArch64SpeculationHardeningPass`（speculation barriers）
- `AArch64SLSHardeningPass`（Straight-Line-Speculation 抑制）
- `AArch64IndirectThunks`（与 SLS 联用）
- `AArch64StackTaggingPass` / `AArch64StackTaggingPreRAPass`（MTE）
- `AArch64LowerHomogeneousPrologEpilogPass`（同质 prolog/epilog）

在纯 C + JIT 场景，function 上没有对应属性（`speculative_load_hardening` / `sanitize_memtag` / 同质序同需求），这些 pass 要么整体不做事、要么本身就因 `cl::opt` 默认 false 而不激活。但相关 `.o` 全部被拉入。

**改动**：
- `llvm/lib/Target/AArch64/CMakeLists.txt`：新增 `option(LLVM_AARCH64_DISABLE_HARDENING_PASSES …)`。
- `llvm/lib/Target/AArch64/AArch64TargetMachine.cpp`：同时 `#ifndef` 掉：
  - 构造器里 5 个 `initialize*Pass` 调用；
  - `addIRPasses` 里 `createAArch64StackTaggingPass`；
  - `addILPOpts` 里 `createAArch64StackTaggingPreRAPass`；
  - `addPreSched2` 里 `createAArch64LowerHomogeneousPrologEpilogPass` / `createAArch64SpeculationHardeningPass` / `createAArch64IndirectThunks` / `createAArch64SLSHardeningPass`。

**验证**：
- map 中上述 5 个 `.cpp.o` 全部不再被拉入。
- text：21,690,303 → 21,530,131（**−160,172 B**）。
- 运行正确。

## 4. 三道硬验证

每个补丁均通过：

1. `file out/<name>` → `statically linked`
2. `ldd out/<name>` → `not a dynamic executable`（由 `-static` 保证）
3. 运行 → `inc(4) is 5 / inc(5) is 6 / inc(6) is 7 / inc(7) is 8`

复现配置：

```bash
cd /home/ruanchen/workspace/llvm-project-15.0.4/build-host
cmake -DLLVM_AARCH64_DISABLE_GISEL=ON \
      -DLLVM_DISABLE_INLINE_ASM_PARSER=ON \
      -DLLVM_DISABLE_DEBUG_INFO_EMISSION=ON \
      -DLLVM_DISABLE_IR_SYMBOL_TABLE_ASM_PARSING=ON \
      -DLLVM_PROFILEDATA_DISABLE_CORRELATOR=ON \
      -DLLVM_AARCH64_DISABLE_FASTISEL=ON \
      -DLLVM_AARCH64_DISABLE_HARDENING_PASSES=ON .
ninja LLVMAArch64CodeGen LLVMAsmPrinter LLVMObject LLVMProfileData
cd ../trim-experiments
EXTRA_STUBS="asmparser_stub.o" ./link_static_add_int.sh p10b_no_hardening archives_phase3.txt
# -> text = 21,530,131 B, 运行正确
```

## 5. 补丁安全性判断

| 补丁 | 风险 | 可否在任何 JIT 场景安全保留 |
|:---|:---|:---|
| P1 `LLVM_AARCH64_DISABLE_GISEL` | 低 | 只要不依赖 GlobalISel（JIT 一般走 SelectionDAG），安全 |
| P2 `LLVM_DISABLE_INLINE_ASM_PARSER` | 中 | 若 JIT 的 IR 里**有函数内 inline asm**，会回退到 `emitRawText`（AsmPrinter 需要接受 raw text，ORC JITLink 在此模式下可能有限制）；普通 C 代码安全 |
| P3 `LLVM_DISABLE_DEBUG_INFO_EMISSION` | 中 | JIT 不生成 DWARF/CodeView 时完全安全；若需要 profiler 符号化，需保留 |
| P4 `LLVM_DISABLE_IR_SYMBOL_TABLE_ASM_PARSING` | 低 | 只影响 module-level inline asm 的符号收集；JIT/ORC 几乎用不到 |
| P5 `LLVM_PROFILEDATA_DISABLE_CORRELATOR` | 很低 | 只影响 PGO `useDebugInfoCorrelate` 格式，JIT 场景不触发 |
| P6 `LLVM_AARCH64_DISABLE_FASTISEL` | 很低 | 只在 `-O0` FastISel 激活时才会影响；JIT 几乎总是 -O2/-O3 |
| P7 `LLVM_AARCH64_DISABLE_HARDENING_PASSES` | 中 | 若 JIT 输入函数带 `speculative_load_hardening` / `sanitize_memtag`，需关闭本选项；普通代码安全 |

推荐**默认全部 ON** 的嵌入式 JIT profile：P1 / P4 / P5 / P6。
需要按业务判断的：P2 / P3 / P7。

## 6. archive footprint 变化

主要 archive 的 member 拉入情况（p10b vs p6b）：

- `libLLVMMCParser.a`：member 数 973 → 1（**P4 贡献**）
- `libLLVMDebugInfoDWARF.a`：在 `archives_phase3.txt` 中完全移除（**P5 贡献**）
- `libLLVMAArch64CodeGen.a`：
  - `AArch64FastISel.cpp.o` 去除（P6）
  - `AArch64SLSHardening / SpeculationHardening / StackTagging / StackTaggingPreRA / LowerHomogeneousPrologEpilog` 全部去除（P7）

## 7. 当前最小 text 与继续可打的点

**当前最小完全静态 text = 21,530,131 B**（约 20.53 MB）。

下轮仍可继续（按可行性排序）：

1. **`libLLVMSelectionDAG.a` 瘦身**（~2.8 MB）
   - 目标：`StatepointLowering` / `FunctionLoweringInfo::CreateSpillStackObject`（仅 GC statepoint 用）/ `LegalizeVectorTypes`（JIT + add_int 场景向量极少）。
   - 需要在 SelectionDAG 内加细粒度 CMake 开关，改动量较大，但潜在收益 >500 KB。

2. **`libLLVMAArch64CodeGen.a` 再瘦**（~100–300 KB）
   - 候选：`AArch64BranchTargets`（BTI，主要用于启用 BTI 的平台）、`AArch64A57FPLoadBalancing`（Cortex-A57 专用）、`AArch64FalkorHWPFFix`（Falkor 专用）、`AArch64CondBrTuning`（默认 cl::opt false）。
   - 做法与 P7 类似，逐个用细粒度开关包住 `initialize*` 与 `addPass`。

3. **`libLLVMipo.a` / `libLLVMScalarOpts.a` / `libLLVMTransformUtils.a` 按 pass opt-in**
   - EasyJIT 实际只跑 `O2` pipeline 中一小部分 IR 转换。
   - 需要调查实际 pass 链，潜在收益 1–2 MB，难度高。

4. **`libLLVMCodeGen.a` 瘦身**（~4.7 MB，占比最大）
   - 候选：`GCMetadata` / `GCRootLowering` / `ShadowStackGCLowering`（非 GC 语言不需要）、`MachineOutliner`（JIT 通常不开）、`FaultMaps` / `StackMaps`（与 statepoint 配对）。
   - 每一刀 50–200 KB，积少成多可 ≥500 KB。

5. **`libLLVMOrcJIT.a` 内部裁剪**
   - 如 `MachOPlatform` / `COFFPlatform` / `Debugging/PerfSupportPlugin` 等非 ELF / 非调试路径。

短期内性价比最高的下一刀：**按 P7 模式继续拆 `libLLVMAArch64CodeGen.a`** 与 **裁 `libLLVMCodeGen.a` 里的 GC / StackMap 系列**。

## 8. 不变量

本轮全程保持：

- 不依赖 EasyJIT 侧打补丁；
- 不依赖 link 层 stub 的「替换」语义（唯一保留的 stub 是 `asmparser_stub.o`，只是占位 `LLVMInitializeAArch64AsmParser`）；
- 每一步都做了 `file` / `ldd` / run 三道验证；
- 每一步都有 git commit 可回滚；
- 所有开关默认 OFF，不破坏 LLVM 默认构建。
