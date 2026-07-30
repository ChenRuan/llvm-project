# EJIT Cross-TU Inlining 设计文档

**版本**: 1.2
**日期**: 2026-07-30
**关联**: SPEC4.md, PLAN4.md, PASS1_EJitRegisterBitcode.md

---

## 1. 概述

### 1.1 背景

当前 `ejit_entry` 函数调用其他 TU 的子函数时，PASS1 在编译期运行，只能看到当前 TU。跨 TU 的子函数在 bitcode 中表现为 `declare external` 声明，PASS1 无法将其内联。JIT 编译时仍有跨函数调用开销。

之前的方案是开启全局 ThinLTO，但 ThinLTO 会对整个程序生效——AOT 代码也被跨模块优化，增加了编译时间并影响了 AOT 代码的确定性。

### 1.2 目标

提供两种只作用于 `__ejit_bitcode` 的跨 TU 优化策略，AOT 代码仍走普通编译流程：

- `-fejit-cross-inline`：按 LLVM 成本模型内联普通 helper；
- `-fejit-cross-jit-helpers`：仅展开 `always_inline`，普通 helper 的传递
  闭包随 entry 进入 JIT module，在 code pool 内生成特化版本。

### 1.3 核心思路

将 PASS1 拆为两阶段：

- **编译期**：完整 Module IR 嵌入 `.ejit_cross` ELF section（不提取闭包，不生成 `__ejit_bitcode`）
- **链接期**：读取所有 `.ejit_cross` section → 合并 → 内联跨 TU 子函数 → 生成最终的 `__ejit_bitcode`

---

## 2. 整体流程

