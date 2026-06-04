# EmbeddedJIT 轻量 AArch64 执行后端（Light Backend）设计文档

> 状态：实验性（experimental）/ 可选（opt-in）
> 适用分支：`ejit_light_backend_proto`（基线 `dong/ejit_dev_spec4`）
> 默认构建**不包含**本后端，ORC/JITLink 仍是唯一且默认的执行路径。

---

## 1. 背景与动机

EmbeddedJIT 现有的执行路径是：

```
bitcode/IR → SPEC4/PASS7 特化优化 → ORC / JITLink / LLJIT → 函数指针
```

ORC/JITLink/EPC 功能完整、正确性有保障，但依赖较重：需要完整的 LLVM
CodeGen（目标 `*CodeGen/*Desc/*Info`）、RuntimeDyld/JITLink、ExecutionSession、
内存管理器、（在某些场景下）host 进程符号解析、mmap、pthread 等。对于
**裸核 / SRE / 极小包**场景，这些依赖既增加体积，也增加移植成本。

本后端（Light Backend）提供一条**可选**的轻量执行路径：

```
bitcode/IR → SPEC4/PASS7 特化优化 → EJIT Light AArch64 发射器
           → CodeAllocator → 函数指针
```

它**绕过** ORC / JITLink / EPC / RuntimeDyld / host 符号解析，直接把经过
特化的 LLVM IR 发射为 AArch64 机器码。其思路迁移自 EasyJIT(LLVM15) 的
`light_codegen` AArch64 后端，但**没有照搬** EasyJIT 的 SRE 调试 hack
（`SRE_MemDbgAlloc`/`SRE_MmuMap`/mmap 调试路径等），并适配到 LLVM21。

### 设计目标

- **可选**：默认 OFF，不影响现有构建与 ORC 路径。
- **极小依赖**：发射器是手写编码器，**不依赖 LLVM 目标 CodeGen**。即便
  构建只包含 X86 target，只要宿主是 aarch64，light 路径仍能发射并执行。
- **不破坏**：不引入 MCJIT；不改动 ORC 默认行为；不破坏 `LLVMEJIT`、
  `ejit_baremetal`、`ejit_test/lipo`。
- **能力边界清晰**：两层校验（候选粗筛 + 发射器细校），所有拒绝都给出原因，
  绝不 abort / longjmp。

---

## 2. 总体架构

```
                ┌─────────────────────────── EJitCompileDriver::getOrCompile ───────────────────────────┐
                │                                                                                        │
  funcName ──▶  │  cache 命中? ──是──▶ 返回                                                               │
  dims          │       │否                                                                              │
                │  时间窗校验 → 取 bitcode → 构造 SpecializationContext                                    │
                │       │                                                                                │
                │  backendMode?                                                                          │
                │   ├─ Light / Auto ─▶ tryCompileLight() ──成功──▶ cache.put → 返回                      │
                │   │                        │失败                                                        │
                │   │            Light(强制): 直接返回 nullptr（不回退）                                   │
                │   │            Auto:        记录原因，继续走 ORC ↓                                       │
                │   └─ Orc(默认) ───────────────────────────────────┐                                   │
                │  if(!syncEngine_) 报错返回                          │                                   │
                │  syncEngine_->loadBitcodeModule + lookup（ORC 路径）│                                   │
                │  cache.put → 返回                                  ▼                                   │
                └────────────────────────────────────────────────────────────────────────────────────┘

  tryCompileLight():
    parseBitcodeFile → EJitOptimizer.runPipeline(SPEC4/PASS7)
      → M.getFunction(fnName) → 收集全局符号(Task G)
      → light::compileAArch64Light(F, globals, HostMmapCodeAllocator)
      → 函数指针
```

