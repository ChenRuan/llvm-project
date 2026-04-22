# EasyJIT / LLVM 嵌入式裁剪阶段总结

这份文档总结当前 `llvm15_trim_llvm_backend` 分支上已经完成的 LLVM 源码级裁剪工作，给第一次接触这条线的人一个比较完整的全景：

- 现在最小可运行、完全静态的结果是多少
- 体积主要被哪些部分占据
- 已经裁掉了哪些东西
- 这条路线当前推进到了什么阶段
- 如果继续往下做，最合理的方案是什么

本文尽量只陈述已经验证过的事实，并说明这些结果对应的前提条件。

## 1. 实验对象与口径

工作目录：

- `llvm-project-15.0.4`

实验分支：

- `llvm15_trim_llvm_backend`

目标平台：

- `aarch64-linux-gnu`

当前统一使用的验证目标：

- EasyJIT trim 工作树中的 `tests/c_api/add_int.c`

当前统一使用的“最小完全静态”验证标准：

1. `file` 显示 `statically linked`
2. `ldd` 显示 `not a dynamic executable`
3. 程序运行正确：

```text
inc(4) is 5
inc(5) is 6
inc(6) is 7
inc(7) is 8
```

当前最小可运行产物：

- `trim-experiments/out/p14_orc`

其 `size` 结果为：

```text
text      data    bss      dec       hex
21211843  790420  176812   22179075  1526d03
```

也就是：

- 当前最小可运行完全静态 `text = 21,211,843`

## 2. 结果应该如何理解

### 2.1 这不是早期 32MB 那套“大盘”口径

历史上我们曾测到过更大的完全静态结果，例如：

- 原始 `libstdc++.a` 路线：`text = 33,220,297`
- `minimal_libstdcpp.o` 路线：`text = 32,795,018`

那一组数字记录的是：

- EasyJIT trim 工作树下
- 完整 native target 初始化补齐
- 完全静态可运行
- 但尚未做 LLVM 仓源码级后端裁剪

而本文记录的是：

- 在 LLVM 仓内逐轮做源码级裁剪之后
- 同样仍然要求完全静态、可运行
- 得到的新最小结果

所以：

- `32.8MB / 33.2MB` 是“源码级 LLVM trim 之前”的完全静态大盘
- `21.2MB` 是“LLVM 源码级 trim 之后”的当前最小完全静态结果

### 2.2 `size` 的 `text` 不是纯机器码

本文里所有 `size` 的 `text`，都应该理解成“代码 / 只读段总量”，通常会包含：

- `.text`
- `.rodata`
- `.eh_frame`
- `.gcc_except_table`
- 以及其它只读段

所以：

- 某个 LLVM 静态库“看起来只是一个 `.a`”
- 但它带进来的不只是机器码，还可能有大量描述表、目标后端表、异常表

这也是为什么某些 LLVM 归档库会贡献几 MB。

### 2.3 下面的“各 part 大小”是 map 口径下的近似贡献

本报告中的“各个 part 大小”来自：

- `trim-experiments/out/p14_orc.map`
- `trim-experiments/analyze_map.sh`

它更适合回答：

- 最终体积的大头是谁
- 哪些 archive 仍然最值得关注

它不是用来精确对账每一个字节的“唯一真值”，但对于排序和定位瓶颈已经足够准确。

## 3. 当前最小完全静态结果的主要组成

以下数字来自当前最小可运行完全静态产物 `trim-experiments/out/p14_orc` 的 map 统计，按贡献从大到小排序。

### 3.1 当前主要 archive / object 贡献

| 部分 | 近似贡献（bytes） |
|---|---:|
| `libLLVMCodeGen.a` | `4,561,542` |
| `libLLVMSelectionDAG.a` | `2,796,659` |
| `libLLVMCore.a` | `2,292,745` |
| `libLLVMAArch64CodeGen.a` | `2,237,319` |
| `libLLVMAnalysis.a` | `1,938,497` |
| `libLLVMAArch64Desc.a` | `1,711,410` |
| `libLLVMTransformUtils.a` | `957,480` |
| `libEasyJitRuntime.a` | `898,941` |
| `libc.a` | `850,457` |
| `libLLVMSupport.a` | `729,343` |
| `libLLVMMC.a` | `701,919` |
| `libLLVMScalarOpts.a` | `551,529` |
| `libLLVMRuntimeDyld.a` | `298,965` |
| `libLLVMObject.a` | `277,573` |
| `libLLVMBitReader.a` | `241,977` |
| `libLLVMAsmPrinter.a` | `202,599` |
| `libLLVMOrcJIT.a` | `173,568` |
| `libLLVMBitWriter.a` | `173,567` |
| `libLLVMAArch64Utils.a` | `125,893` |
| `libLLVMProfileData.a` | `109,730` |
| `libLLVMipo.a` | `89,763` |
| `libm-2.39.a` | `89,452` |
| `libLLVMLinker.a` | `82,788` |
| `libLLVMBitstreamReader.a` | `39,916` |
| `libgcc_eh.a` | `32,140` |
| `libLLVMBinaryFormat.a` | `22,514` |
| `libLLVMJITLink.a` | `16,138` |
| `libLLVMTarget.a` | `14,696` |
| `libgcc.a` | `9,201` |
| `libLLVMOrcShared.a` | `8,650` |