```
┌─────────────────────────────────────────────────────────────────┐
│  编译期 (-fejit-cross-inline / -fejit-cross-jit-helpers -c)     │
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
│  链接期 (-fejit-cross-inline *.o -o output, 需 -fuse-ld=lld)     │
│  (-r 部分链接和最终链接处理逻辑完全相同)                          │
│                                                                 │
│  ld.lld (唯一 owner, 仅在 --ejit-cross-inline 时执行一次):       │
│    1. 扫描所有输入 .o, 提取 .ejit_cross section                 │
│    2. parseBitcodeFile → 每个 section 解析为一个 Module          │
│    3. llvm::Linker::linkInModule() → 合并所有 Module            │
│    4. 找 ejit_entry → computeTransitiveClosure (保守引用收集)    │
│    5. helper 策略:                                               │
│       cross-inline: AlwaysInliner + 成本模型 ModuleInliner       │
│       jit-helpers: 仅 AlwaysInliner, 普通 helper 保留为定义      │
│    6. reAnnotateMayConst (复用 EJitCommon.h canonical resolver)  │
│    7. 对每个 ejit_entry 单独:                                   │
│       CloneModule → computeTransitiveClosure → trim             │
│       → verifyModule → 独立 @__ejit_bitcode_<name>             │
│    8. generateRegistryTable (registry 全部常量属于 TmpM;         │
│       external func/global 在 TmpM 内建 declaration)            │
│    9. verifyModule(TmpM) → 失败则链接失败                       │
│   10. WriteBitcodeToFile → 临时 .bc; 链接后由 Driver 清理        │
│   11. lld 链接 (原始 .o + 临时 .bc), 丢弃原始 .ejit_cross        │
│                                                                 │
│  任一阶段失败 → 链接失败 (绝不静默 fallback 到 AOT)             │
│  输出: output 中无 .ejit_cross, 只有一套 @__ejit_bitcode         │
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

### 2.2 JIT-local helper 模式

```
clang -fejit-cross-jit-helpers -c entry.c -o entry.o
clang -fejit-cross-jit-helpers -c helper.c -o helper.o
clang -fuse-ld=lld -fejit-cross-jit-helpers entry.o helper.o -o app
```

clang cc1 仍使用内部 `-ejit-cross-inline` 生成同一种 `.ejit_cross` 输入；
driver 额外向 lld 传递 `--ejit-cross-jit-helpers`。因此两种策略可以使用同一批
对象文件，区别只发生在 lld 合并后的 helper policy。

对 `A -> B -> C`：

1. 链接器将全部 entry 的闭包存入一份 repository bitcode，B、C 模板只保存
   一次；
2. SCC manifest 记录每个 entry 依赖的函数强连通分量；
3. runtime 以目标 entry 为唯一 root，通过 GlobalDCE 重建其临时闭包；
4. IPSCCP 把 entry 已固定的维度参数逐层传播到 B、C；
5. 第二轮 InstCombine + StructFieldPass 折叠 helper 内的 may_const；
6. A、B、C 一起 codegen/JITLink，内部调用生成 code-pool 内的直接 `BL`。

repository 复用的是最终镜像中的 helper bitcode 模板。每个 specialization
仍拥有独立的临时 IR、机器码和 JITDylib，因此不同常量可以生成不同的 B/C
版本且不会串号。详细格式与校验协议见
`EJIT_BITCODE_REPOSITORY_SCC.md`。

限制：

- helper 若通过运行时不可推断的函数指针调用，闭包只能保留静态可见候选，
  IPSCCP 也不能假定参数恒定；
- helper 被多个 entry 使用时，各 entry specialization 仍各自生成一份；
- helper 与 entry 同属一个 JITLink graph/code allocation，现有 2 MiB pool 下
  可使用 AArch64 直接 `BL`；若未来单个 graph 超过 `BL` 范围，需要重新引入
  range extension thunk；
- fully folded 的纯 helper 调用可能被后续 DCE 完全消除，这比保留 `BL` 更优，
  不应视为模式失效。

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
  // 但允许链接器 GC（--gc-sections）。ld.lld 在 cross-link 处理后仅对
  // EJitCrossLinkResult::consumedFiles 中的输入主动丢弃此 section。
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

唯一实现文件：**`lld/ELF/EJitCrossLink.cpp`**（clang driver 复制版已删除）

```cpp
/// 链接期跨 TU 内联处理。
/// 从所有输入 .o 中提取 .ejit_cross section，合并、内联、生成 @__ejit_bitcode，
/// 写入临时 .bc，并返回路径及被精确消费的输入集合。
/// 无 .ejit_cross 时返回空结果；检测后的任何失败返回 Error。
Expected<EJitCrossLinkResult>
runEJitCrossLink(ArrayRef<std::string> InputFiles, StringRef TargetTriple,
                 StringRef SaveTempsPrefix = {});
```

### 4.2 处理流程

```
runEJitCrossLink(InputFiles, TargetTriple):

  1. 快速扫描: 检查是否有任何输入包含 .ejit_cross section
     └─ 无 → return EJitCrossLinkResult{}

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
     computeTransitiveClosure(EntryFuncs) → ClosureFuncs + ClosureGlobals
     扫描 operand、常量表达式/聚合、alias/ifunc、全局 initializer
     deleteBody/setInitializer(nullptr) + GlobalDCE 安全裁剪

  5. 内联跨 TU 子函数:
     将 JIT-only composite definitions 标记 dso_local
     AlwaysInlinerPass + ModuleInlinerWrapperPass (成本模型)

  6. 轻量 cleanup + metadata 恢复:
     Promote → InstCombine → SimplifyCFG
     reAnnotateMayConst(Composite)

  7. 对每个 ejit_entry 单独提取闭包 + 序列化:
     for each EntryFunc:
       PerFuncModule = CloneModule(Composite)
       computeTransitiveClosure({EntryFunc}, Closure, Globals)
       安全裁剪 PerFuncModule 中非闭包函数/全局变量
       externalize mutable globals
       collectExternalSymbols → 在 TmpModule 中建立稳定 declaration
       verifyModule(*PerFuncModule)
       serializeToBitcode(*PerFuncModule) → BC

  8. 生成临时 Module (含 N 个 @__ejit_bitcode_<name> + 单一 registry):
     TmpModule = new Module("ejit_cross_link", Ctx)
     复制 DataLayout + TargetTriple
     for each EntryFunc + its BC:
       embedBitcode(TmpModule, BC, "__ejit_bitcode_" + funcName)
     generateRegistryTable(TmpModule, entries, external symbols)
     verifyModule(TmpModule)

  9. 写入临时 .bc:
     WriteBitcodeToFile(TmpModule) → 临时 .bc 文件
     lld 原生支持 .bc 输入，无需编译为 .o

  10. return {tmpfile path, consumed input files}