关键点：**light 路径与 ORC 引擎解耦**。`tryCompileLight` 自己 parse bitcode、
自己跑 `EJitOptimizer`（与 ORC IRTransformLayer 用的是同一套特化 pipeline），
只用到 `PeriodArrayRegistry` 与本地 user-symbol 表，**不需要** `EJitOrcEngine`
存在。因此即使 ORC 引擎创建失败（例如宿主目标未注册到 LLVM CodeGen），
light 路径仍可工作。

---

## 3. 代码结构

### 头文件（`llvm/include/llvm/ExecutionEngine/EJIT/LightBackend/`）

| 文件 | 作用 |
|------|------|
| `EJitLightAArch64.h` | 原始发射器接口 `llvm::ejit::light::raw`：`emit()`、`Status`、`Result`、`GlobalSymbol`。纯函数式发射，**与 LLVM 执行引擎无关**。 |
| `EJitLightCodeAllocator.h` | 可执行内存分配抽象 `CodeAllocator`；两个实现：`HostMmapCodeAllocator`（POSIX mmap）与 `StaticSlabCodeAllocator`（在调用者缓冲里 bump 分配，可注入 flush/mprotect 钩子，无 POSIX 依赖，裸核友好）。 |
| `EJitLightBackend.h` | 对外公开 API：`compileAArch64Light()`、`isLightBackendCandidate()`、`GlobalSymbol`、`Status`、`CompileResult`。 |

### 实现（`llvm/lib/ExecutionEngine/EJIT/LightBackend/`）

| 文件 | 作用 |
|------|------|
| `EJitLightAArch64.cpp`（~3350 行） | 从 EasyJIT 迁移的 AArch64 发射器：纯编码器（`encAddSubImm`/`encMovz16`/`encFaddS` …）全部保留；triple 闸门改为 `getTargetTriple().str()`；**删除** EasyJIT 的 SRE `compile()` 与所有 weak SRE extern。 |
| `EJitLightCodeAllocator.cpp` | 两个分配器的实现。 |
| `EJitLightBackend.cpp` | 候选检查 + 编译驱动；`raw::Status` → 公开 `Status` 的映射；把 IR 发射到探测缓冲，再分配真实可执行内存并 memcpy（位置无关），finalize 后返回指针。 |

### Runtime 接入（已有文件改动）

| 文件 | 改动 |
|------|------|
| `EJitOptions.h` | 新增 `enum class BackendMode { Orc, Light, Auto }` 与 `Config::backendMode`（默认 `Orc`）。 |
| `EJitRuntime.h/.cpp` | 新增 C API：`ejit_backend_mode_t` + `ejit_set/get_backend_mode` + `ejit_light_backend_available`。 |
| `EJit.h/.cpp` | 新增 `setBackendMode/getBackendMode` 与静态 `isLightBackendAvailable`；并修复 `EJitCompileDriver` 构造时误传构造参数（临时量）而非成员 `config_` 的悬垂引用 bug。 |
| `EJitCompileDriver.h/.cpp` | `getOrCompile` 中新增 light 分支（在 `syncEngine_` 可用性检查之前）；自包含的 `tryCompileLight`；持有 `HostMmapCodeAllocator` 与本地 user-symbol 表。 |
| `EJitRuntimeState.h` | `PeriodArrayRegistry::getAllArraysByPeriod()` 只读枚举（供 Task G 收集全局地址）。 |

所有 light 专属代码都被 `#ifdef EJIT_LIGHT_BACKEND` 包裹；OFF 构建下这些
TU/分支完全不参与编译，默认构建产物不受影响（见 §8 体积/验证）。

---

## 4. 构建开关

```cmake
# llvm/lib/ExecutionEngine/EJIT/CMakeLists.txt
option(EJIT_ENABLE_LIGHT_BACKEND
  "Enable EmbeddedJIT experimental AArch64 light backend" OFF)
```

- **OFF（默认）**：不编译 LightBackend 源文件，不定义宏，ORC 为唯一后端。
- **ON**：把 3 个 LightBackend 源加入 `LLVMEJIT`，并 `target_compile_definitions`
  公开宏 `EJIT_LIGHT_BACKEND=1`；同时启用两个测试目标。