### 3.2 这组数字说明了什么

最重要的结论有三个：

1. **主要体积仍然来自 LLVM 后端和中层基础设施**
   - `LLVMCodeGen`
   - `LLVMSelectionDAG`
   - `LLVMCore`
   - `LLVMAArch64CodeGen`
   - `LLVMAnalysis`

2. **EasyJIT 自己这层不是主要瓶颈**
   - `libEasyJitRuntime.a` 约 `898,941`
   - 它明显不是 21MB 体积的主要来源

3. **ORC 自身已经不是最大头**
   - `libLLVMOrcJIT.a` 约 `173,568`
   - `libLLVMOrcShared.a` 约 `8,650`
   - 当前大头更像是“真正生成 AArch64 机器码所需的 codegen/backend 闭包”

## 4. 当前裁剪进度

### 4.1 起点与当前最小结果

本轮 LLVM 源码级裁剪的 Phase-0 基线是：

- `26,819,671`

当前 round-4 结束后的最好结果是：

- `21,211,843`

累计下降：

- `-5,607,828`
- 约 `-20.91%`

如果相对于历史 32.80MB 大盘看：

- 约下降 `35%` 左右

### 4.2 已完成的源码级裁剪点

当前已经落地并验证过的开关包括：

| 开关 | 作用 | 安全性结论 |
|---|---|:-:|
| `LLVM_AARCH64_DISABLE_GISEL` | 关闭 AArch64 GlobalISel 路径 | ✅ |
| `LLVM_DISABLE_INLINE_ASM_PARSER` | 关闭 inline asm parser 相关路径 | ⚠ |
| `LLVM_DISABLE_DEBUG_INFO_EMISSION` | 关闭 AsmPrinter 调试信息发射 | ⚠ |
| `LLVM_DISABLE_IR_SYMBOL_TABLE_ASM_PARSING` | 关闭 IR 符号表里的 asm parsing 路径 | ✅ |
| `LLVM_PROFILEDATA_DISABLE_CORRELATOR` | 关闭 profile correlator | ✅ |
| `LLVM_AARCH64_DISABLE_FASTISEL` | 关闭 AArch64 FastISel | ✅ |
| `LLVM_AARCH64_DISABLE_HARDENING_PASSES` | 关闭 AArch64 hardening 类 pass | ⚠ |
| `LLVM_AARCH64_DISABLE_CPU_SPECIFIC_PASSES` | 关闭 A53/A57/Falkor/BTI/CondBrTuning 等 | ✅ |
| `LLVM_CODEGEN_DISABLE_NONLINUX_EH` | 关闭非 Linux EH 相关 prepare/helper 路径 | ✅ |
| `LLVM_CODEGEN_DISABLE_MACHINE_OUTLINER` | 关闭 MachineOutliner | ✅ |
| `LLVM_CODEGEN_DISABLE_SAFESTACK` | 关闭 SafeStack | ✅ |
| `LLVM_ORC_DISABLE_NON_ELF_PLATFORMS` | 关闭 ORC 的 MachO/COFF 平台层 | ✅ |

### 4.3 各轮收益趋势

从阶段效果看，收益已经明显进入递减：

- Round-1/2：还存在 MB 级收益
- Round-3：`-281,072`
- Round-4：`-37,216`

这说明：

- 易于裁掉的“明显不属于当前嵌入式 JIT 场景”的部分，基本已经清掉了
- 剩下的部分越来越接近真正被当前 JIT 路径使用的代码

## 5. 当前阶段的客观判断

### 5.1 这条路线还没到“绝对极限”

从工程上说，LLVM 仓内当然还可以继续改：

- 可以碰 `SelectionDAG` 更深层的骨架
- 可以继续碰 GC / statepoint
- 可以把 `LLVMCore` / `LLVMAnalysis` / `LLVMTransformUtils` 改成更激进的 opt-in 结构

所以不能说“完全没有空间”。