```

### 4.3 与 PASS1 共享的语义

cross-link 的闭包、内联和 registry 生成属于 lld，不再复制一份 clang/PASS1
实现。`may_const` 指针形态解析直接复用 `EJitCommon.h` 中的
`ejitMayConstFieldOffset` 与 `ejitAccessFitsMayConstField`，保证与运行时优化 pass
使用同一套保守规则。

### 4.4 Driver 集成点（PR #102 hardening: ld.lld 为唯一 owner）

为保证跨 TU 合并/注册**恰好执行一次**，`ld.lld` 是唯一的 cross-link owner；clang
driver 自身**不再**执行任何合并。协议如下：

1. **clang driver**: `clang/lib/Driver/ToolChains/Gnu.cpp` — `Linker::ConstructJob`
   中，当 `-fejit-cross-inline` 生效时用 `ToolChain::GetLinkerPath(&LinkerIsLLD)`
   判断链接器：
   - 若不是 lld → `err_drv_argument_only_allowed_with` 硬报错（`-fejit-cross-inline`
     只允许配合 `-fuse-ld=lld`）。绝不生成 GNU ld 无法消费的 `.bc`，也绝不把巨大的
     `.ejit_cross` 静默留在最终产物。
   - 若是 lld → 仅向链接命令行追加 `--ejit-cross-inline`，把处理交给 lld。
2. **ld.lld**: `lld/ELF/Driver.cpp` — `LinkerDriver::link()` 中，`parseFiles` 之前，
   **仅当 `--ejit-cross-inline` 存在**（`ctx.arg.ejitCrossInline`）时扫描 OPT_INPUT，
   调用 `runEJitCrossLink`：
   - 返回 `Expected<EJitCrossLinkResult>`：`Error` → `ErrAlways(ctx)` 令链接失败；
     空 `tempPath` → 无 `.ejit_cross`（正常跳过，默认行为完全不变）；非空 →
     临时 `.bc` 路径和被精确消费的输入文件集合。
   - 生成的 `.bc` 通过 `addFile` 加入链接；`InputFiles.cpp` 只丢弃
     `consumedFiles` 中输入的 `.ejit_cross` section，不能用全局布尔值误删后续由
     `-l`、archive 或 linker script 引入但未处理的 section。
   - 临时 `.bc` 在 `parseFiles` 读入内存后由 Driver 立即 `sys::fs::remove` 删除，
     并在创建时 `RemoveFileOnSignal` 注册，保证不会在 `/tmp` 永久遗留。

> clang 复制版 `clang/lib/Driver/EJitCrossLink.cpp` 已删除，因此实现只剩 lld 一份，
> 不存在两份实现漂移（Area 6）。`.ejit_cross` 里的 may_const 语义直接复用
> `EJitCommon.h` 的 `ejitMayConstFieldOffset` / `ejitAccessFitsMayConstField`
> canonical resolver，不再维护过时的简化版。

```
# clang 驱动（必须使用 lld）
clang -fejit-cross-inline -fuse-ld=lld -r tu_a.o tu_b.o -o combined.o

# 直接用 ld.lld
ld.lld --ejit-cross-inline -r tu_a.o tu_b.o -o combined.o
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

clang driver 只负责校验选中的 linker 并转发 `--ejit-cross-inline`；helper
模式再追加 `--ejit-cross-jit-helpers`。driver 不再链接 BitReader/Object/Linker
等 cross-link 实现依赖。