启用方式：

```bash
cmake -S llvm -B build-ejit -DEJIT_ENABLE_LIGHT_BACKEND=ON
cmake --build build-ejit --target LLVMEJIT
```

### CMake 目标

| 目标 | 说明 |
|------|------|
| `ejit-light-backend-test` / `check-ejit-light-backend` | 发射器/分配器**单元级**独立测试（不依赖 lit/gtest）。 |
| `ejit-light-runtime-test` / `check-ejit-light-runtime` | 走 `ejit_*` C API 的**运行时端到端**测试（任务 F/G）。 |

---

## 5. API

### C API（`EJitRuntime.h`）

```c
typedef enum {
  EJIT_BACKEND_ORC   = 0,  /* 默认：ORC/JITLink/LLJIT */
  EJIT_BACKEND_LIGHT = 1,  /* 可选：AArch64 light 后端（无 ORC） */
  EJIT_BACKEND_AUTO  = 2,  /* 先试 light，不支持则回退 ORC */
} ejit_backend_mode_t;

void                ejit_set_backend_mode(ejit_backend_mode_t mode);
ejit_backend_mode_t ejit_get_backend_mode(void);
bool                ejit_light_backend_available(void); /* 构建是否带 light */
```

> 当 `LLVMEJIT` 未启用 light 后端时：`ejit_light_backend_available()` 返回
> `false`，且 `LIGHT/AUTO` 行为等同 `ORC`。

### C++ API（`EJit.h`）

```cpp
void        EJit::setBackendMode(BackendMode);
BackendMode EJit::getBackendMode() const;
static bool EJit::isLightBackendAvailable();
```

### 后端内部 API（`LightBackend/EJitLightBackend.h`）

```cpp
// 把（已特化的）F 发射为 AArch64 机器码，返回可调用函数指针。
Expected<void *> compileAArch64Light(llvm::Function &F,
                                     ArrayRef<GlobalSymbol> Globals,
                                     CodeAllocator &Allocator,
                                     CompileResult *OutResult);

// 候选粗筛：明显不支持的 IR 形状提前拒绝，给出 reason。
bool isLightBackendCandidate(llvm::Function &F, std::string *Reason);
```

---

## 6. 支持 / 不支持的 IR 能力表

light 后端能力比 LLVM CodeGen 窄很多，采用**两层校验**：候选检查
（`isLightBackendCandidate`）做粗筛，发射器内部再做细校；两层都可能拒绝。

### 支持（典型）

- 标量整数运算（add/sub/mul/and/or/xor/shift/比较/选择等）。
- 标量浮点（`float`/`double`：fadd/fsub/fmul、比较、`fmin`/`fmax`、转换等）。
- 控制流：基本块、条件/无条件跳转（`br`）、`phi`。
- 内存：`alloca`、`load`/`store`、`getelementptr`（含动态下标/多维数组）。
- 栈传参与寄存器溢出（spill）下的多参数函数。
- 全局符号引用：通过 `GlobalSymbol` 表绑定**绝对地址**（period 数组基址、
  静态变量、用户注册符号）。
- 受支持的 intrinsic：`memcpy`、`fmuladd`、`dbg.*`、`lifetime.*`（其余拒绝）。

### 不支持（候选检查直接拒绝，附 reason）

| 拒绝原因（reason） | 场景 |
|---|---|
| `triple is not aarch64*/arm64* (or is ILP32)` | 非 aarch64/arm64，或 ILP32（aarch64_32/arm64_32） |
| `varargs not supported` | 可变参数函数 |
| `vector return type / argument / instruction ... not supported` | 任何 vector 类型/指令/操作数 |
| `aggregate return type / aggregate-by-value argument not supported` | 结构体按值返回/传参 ABI |
| `exception handling not supported` | `invoke`/`resume`/`landingpad` |
| `indirectbr not supported` | `indirectbr` |
| `switch not supported (lower to branches first)` | `switch`（需先 lower 成分支） |
| `va_arg not supported` | `va_arg` |
| `indirect call not supported` | 间接调用（函数指针调用） |
| `unsupported intrinsic: <name>` | 不在白名单内的 intrinsic |

