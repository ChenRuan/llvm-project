# EJIT Cross-TU Inlining 设计文档

**版本**: 1.1
**日期**: 2026-07-30
**关联**: SPEC4.md, PLAN4.md, PASS1_EJitRegisterBitcode.md

---

## 1. 概述

### 1.1 背景

当前 `ejit_entry` 函数调用其他 TU 的子函数时，PASS1 在编译期运行，只能看到当前 TU。跨 TU 的子函数在 bitcode 中表现为 `declare external` 声明，PASS1 无法将其内联。JIT 编译时仍有跨函数调用开销。

之前的方案是开启全局 ThinLTO，但 ThinLTO 会对整个程序生效——AOT 代码也被跨模块优化，增加了编译时间并影响了 AOT 代码的确定性。

### 1.2 目标

通过新选项 `-fejit-cross-inline`，让**只有** `__ejit_bitcode` 获得跨 TU 内联优化，AOT 代码走普通编译流程。

### 1.3 核心思路

将 PASS1 拆为两阶段：

- **编译期**：完整 Module IR 嵌入 `.ejit_cross` ELF section（不提取闭包，不生成 `__ejit_bitcode`）
- **链接期**：读取所有 `.ejit_cross` section → 合并 → 内联跨 TU 子函数 → 生成最终的 `__ejit_bitcode`

---

## 2. 整体流程

```
┌─────────────────────────────────────────────────────────────────┐
│  编译期 (-fejit-cross-inline -c)                                │
│                                                                 │
│  clang → IR → PASS1(cross-inline 模式)                         │
│                  │                                              │
│                  └─ 完整 Module 序列化为 bitcode                 │
│                     embedBitcodeInSection( ".ejit_cross" )      │
│                     不生成 @__ejit_bitcode                      │
│                     不生成 ejit_auto_register                   │
│                  │                                              │
│                  ↓                                              │
│              tu_a.o (含 .ejit_cross section)                    │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│  链接期 (-fejit-cross-inline *.o -o output)                     │
│  (-r 部分链接和最终链接处理逻辑完全相同)                          │
│                                                                 │
│  clang driver 或 ld.lld:                                        │
│    1. 扫描所有输入 .o, 提取 .ejit_cross section                 │
│    2. parseBitcodeFile → 每个 section 解析为一个 Module          │
│    3. llvm::Linker::linkInModule() → 合并所有 Module            │
│    4. 找 ejit_entry → computeTransitiveClosure                  │
│    5. AlwaysInliner → 内联跨 TU 子函数                          │
│    6. preOptimizeBitcode + reAnnotateMayConst                   │
│    7. 对每个 ejit_entry 单独:                                   │
│       CloneModule → computeTransitiveClosure                    │
│       → serializeToBitcode → 独立 @__ejit_bitcode_<name>       │
│    8. generateRegisterCall (每函数注册自己的 bitcode)            │
│    9. WriteBitcodeToFile → 临时 .bc (lld 原生支持)              │
│   10. lld 链接 (原始 .o + 临时 .bc)                             │
│   11. lld 丢弃原始 .ejit_cross section (已消费)                 │
│                                                                 │
│  输出: output 中无 .ejit_cross, 只有 @__ejit_bitcode            │
└─────────────────────────────────────────────────────────────────┘
```

### 2.1 `-r` 部分链接行为

```
clang -fejit-cross-inline -r tu_a.o tu_b.o -o combined.o
  → combined.o 中只有 @__ejit_bitcode, 无 .ejit_cross

clang -fejit-cross-inline combined.o tu_c.o -o app
  → 读取 tu_c.o 的 .ejit_cross + combined.o 的 @__ejit_bitcode
  → 合并处理 → 生成新的 @__ejit_bitcode
```

---

## 3. 编译期详细设计

### 3.1 新增 flag

**文件**: `llvm/lib/Passes/PassBuilderPipelines.cpp`

flag 必须放在 `PassBuilderPipelines.cpp`（`LLVMPasses` 库）而非 `EJitPassOptions.cpp`，因为 clang 解析 `cl::opt` 早于 `LLVMEmbeddedJIT` 的静态初始化。

```cpp
cl::opt<bool> EJitCrossInline(
    "ejit-cross-inline", cl::init(false), cl::Hidden,
    cl::desc("Defer bitcode extraction to link time for cross-TU inlining"));
```

flag 值通过 `EJitRegisterBitcodePass(bool CrossInline)` 构造函数传入。

### 3.2 PASS1 分支逻辑

**文件**: `llvm/lib/Transforms/EmbeddedJIT/EJitRegisterBitcode.cpp`