---

## 6. 与其他模块的交互

| 模块 | 影响 |
|---|---|
| **PASS3 (WrapperGen)** | 不受影响 — wrapper 仍正常生成 |
| **PASS2 (RegisterPeriod)** | 不受影响 |
| **运行时 (libLLVMEJIT)** | ABI 不变；helper 模式复用 internalize + IPSCCP |
| **JIT 编译** | inline 模式展开 helper；helper 模式生成同 module 特化 helper |
| **默认编译路径** | 不受影响 — 不传两个 EJIT cross-TU flag 时行为完全不变 |

---

## 7. 错误处理 & 边界情况

### 7.1 错误处理（PR #102: 检测到 .ejit_cross 后绝不静默吞错）

区分「输入没有 `.ejit_cross`」（正常跳过）与「已检测到 `.ejit_cross` 但处理失败」
（硬失败）。`runEJitCrossLink` 返回 `Expected<EJitCrossLinkResult>`，每个错误都带
**输入文件名 + 处理阶段 + 底层 llvm::Error** 文本，由 `ld.lld` 转成致命链接错误。

| 场景 | 策略 |
|---|---|
| 无 .ejit_cross section 的链接 | 返回空结果，正常链接（默认行为不变） |
| 普通非对象参数或不含该 section 的输入 | 跳过，非错误 |
| **显式 archive 成员含 .ejit_cross** | 扫描并合并选中的 archive 成员 |
| **bitcode 解析失败 / section 读取失败** | **链接失败**（带文件名+阶段+Error） |
| **`Linker::linkInModule` 合并冲突** | **链接失败**（检查返回值） |
| 合并后无 ejit_entry | **链接失败**（已承诺有 .ejit_cross，却无可注册入口） |
| **per-entry / registry `verifyModule` 失败** | **链接失败**，不写出任何 bitcode |
| **临时 .bc 创建 / 写入失败** | **链接失败**（删除半成品临时文件） |

> 旧实现在上述多数场景 `continue` 或返回空串静默 fallback 到 AOT；现已全部改为
> 通过 `Error` 传播到链接器用户。clang 与 lld 两条入口的错误语义一致（clang 仅在
> 非 lld 链接器时报错，其余交给 lld）。

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

Cross-link 返回精确的 `consumedFiles`，Driver 写入
`ctx.ejitCrossConsumedFiles`。`InputFiles.cpp` 只有在当前 input identifier 命中该
集合且 section 名为 `.ejit_cross` 时才设为 `InputSection::discarded`：
- 已处理的原始 `.o` section 不进入输出，`-r` 和最终链接都生效；
- 显式输入的 archive 会扫描并消费选中成员；
- `-l` 或 linker script 后续引入的未处理 section 不会被全局状态误删，而是在
  parse 后得到明确的 unsupported 诊断。

### 7.4 被调用的 TU 也需要相同 cross-TU 编译模式

只有 `ejit_entry` 所在 TU 用该 flag 是不够的——`child_func()` 定义的 TU 也必须用，否则链接期看不到它的 IR。

```
clang -fejit-cross-inline -c tu_a.c -o tu_a.o   # ejit_entry 在此
clang -fejit-cross-inline -c tu_b.c -o tu_b.o   # child_func 在此, 必须
```

使用 JIT-local helper 模式时，将两条命令中的 flag 都替换为
`-fejit-cross-jit-helpers`。

---

## 8. 与 ThinLTO/FullLTO 方案对比

