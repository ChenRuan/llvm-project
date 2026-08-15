# EmbeddedJIT 诊断 dump：逐行打印延时

> 适用分支：`ejit-throttle-simplify-djq`
> 开关：`EJIT_DIAG_PRINT_THROTTLE_TICKS`（CMake 缓存变量，默认 20，0 关闭）
> 目标平台：`aarch64_be`（SRE，shell 环形 buffer 串口日志）

---

## 1. 平台依据

SRE 串口日志经 shell 环形 buffer 转发，相关平台参数只有两条：

- **大小**：环形 buffer 512 B（部分配置 128 B）；单行日志约 60 B，16 B 对齐后约 64 B，512 B 约容纳 7 行
- **频率**：消费者每 10 tick（100 ms）轮询排空一次

## 2. 机制

诊断 dump API 的**循环打印**（每个数据项一行）在每打印一行后调用 `ejitDiagPrintThrottle()`（`EJitDiag.h`），即**每循环一轮延时一次**：

- 每次延时 `EJIT_DIAG_PRINT_THROTTLE_TICKS` 个调度 tick（`SRE_TaskDelay`，默认 **20** tick = 200 ms = 2 个消费者排空周期）
- 仅在 `EJIT_DIAG_ENABLE` + `EJIT_FREESTANDING` 下生效；host 构建与诊断关闭时为零开销 no-op
- 行未实际打印（日志级别低于 INFO）时不延时
- 参数经 CMake 缓存变量传递（`llvm/CMakeLists.txt`，非负整数校验，0 关闭），`ejit-minimal-aarch64_be` 预设配 30

## 3. 覆盖的循环

| dump 功能 | 入口 | 循环 |
|---|---|---|
| dump IR / ASM | `printDumped` / `dumpBytesSafe` | 逐行（原每 16 行节流改为逐行） |
| registry | `EJit::printRegistry` | 逐 funcIdx / period 数组 / 静态变量 |
| func meta | `EJit::printFuncMeta` | 逐元数据项 |
| active cells | `EJit::printActive` | 逐 period×cell |
| compiled list | `ejit_taskpool_print_compiled` | 逐缓存条目 |
| icache slots | `ejitDumpIcacheSlots` | 逐 slot |

**范围之外**：正常路径日志（注册、编译、错误回报等）不延时；固定行数的 dump（taskpool/code pool stats）无循环、不延时；正常路径上已有的延时（worker 任务间节流等）保持不变。

## 4. 效果与依据

- 实测：逐行 delay=2 tick（20 ms）时生产者约 5 行/100 ms，卡在消费者排空边界，偶发丢行；delay=100 tick 稳定但过慢。预设 30 tick/行（28 行 dump ≈ 8.4 s）在排空边界上留有裕度且可接受，可按板子 UART 速率调低。
- 每个 dump 循环的逐项行是信息量最大的部分，也是唯一需要节流的部分——头部/汇总行数量固定且少，不参与延时。

## 5. 相关代码

- `llvm/include/llvm/ExecutionEngine/EJIT/EJitDiag.h` — `ejitDiagPrintThrottle()`、`EJIT_DIAG_PRINT_THROTTLE_TICKS` 默认值
- `llvm/CMakeLists.txt` — `EJIT_DIAG_PRINT_THROTTLE_TICKS` 缓存变量与校验
- `llvm/CMakePresets.json` — `ejit-minimal-aarch64_be` 预设（30）
- `llvm/lib/ExecutionEngine/EJIT/` — `EJit.cpp` / `EJitRuntime.cpp` / `EJitOrcEngine.cpp` / `EJitSharedTaskPool.cpp` 中的循环调用点