```cpp
struct EJitRegisterBitcodePass {
  bool CrossInline = false;
  // ...
};

PreservedAnalyses run(Module &M, ...) {
  if (CrossInline) {
    embedBitcodeInSection(M, ".ejit_cross");
    return PreservedAnalyses::none();
  }
  // 原有逻辑不变 ...
}
```

### 3.3 embedBitcodeInSection

```cpp
static void embedBitcodeInSection(Module &M, const std::string &BC,
                                    StringRef SectionName) {
  LLVMContext &Ctx = M.getContext();
  auto *ArrTy = ArrayType::get(Type::getInt8Ty(Ctx), BC.size());
  auto *Const = ConstantDataArray::get(Ctx, StringRef(BC.data(), BC.size()));
  auto *GV = new GlobalVariable(M, ArrTy, true, GlobalValue::InternalLinkage,
                                Const, "__ejit_cross_module");
  GV->setSection(SectionName);
  GV->setAlignment(Align(1));
  // 加入 llvm.compiler.used 防止编译器丢弃此 unreferenced global，
  // 但允许链接器 GC（--gc-sections）。ld.lld 在 cross-link 处理后
  // 主动丢弃此 section（ctx.ejitCrossLinked → InputSection::discarded）。
  appendToCompilerUsed(M, {GV});
}
```

### 3.4 与现有 PASS 顺序的关系

PASS1 (cross-inline 模式) 仍在 `buildPerModuleDefaultPipeline` 的最前面运行。
不生成 wrapper — 由后续的 PASS3 (EJitWrapperGenPass) 正常生成。
PASS2 (period 注册) 不受影响。

### 3.5 Clang Driver 编译选项

**文件**: `clang/include/clang/Driver/Options.td`

```
def fejit_cross_inline : Flag<["-"], "fejit-cross-inline">,
  HelpText<"Enable cross-TU inlining for EJIT entry functions">,
  Visibility<[ClangOption, CC1Option]>;
```

**文件**: `clang/lib/Driver/ToolChains/Clang.cpp`

在编译 Job 构造时（`ConstructJob`）传递：

```cpp
if (Args.hasArg(options::OPT_fejit_cross_inline)) {
  CmdArgs.push_back("-mllvm");
  CmdArgs.push_back("-ejit-cross-inline");
}
```

---

## 4. 链接期详细设计

### 4.1 核心函数

新建文件：**`clang/lib/Driver/EJitCrossLink.cpp`**

```cpp
/// 链接期跨 TU 内联处理。
/// 从所有输入 .o 中提取 .ejit_cross section，合并、内联、生成 @__ejit_bitcode，
/// 写入临时 .o 并返回路径。无 .ejit_cross 时返回空字符串。
std::string runEJitCrossLink(ArrayRef<std::string> InputFiles,
                              const std::string &TargetTriple);
```

### 4.2 处理流程

```
runEJitCrossLink(InputFiles, TargetTriple):

  1. 快速扫描: 检查是否有任何输入包含 .ejit_cross section
     └─ 无 → return ""

  2. 提取并解析:
     for each input file:
       ObjectFile::createObjectFile()
       for each section:
         if name == ".ejit_cross":
           parseBitcodeFile(section contents) → Module

  3. 合并:
     Composite = std::move(FirstModule)
     for each remaining Module:
       Linker::linkInModule(module, None)  // 全部链接，确保所有 ejit_entry 都在

  4. 闭包计算:
     collectEntryFunctions(Composite) → EntryFuncs
     computeTransitiveClosure(EntryFuncs) → ClosureFuncs
     eraseFromParent all non-closure non-decl functions

  5. 内联跨 TU 子函数:
     ModuleInlinerPass(O2, ThinOrFullLTOPhase::None).run(Composite)

  6. 预优化 + metadata 恢复:
     preOptimizeBitcode(Composite)  // AlwaysInline → Mem2Reg → InstCombine → SimplifyCFG
     reAnnotateMayConst(Composite)

  7. 对每个 ejit_entry 单独提取闭包 + 序列化:
     for each EntryFunc:
       PerFuncModule = CloneModule(Composite)
       computeTransitiveClosure({EntryFunc}, Closure, Globals)
       删 PerFuncModule 中非闭包函数/全局变量
       preOptimizeBitcode(*PerFuncModule)       // 轻量优化
       serializeToBitcode(*PerFuncModule) → BC  // 只含当前函数+依赖

  8. 生成临时 Module (含 N 个 @__ejit_bitcode_<name> + ejit_auto_register):
     TmpModule = new Module("ejit_cross_link", Ctx)
     复制 DataLayout + TargetTriple
     for each EntryFunc + its BC:
       embedBitcode(TmpModule, BC, "__ejit_bitcode_" + funcName)
     generateRegisterCall(TmpModule, EntryFuncBitcodeMap)
       → 每函数注册自己的 bitcode

  9. 写入临时 .bc:
     WriteBitcodeToFile(TmpModule) → 临时 .bc 文件
     lld 原生支持 .bc 输入，无需编译为 .o

  10. return tmpfile path
```