> 注意：候选检查通过**不代表**一定能发射成功；发射器仍可能在更细粒度
> 拒绝（返回 `Status::Unsupported/TooLarge` 等）。

---

## 7. 关键模型

### 7.1 字节序模型

发射器对 little-endian（`aarch64-`/`arm64-`）与 big-endian（`aarch64_be-`）
都接受。AArch64 指令编码本身固定，发射的指令字节在 LE/BE 下一致（单测
`endian parity` 已验证 LE==BE，56 字节/段）。数据的端序由 IR 的
`DataLayout` 决定。

### 7.2 代码内存模型

- `HostMmapCodeAllocator`：`mmap(RW)` 写入 → `finalize` 时
  `__clear_cache` + `mprotect(RX)` → 析构 `munmap`。POSIX 路径。
- `StaticSlabCodeAllocator`：在调用者提供的缓冲里 bump 分配，flush I-cache
  与“置可执行”通过**可注入钩子**完成，**无 POSIX 依赖**，面向裸核。
- 发射的机器码是**位置无关**的（先发射到探测缓冲再 memcpy 到最终地址），
  因此分配与发射可解耦。

### 7.3 全局/地址模型（Task G）

`tryCompileLight` 从 `PeriodArrayRegistry` 收集：period 数组基址
（`getAllArraysByPeriod`）+ 静态变量（`getStaticVars`）+ 用户注册符号
（`ejit_register_symbol`），构成 `GlobalSymbol{name, absoluteAddress}` 表，
传给发射器。发射器把这些地址作为**立即数（MOVZ/MOVK 链）**烧进代码。

> ⚠️ **跨进程警告**：因为全局地址/快照地址是以**绝对地址**烧进机器码的，
> light 后端产出的函数指针**不能直接跨进程共享**。跨进程缓存应当共享
> **优化后的 bitcode** 或 **代码模板 + 重定位补丁表**，而不是函数指针本身。

---

## 8. 体积影响与验证

### 8.1 体积（`libLLVMEJIT.a`，Release，本机 aarch64 宿主，X86 target）

| 构建 | 大小 | 说明 |
|------|------|------|
| OFF（默认） | 881,340 B | 无任何 light 对象 |
| ON | 1,112,398 B | 含 `EJitLightAArch64/CodeAllocator/Backend` 三个 .o |
| 增量 | **+231,058 B（约 +226 KB）** | 主要是 ~3350 行发射器 |

依赖合理性：ON 构建**未新增** LLVM 链接组件——light 路径复用了
`BitReader`（parse bitcode）与既有的 `EJitOptimizer`（pass pipeline），
发射器本身不依赖任何 `*CodeGen` 目标库。

### 8.2 默认构建不受影响

- OFF 归档中 `ar t` **不含**任何 light 对象（已验证）。
- 所有 light 专属符号/分支均在 `#ifdef EJIT_LIGHT_BACKEND` 内。

### 8.3 测试结果

| 测试 | 结果 |
|------|------|
| `check-ejit-light-backend`（单元级） | **42 checks, 0 failures, PASS**（含 aarch64 上真实执行：poly/addsub/sel/fexpr/dexpr/fmin/栈参/2D 数组/spill；endian parity；triple 闸门；MOVZ/MOVK/MOVN 字节模式；vector 拒绝；分配器行为） |
| `check-ejit-light-runtime`（端到端） | **14 checks, 0 failures, PASS**（走 `ejit_*` C API：注册 bitcode → `LIGHT/AUTO` → `compile_or_get`；绑定外部全局；`compute(5)=105`、改全局后 `compute(9)=10`、缓存命中、AUTO 成功） |