| 维度 | ThinLTO/FullLTO | `-fejit-cross-inline` | `-fejit-cross-jit-helpers` |
|---|---|---|---|
| AOT 代码影响 | 全局跨模块优化 | 无影响 | 无影响 |
| 编译时间 | 增加 (summary + 多阶段) | 多一次序列化 | 多一次序列化 |
| 链接时间 | 增加 (LTO backend) | 合并 + 内联 | 合并 + mandatory inline |
| JIT bitcode | helper 展开 | 成本模型展开 | 传递 helper 闭包 |
| 代码尺寸倾向 | 视 LTO 决策 | entry 可能膨胀 | helper 每 specialization 一份 |
| 内部调用 | 可能消失 | 多数消失 | code pool 内直接 `BL` |
| `-r` 兼容性 | 复杂 | 标准 ELF section | 标准 ELF section |

---

## 9. 文件改动清单

PR #102 hardening 后（ld.lld 为唯一 owner）：

| 文件 | 改动 |
|---|---|
| `clang/include/clang/Driver/Options.td` | 两种 cross-TU 策略 flag |
| `clang/lib/Driver/ToolChains/Clang.cpp` | 两种模式生成相同 `.ejit_cross` + `-flto` 冲突检测 |
| `clang/lib/Driver/ToolChains/Gnu.cpp` | 校验 lld，转发 cross-link 与 helper policy |
| ~~`clang/lib/Driver/EJitCrossLink.cpp` / `.h`~~ | **已删除**（去重：实现只剩 lld 一份） |
| `clang/lib/Driver/CMakeLists.txt` | 移除 `EJitCrossLink.cpp` 及其专用 LINK_COMPONENTS |
| `lld/ELF/EJitCrossLink.cpp` | 保守闭包、inline/JIT-helper policy、registry、错误传播 |
| `lld/ELF/EJitCrossLink.h` | API 声明（`Expected<EJitCrossLinkResult>`） |
| `lld/ELF/Options.td` | cross-link gate + JIT-helper policy |
| `lld/ELF/Config.h` | 两种模式状态 + 精确消费集合 |
| `lld/ELF/Driver.cpp` | 解析 flag；gated 调用 + `Expected`/`Error` 处理；临时文件清理（`+Signals.h`） |
| `lld/ELF/InputFiles.cpp` | 仅丢弃精确匹配已消费输入的 `.ejit_cross` |
| `lld/ELF/CMakeLists.txt` | 新增源（不变） |
| `llvm/lib/Passes/PassBuilderPipelines.cpp` | flag + 构造函数传参（不变） |
| `llvm/include/.../EJitPasses.h` | CrossInline 成员（不变） |
| `llvm/.../EJitRegisterBitcode.cpp` | 分支 + embedBitcodeInSection（不变） |
| `lld/test/ELF/ejit-cross-inline.ll` | 两种 policy、嵌套闭包、AArch64 direct `BL` |
| `llvm/unittests/.../EJitRuntimeTest.cpp` | IPSCCP 穿透 A→B→C noinline helper |
| `ejit_test/ejit_jit_local_helpers_sre_*.c` | SRE worker-first 双 TU 功能 demo |

### 9.1 闭包支持的引用形态（Area 4）

`computeTransitiveClosure` / `collectFromConstant` 统一、保守地收集引用：扫描所有
指令 operand，穿透 `stripPointerCasts` / `ConstantExpr` / `ConstantAggregate`，解析
`GlobalAlias` 与 `GlobalIFunc` 目标，扫描已保留全局的 initializer（函数指针表）。
`CallInst` / `InvokeInst` / `CallBrInst`（均为 `CallBase`，callee 是 operand）自然覆盖，
bitcast 后的 callee 也覆盖。运行时传入的真正间接目标无法静态推断，间接调用保持不变；
module-owned 函数表目标由 initializer 扫描覆盖。trim 先 `deleteBody`，再由 `GlobalDCE`
安全回收，Debug/Release 都不会断言或留下 dangling reference。

---

## 10. 未来扩展

- 支持从 archive (.a) 中提取 `.ejit_cross`
- 增量链接：缓存已处理的跨 TU bitcode 避免重复合并
- `-fejit-cross-inline=thin` 变体：利用 ThinLTO summary 实现按需导入

---

*文档版本: 1.0*
*创建日期: 2026-07-29*