### 5.2 但已经接近“低风险源码级裁剪”的阶段性极限

如果约束仍然是：

- 保持完全静态
- 保持当前 ORC + AArch64 原生 codegen 路线
- 不动 SelectionDAG backbone
- 不做大规模中层框架重构

那当前这条源码级开关裁剪路线已经接近瓶颈。

最直接的证据就是：

- Round-4 总收益只有 `37 KB`

这意味着后续继续做类似级别的小开关，很可能只会获得：

- 几十 KB
- 或几百 KB

而不是再来一次 MB 级跳水。

### 5.3 6MB 目标在当前技术路线下基本不现实

如果同时要求：

- 完全静态
- 可运行
- 保留 LLVM ORC JIT
- 保留 AArch64 原生 codegen

那么从当前 `21.2MB` 再压到 `6MB`，意味着还要继续砍掉 70% 左右的“当前实际在工作”的后端与中层代码。

这已经不是继续加几个裁剪开关能解决的问题。

## 6. 当前推荐的最终裁剪方案

这里的“最终方案”，不是说项目永远不能再改，而是指：**基于当前收益、风险和维护成本，适合收口并对外使用的一套方案。**

### 6.1 推荐保留的 LLVM 源码级裁剪集合

对于当前嵌入式 / 静态部署场景，推荐把以下开关作为一套成组方案看待：

- `LLVM_AARCH64_DISABLE_GISEL`
- `LLVM_DISABLE_IR_SYMBOL_TABLE_ASM_PARSING`
- `LLVM_PROFILEDATA_DISABLE_CORRELATOR`
- `LLVM_AARCH64_DISABLE_FASTISEL`
- `LLVM_AARCH64_DISABLE_CPU_SPECIFIC_PASSES`
- `LLVM_CODEGEN_DISABLE_NONLINUX_EH`
- `LLVM_CODEGEN_DISABLE_MACHINE_OUTLINER`
- `LLVM_CODEGEN_DISABLE_SAFESTACK`
- `LLVM_ORC_DISABLE_NON_ELF_PLATFORMS`

这组更适合默认开启，因为它们对应的功能与当前 ELF-only、AArch64、普通 C JIT 场景的耦合最低。

以下几项建议按需判断：

- `LLVM_DISABLE_INLINE_ASM_PARSER`
- `LLVM_DISABLE_DEBUG_INFO_EMISSION`
- `LLVM_AARCH64_DISABLE_HARDENING_PASSES`

原因是它们虽然已经验证可用，但更依赖下游对 IR 属性、调试需求、安全要求的接受程度。

### 6.2 推荐的构建级配置

如果目标是继续在当前路线下尽量压体积，构建级建议优先保留：

- `-static`
- `-no-pie`
- `-O2`
- `-s`
- `-Wl,--gc-sections`

这几项已经被证明能明显改善最终静态体积口径。

### 6.3 后续如果还要继续降体积，优先顺序应改变

当前源码级裁剪已经接近低风险瓶颈。  
如果后续仍要继续往下压，更建议转向：

1. **build-level 优化**
   - LTO
   - `--icf=safe`
   - 更激进的 section 合并与去重

2. **EasyJIT 侧功能面继续缩窄**
   - 减少真正会触发的 LLVM 功能面
   - 继续让前端 specialization 更定向

3. **只有在愿意接受更大风险时，才继续 LLVM backbone 级改动**
   - `SelectionDAG`
   - GC / statepoint
   - 更深的 `LLVMCore / Analysis / TransformUtils` opt-in 化

## 7. 结论

当前这条 LLVM 源码级裁剪路线已经取得了明确成果：

- 从 Phase-0 的 `26.8MB`
- 降到当前最小完全静态可运行的 `21.2MB`

而且这些结果是：

- 完全静态
- 可运行
- 已经过 `file` / `ldd` / 实际执行验证

当前体积的主要来源已经非常清楚：

- `LLVMCodeGen`
- `LLVMSelectionDAG`
- `LLVMCore`
- `LLVMAArch64CodeGen`
- `LLVMAnalysis`

这说明：

- EasyJIT 自己并不是当前静态体积的主要瓶颈
- LLVM 后端和中层闭包才是

就当前这条“低风险源码级裁剪”路线而言，已经接近阶段性瓶颈。  
继续往下做当然还有空间，但更可能需要：

- 更重的 LLVM 骨架改动
- 或转向 build-level / EasyJIT-side 方案

如果目标是稳定、可维护、可解释地收口，那么当前 12 个 LLVM 裁剪开关加上现有 build 配置，已经可以视为这轮嵌入式静态裁剪的阶段性最终方案。