### 4.3 复用 PASS1 的内部函数

`EJitRegisterBitcode.cpp` 中以下函数目前是 `static`，需要暴露为公共 API（放在头文件或新增 `EJitCrossLink.h` 中）：

| 函数 | 用途 |
|---|---|
| `collectEntryFunctions()` | 找 ejit_entry |
| `computeTransitiveClosure()` | 计算传递闭包 |
| `preOptimizeBitcode()` | AOT 预优化 |
| `reAnnotateMayConst()` | 恢复 metadata |
| `embedBitcode()` | 创建 @__ejit_bitcode |
| `generateRegisterCall()` | 创建 ejit_auto_register |
| `collectReferencedGlobals()` | 收集引用的全局变量 |

### 4.4 Driver 集成点

**两处集成**：

1. **clang driver**: `clang/lib/Driver/ToolChains/Gnu.cpp` — `Linker::ConstructJob` 中调 `runEJitCrossLink`，生成的 `.bc` 追加到 ld 命令行参数。
2. **ld.lld**: `lld/ELF/Driver.cpp` — `LinkerDriver::link()` 中，`parseFiles` 之前，扫描 OPT_INPUT 收集路径，调用 `runEJitCrossLink`，生成的 `.bc` 通过 `addFile` 加入链接。同时设置 `ctx.ejitCrossLinked = true`，触发 `InputFiles.cpp` 中丢弃原始 `.ejit_cross` section。

```
# clang 驱动
clang -fejit-cross-inline -r tu_a.o tu_b.o -o combined.o

# 直接用 ld.lld
ld.lld -r tu_a.o tu_b.o -o combined.o
```

### 4.5 每函数独立 bitcode 设计

**为什么不共用一份 `@__ejit_bitcode`？**

如果所有 ejit_entry 共用一份 bitcode，JIT 编译 `jit_board_check` 时需要 parse 整个 Module（含其他 3 个函数的全部代码和跨 TU 内联来的代码），浪费 parse 时间和内存。

**方案**：对每个 ejit_entry 单独提取闭包 + 独立序列化：

```llvm
@__ejit_bitcode_board = internal constant [N] <jid_board_check + closure>  ; 只含自己的闭包
@__ejit_bitcode_cell  = internal constant [M] <jid_cell_check + closure>
@__ejit_bitcode_chain = internal constant [K] <jid_chain_check + closure>
@__ejit_bitcode_prior = internal constant [L] <jid_priority + closure>
```

**JIT 编译时**：按 funcIdx 取出对应那份 bitcode → parse → `getFunction(funcName)` → 编译。Module 里只含当前函数 + 依赖，无冗余。

**运行时无需改动**：`EJitModuleLoader` 本来按 funcName 独立存储，每个 entry 有自己的 data 指针。多函数共用同一份 bitcode 是当前行为，改成独立后运行时完全无感。

```cpp
// EJitModuleLoader 内部 (已有逻辑，无需修改)
struct BitcodeEntry {
  std::string funcName;
  const uint8_t *data;   // 各指向不同的 @__ejit_bitcode_xxx
  size_t size;
};
```

---

## 5. 构建依赖

### 5.1 LLVMEmbeddedJIT (CMakeLists.txt)

```cmake
add_llvm_component_library(LLVMEmbeddedJIT
  EJitPassOptions.cpp
  EJitRegisterBitcode.cpp
  ...

  LINK_COMPONENTS
  Core
  IRReader
  Support
  TransformUtils
  BitWriter
  BitReader      # 新增: parseBitcodeFile (链接期)
  Object         # 新增: 读取 ELF section
  Linker         # 新增: linkInModule
  )
```

### 5.2 clangDriver

`EJitCrossLink.cpp` 编入 clang 自身（clang 已链接所有依赖）。

---

## 6. 与其他模块的交互

| 模块 | 影响 |
|---|---|
| **PASS3 (WrapperGen)** | 不受影响 — wrapper 仍正常生成 |
| **PASS2 (RegisterPeriod)** | 不受影响 |
| **运行时 (libLLVMEJIT)** | 不受影响 — `ejit_init` 仍通过 `ejit_register_bitcode` 加载 `@__ejit_bitcode` |
| **JIT 编译** | 受益 — bitcode 中跨 TU 子函数已内联，PASS6 可以追到 may_const load |
| **默认编译路径** | 不受影响 — 不传 `-fejit-cross-inline` 时行为完全不变 |

---

## 7. 错误处理 & 边界情况

### 7.1 错误处理

