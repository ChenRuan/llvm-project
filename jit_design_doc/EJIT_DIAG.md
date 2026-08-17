# EmbeddedJIT 诊断 dump：逐行打印延时

> 适用分支：`ejit-print-compiled-fmt-djq`
> 开关：`EJIT_DIAG_PRINT_THROTTLE_TICKS`（CMake 缓存变量，默认 20，0 关闭）
> 目标平台：`aarch64_be`（SRE，shell 环形 buffer 串口日志）

---

## 1. 平台依据

SRE 串口日志经 shell 环形 buffer 转发，相关平台参数只有两条：

- **大小**：环形 buffer 512 B（部分配置 128 B）；单行日志约 60 B，16 B 对齐后约 64 B，512 B 约容纳 7 行
- **频率**：消费者每 10 tick 轮询排空一次（1 tick = 1 ms，即 10 ms 一轮）

## 2. 机制

诊断 dump API 的**循环打印**（每个数据项一行）在每打印一行后调用 `ejitDiagPrintThrottle()`（`EJitDiag.h`），即**每循环一轮延时一次**：

- 每次延时 `EJIT_DIAG_PRINT_THROTTLE_TICKS` 个调度 tick（`SRE_TaskDelay`，默认 **20** tick = 20 ms = 2 个消费者排空周期：第一个周期完成排空，第二个周期是吸收消费者抖动与多核日志交错的裕量）
- 仅在 `EJIT_DIAG_ENABLE` + `EJIT_FREESTANDING` 下生效；host 构建与诊断关闭时为零开销 no-op
- 行未实际打印（日志级别低于 INFO）时不延时
- 参数经 CMake 缓存变量传递（`llvm/CMakeLists.txt`，非负整数校验，0 关闭），`ejit-minimal-aarch64_be` 预设配 20

## 3. 覆盖的循环

| dump 功能 | 入口 | 循环 |
|---|---|---|
| dump IR / ASM | `printDumped` / `dumpBytesSafe` | 逐行（原每 16 行节流改为逐行） |
| registry | `EJit::printRegistry` | 逐 funcIdx / period 数组 / 静态变量 |
| func meta | `EJit::printFuncMeta` | 逐元数据项 |
| active cells | `EJit::printActive` | 逐 period×cell |
| compiled list | `ejit_taskpool_print_compiled` | 逐缓存条目 |
| icache slots | `ejitDumpIcacheSlots` | 逐 slot |

**全部 dump 输出行（头/尾固定行与循环条目行）统一用 `EJIT_DIAG_RAW` 打印**（无 `[EJIT] func:line` 前缀）：同一前缀在一块 dump 里逐行重复，只占 ring buffer 空间；grep 锚点是各块自带的文字标签（`registry:`、`active periods:`、`code pool:`、`stats_t:`、`=== ... ===`、`compiled:` 等）。正常路径（非 dump）日志仍用 `EJIT_DIAG`，保留 func:line 来源；hosted 构建（std::printf 分支）同样无前缀。信息量增强：active cells 每 period 先打 `active=N` 计数行（0 个时打 `active=0`，替代原 "(no active cells)"；计数与条目行两遍扫描之间状态可能翻转，同 compiled list 一样非快照）；func meta 条目行为 `op=<原始操作数下标> tag=<名> vals=<值>`（op= 与 bitcode 中 `!ejit.metadata` 的操作数位置一一对应，被跳过的条目也占号）；IR/ASM 长行按 180 B 分块，非末块尾缀 `...`；icache slots 带函数名（模块 loader 按 funcIndex 解析，查不到为 `<unknown>`、未传 loader 为 `?`，超 24 字符截断）与槽容量 `cells=16^numDims`；taskpool stats 补 shared 诊断 9 字段（initState/ownerCore/gen/lastInitErr/initAttempts/share + workerTaskId/regFingerprint/execPrepFailed，直读 getDiagnostics()，不动 C ABI 结构体）；code pool stats 末行补 `total/used/usage` 汇总（total=reservedBytes，打印时整数千分比派生，无 FPU）。

**compiled list 汇总行先打印**（两遍遍历：第一遍只统计，第二遍才打印条目），报总数、槽位占用、统计遍历中被锁竞争跳过的桶数与按维度数分布（`byDims: 0d=.. 1d=..`，只列非零档）；若打印遍历本身又跳过桶，末尾补一行说明（固定行数，不延时），`forEachCompiled` 返回的统计使跳过的桶不再静默。dims 只打实际个数（无 `0:0` 填充）；VERBOSE 级同行追加 `ver/size/pool/gen`（版本快照、代码大小、池号、owner 代）。两遍遍历之间的并发 publish 可能使汇总与条目行略有出入（遍历本就不是快照）。

**范围之外**：正常路径日志（注册、编译、错误回报等）不延时；固定行数的 dump（taskpool/code pool stats）无循环、不延时；正常路径上已有的延时（worker 任务间节流等）保持不变。

## 4. 效果与依据

- 实测：逐行 delay=2 tick（2 ms）时生产速率（1 行/2 ms）远超消费者排空容量（约 7 行/10 ms），丢行；delay=100 tick 稳定但过慢。选值 20 tick/行（20 ms = 2 个排空周期，28 行 dump ≈ 560 ms）：每行留 1 个完整排空周期裕量，多核日志交错场景下 ring（约 7 行）仍有 5 行以上余量；15 tick（1.5 个排空周期）裕量减半被否，50 tick（5 个排空周期，≈1.4 s）过于保守。长行（compiled list VERBOSE 条目）也被 2 个排空周期覆盖，可按板子 UART 速率再调。
- 每个 dump 循环的逐项行是信息量最大的部分，也是唯一需要节流的部分——头部/汇总行数量固定且少，不参与延时。

## 5. 相关代码

- `llvm/include/llvm/ExecutionEngine/EJIT/EJitDiag.h` — `ejitDiagPrintThrottle()`、`EJIT_DIAG_PRINT_THROTTLE_TICKS` 默认值
- `llvm/CMakeLists.txt` — `EJIT_DIAG_PRINT_THROTTLE_TICKS` 缓存变量与校验
- `llvm/CMakePresets.json` — `ejit-minimal-aarch64_be` 预设（20）
- `llvm/lib/ExecutionEngine/EJIT/` — `EJit.cpp` / `EJitRuntime.cpp` / `EJitOrcEngine.cpp` / `EJitSharedTaskPool.cpp` 中的循环调用点