---

## 9. 后端模式语义（ORC / LIGHT / AUTO）

| 模式 | 行为 |
|------|------|
| `ORC`（默认） | 完全走原有 ORC/JITLink 路径，light 代码不参与。 |
| `LIGHT`（强制） | 只走 light；若 IR 不支持或发射失败，**直接失败返回 nullptr，不静默回退 ORC**（错误经 logger 记录：函数名、cacheKey、reason）。 |
| `AUTO` | 先试 light；若 light 不支持/失败，**透明回退 ORC**（回退原因记入日志）。 |

错误信息保真：candidate/发射器的 reason、函数名、cacheKey 均通过
`EJitLogger` 记录，可经 `ejit_get_last_error()` 读取。

---

## 10. 与 EasyJIT(LLVM15) 的差异

| 方面 | EasyJIT light_codegen | EmbeddedJIT Light Backend |
|------|------------------------|---------------------------|
| 纯编码器 | 保留 | 原样保留（verbatim） |
| triple 闸门 | `getTargetTriple()` 返回 `std::string` | LLVM21 返回 `const Triple&`，改用 `.str()` |
| `setTargetTriple` | `StringRef` | LLVM21 需 `llvm::Triple(...)` 包装 |
| SRE 调试 hack | `SRE_MemDbgAlloc`/`SRE_MmuMap`/mmap 调试、weak extern、`LIGHT_SRE_LOG` | **全部删除**，不照搬 |
| `compile()` 驱动 | 与 SRE 强绑定 | 重写为 `compileAArch64Light` + `CodeAllocator` 抽象 |
| 命名空间 | 全局 `light` | `llvm::ejit::light`（`::raw` 为发射器） |
| 接入 | EasyJIT 私有 | 作为 EmbeddedJIT **可选** runtime backend mode |

---

## 11. 与 SPEC4 / PASS7 的关系

light 后端**不替代**特化，只替代“特化之后的代码生成”。`tryCompileLight`
调用的 `EJitOptimizer.runPipeline` 与 ORC IRTransformLayer 用的是**同一套
SPEC4/PASS7 特化 pipeline**（参数替换 → InstCombine → StructFieldPass →
核心优化）。因此 light 与 ORC 看到的是**相同的特化 IR**，差异只在最后的
codegen 后端。

---

## 12. 已知限制与后续工作

- **能力边界**：见 §6。复杂 IR（vector/EH/switch/间接调用/聚合 ABI 等）
  仍需走 ORC（`AUTO` 会自动回退）。
- **代码内存生命周期**：当前 `HostMmapCodeAllocator` 在 driver 析构前一直
  持有 light 代码；cache 淘汰不会立即释放对应可执行内存（原型取舍）。
- **跨进程不可共享函数指针**：见 §7.3。
- **后续**：
  1. 把 light 代码内存纳入 cache 淘汰/释放管理；
  2. 提供“代码模板 + 重定位补丁表”以支持跨进程缓存；
  3. 扩展受支持 IR（如部分 switch lower、更多 intrinsic）；
  4. 在裸核 preset 上接 `StaticSlabCodeAllocator` 跑通端到端。

---

## 13. 快速上手

```c
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"

ejit_config_t cfg = {0};
cfg.optLevel = EJIT_OPT_L2;
ejit_init(&cfg);

if (ejit_light_backend_available())
  ejit_set_backend_mode(EJIT_BACKEND_AUTO);   /* 先试 light，失败回退 ORC */

ejit_register_static_var("g_threshold", &host_threshold);
ejit_register_bitcode("compute", bc_data, bc_size);

void *fn = ejit_compile_or_get("compute", NULL, 0, NULL);
/* aarch64 宿主上可直接调用 fn */
```