| 场景 | 策略 |
|---|---|
| 无 .ejit_cross section 的链接 | 跳过，正常链接 |
| bitcode 解析失败 | 跳过该 section，记录 warning |
| 合并后无 ejit_entry | 跳过，不生成临时 .o |
| 临时 .o 生成失败 | 记录 error，fallback 到正常链接 |
| `-r` 链接的输入有旧的 `@__ejit_bitcode` | `.ejit_cross` 优先替换旧的 bitcode |

### 7.2 与 `-flto` 冲突

`-flto` / `-flto=thin` 和 `-fejit-cross-inline` 是两套独立的跨模块机制，同时使用会导致：
- 两套机制都做内联，结果不确定
- LTO 的 bitcode section (`.llvmbc`) 和 `.ejit_cross` 共存，语义混乱

**策略**：clang driver 检测到同时出现时直接报错。

```cpp
if (Args.hasArg(options::OPT_fejit_cross_inline) &&
    Args.hasArg(options::OPT_flto_EQ)) {
  C.getDriver().Diag(clang::diag::err_drv_argument_not_allowed_with)
      << "-fejit-cross-inline" << "-flto";
}
```

### 7.3 `.ejit_cross` section 自动清除

Cross-link 处理成功后，`ctx.ejitCrossLinked = true`。`InputFiles.cpp` 中检查此标记：
- 如果为 true 且 section 名为 `.ejit_cross` → `sections[i] = &InputSection::discarded`
- 原始 `.o` 中的 `.ejit_cross` section 被丢弃，不出现在输出中
- `-r` 和最终链接都生效

### 7.4 被调用的 TU 也需要 `-fejit-cross-inline`

只有 `ejit_entry` 所在 TU 用该 flag 是不够的——`child_func()` 定义的 TU 也必须用，否则链接期看不到它的 IR。

```
clang -fejit-cross-inline -c tu_a.c -o tu_a.o   # ejit_entry 在此
clang -fejit-cross-inline -c tu_b.c -o tu_b.o   # child_func 在此, 必须
```

---

## 8. 与 ThinLTO/FullLTO 方案对比

| 维度 | ThinLTO/FullLTO | `-fejit-cross-inline` |
|---|---|---|
| AOT 代码影响 | 全局跨模块优化 | 无影响 |
| 编译时间 | 增加 (summary + 多阶段) | 仅编译期多一次序列化 |
| 链接时间 | 增加 (LTO backend) | 增加 (合并 + 内联) |
| JIT bitcode 质量 | 跨模块内联 ✓ | 跨模块内联 ✓ |
| `!ejit.may_const` | 需要 noinline 保护 | 天然保留 (完整 Module) |
| 构建系统改动 | clang flag | 同 |
| `-r` 兼容性 | 复杂 | 简单 (标准 ELF section) |

---

## 9. 文件改动清单

| 文件 | 改动 |
|---|---|
| `clang/include/clang/Driver/Options.td` | +4: `-fejit-cross-inline` |
| `clang/lib/Driver/ToolChains/Clang.cpp` | +10: 编译选项传递 + `-flto` 冲突检测 |
| `clang/lib/Driver/ToolChains/Gnu.cpp` | +14: Linker::ConstructJob |
| **新建** `clang/lib/Driver/EJitCrossLink.cpp` | ~420: 链接期处理核心（clang driver 副本） |
| **新建** `clang/lib/Driver/EJitCrossLink.h` | ~18: API 声明 |
| `clang/lib/Driver/CMakeLists.txt` | +8: 新增源 + LINK_COMPONENTS |
| **新建** `lld/ELF/EJitCrossLink.cpp` | ~420: 链接期处理核心（lld 副本） |
| **新建** `lld/ELF/EJitCrossLink.h` | ~18: API 声明 |
| `lld/ELF/Driver.cpp` | +15: link() 中调 runEJitCrossLink |
| `lld/ELF/InputFiles.cpp` | +10: 丢弃已消费的 .ejit_cross |
| `lld/ELF/Config.h` | +3: ctx.ejitCrossLinked |
| `lld/ELF/CMakeLists.txt` | +3: 新增源 + LINK_COMPONENTS |
| `llvm/lib/Passes/PassBuilderPipelines.cpp` | +22: flag + 构造函数传参 |
| `llvm/include/.../EJitPasses.h` | +4: CrossInline 成员 |
| `llvm/.../EJitRegisterBitcode.cpp` | +31: 分支 + embedBitcodeInSection |

总计约 **1400 行**新增代码。

---

## 10. 未来扩展

- 支持从 archive (.a) 中提取 `.ejit_cross`
- 增量链接：缓存已处理的跨 TU bitcode 避免重复合并
- `-fejit-cross-inline=thin` 变体：利用 ThinLTO summary 实现按需导入

---

*文档版本: 1.0*
*创建日期: 2026-07-29*
