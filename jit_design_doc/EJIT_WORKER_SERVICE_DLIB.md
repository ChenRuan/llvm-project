# EmbeddedJIT 独立 dlib 单例 Worker Service 原型设计

> 分支：`codex/ejit-taskpool-worker-dlib`（基线 `35cb60bf1999`）
> 组件：`LLVMEJITWorkerService`（`libLLVMEJITWorkerService.so`） / C ABI 头 `EJitWorkerService.h`
> 默认 **OFF**（`-DEJIT_WORKER_SERVICE=ON` 才构建），不改变默认 `LLVMEJIT` 行为。

本文用一句话回答最初的问题：

> **如果平台保证所有核共享同一个 dlib 的 data/bss 实例，那么依靠 dlib 内部一个“普通单例”，就足以安全地实现“一个 worker、一份 queue、一份 cache、一份 ORC”，无需再维护复杂的跨核 shared-state blob。**
> 这一结论由代码（`EJitWorkerService.cpp`）和 33 个确定性单元测试（`EJitWorkerServiceTest.cpp`）证明；前提“所有核共享同一 dlib 实例”由 `ejit_service_get_identity()` 在真机上**验证**而非假设。

---

## 1. 为什么用 dlib 单例，而不是跨核 shared-state blob

之前的跨核方案（`cb9547f61884`，分支 `ejit_taskpool`）需要：自定义共享 section、owner 选举、跨核注册指纹比对、共享状态结构体的手工内存布局与大小端逐字段解析等。复杂度高、易错、难验证。

本原型改为**把“共享”这件事交给平台的 dlib 加载器**：

| 维度 | 跨核 shared-state blob | dlib 单例（本原型） |
|------|------------------------|----------------------|
| 共享机制 | 自定义共享 section + owner 选举 | 平台保证“一份 dlib data/bss” |
| 单例载体 | 手工布局的 shared 结构体 | 普通 C++ 函数局部 static（Meyers 单例） |
| worker/queue/cache | 放进 shared blob | 放进 dlib 普通成员 |
| 注册一致性 | 跨核指纹比对 | 单例内的注册表 + 全局 `EJitFuncRegistry` |
| 是否成立的验证 | 难 | `ejit_service_get_identity()` 运行期比对地址/代数 |
| 失败回退 | 复杂 | 地址不一致即判定 model B，干净回退 |

**核心洞察**：`EJitWorkerService` 单例、`EJitFuncRegistry`/`EJitLifecycleRegistry` 单例、内部 `EJit`（含 ORC/taskpool/worker/cache）全部位于 dlib 的 data/bss。只要平台把这份 data/bss 在所有核上映射为**同一实例**（model A），它们天然就是“同一个”——不需要任何自定义共享状态。

---

## 2. 平台模型 A / B 与“可验证而非假设”

* **模型 A（目标）**：所有业务核共享同一份 dlib data/bss。则单例 = 唯一共享服务，`workerStartCount==1`，所有核 `instanceAddress`/`queueAddress`/`cacheAddress`/`generation` 相同。
* **模型 B**：每核各有一份 dlib data/bss 拷贝。则每核看到独立单例，地址不同；本机制**无法**实现“单一 worker”，必须回退。

服务**不自动假设** model A。提供 `ejit_service_get_identity()`，由集成方在各核调用后比对：

```
Core0.get_identity()  ┐
                      ├─ instanceAddress 相同 && generation 相同 && workerStartCount==1  ⇒ model A 成立
Core1.get_identity()  ┘  否则 ⇒ model B，单 worker 方案不适用，按 §10 回退
```

---

## 3. 组件边界

```
 业务核 A/B/C ──(仅) C ABI ejit_service_*──▶  libLLVMEJITWorkerService.so
                                              ├─ WorkerService 单例（状态机/注册表/诊断/identity）
                                              └─ EJit（复用）
                                                  ├─ EJitCompileDriver（compileNow / 一条 compileCold）
                                                  ├─ EJitTaskPool（一份 queue/cache/dedup + 单 worker）
                                                  ├─ EJitOrcEngine（一份 ORC/JITLink）
                                                  ├─ EJitModuleLoader / EJitRuntimeState
                                                  └─ code pool（4K 封装 RW→RX，未改动）
                                              全局单例：EJitFuncRegistry / EJitLifecycleRegistry
```

* 业务模块**只**接触 `EJitWorkerService.h` 里的 C ABI 与 POD，从不接触任何 C++ 对象。
* 服务**复用**既有编译链（`EJit`/`EJitCompileDriver`/`compileNow`/`EJitTaskPool`/`EJitWorker`/`EJitSreTask`/`EJitCache`/`EJitOrcEngine`/code pool），**不复制**编译流水线、**不**引入第二个 optimizer、**不**用 MCJIT。

---

## 4. C ABI（dlib 唯一导出面）

全部见 `EJitWorkerService.h`。要点：

* 仅定宽整型 / 指针 / POD；每个 struct 带 `abiVersion + structSize`，字段**只增不改**；逐字段标量访问，大小端安全（无字节解析）。
* 显式状态码 `ejit_service_status_t`，无异常、无 RTTI、无 STL 跨界。
* 平台符号仅声明（`SRE_printf` 仅在 `-DEJIT_SERVICE_DIAG_ENABLE` 时），无 weak 兜底。

入口（节选）：`ejit_service_init/shutdown/get_state/get_diagnostics/get_identity/register_module/compile_or_get/release_read/worker_poll_one/worker_poll_budget/activate/deactivate/free_code/get_abi_version`。

---

## 5. 服务生命周期状态机（显式枚举，非 bool）

```
 UNINITIALIZED ──register_module(暂存)──▶ UNINITIALIZED
       │ init()
       ▼
 INITIALIZING ──build backend 失败──▶ FAILED ──shutdown──▶ STOPPED
       │ backend ok + worker commissioned        ▲
       ▼                                          │
     READY ──register_module──▶ FROZEN(拒绝)      │
       │ init()(幂等,返回 OK,不重建)               │
       │ shutdown                                  │
       ▼                                          │
   STOPPING ──join worker, 销毁 ORC/cache──▶ STOPPED ──init──▶ INITIALIZING(代数+1)
```

* 首次 `init` 创建单例后端；重复 `init` 幂等返回 OK，不重建、不重启 worker。
* `INITIALIZING` 期间用原子 CAS 占位，其它核此时 `init` 得到 `ERR_NOT_READY`，不会读到半初始化对象。
* worker 仅在“全部注册基础设施 + ORC + taskpool 就绪”后才被 commission；worker 启动失败 ⇒ `FAILED` + `ERR_INIT_FAILED`（绝不伪造 AOT/JIT 成功）。
* `shutdown` 先停/join worker，再销毁 ORC/注册；之后没有 worker 回调能触达已销毁的服务。
* v1 为进程级单例（`generation` 单调递增，`shutdown` 不回退 `generation`，体现“同一物理单例、新一代”）。

---

## 6. 模块注册协议

POD：`ejit_service_reg_kind_t` / `ejit_service_reg_entry_t` / `ejit_service_module_reg_t`（见头文件）。`entries` 为**可写**数组——服务把分配到的 `funcIndex`/`dimType` 回填进 `entry.value0`，wrapper 读回该全局槽，而非自行重算。

* **幂等**：同名同 payload 再注册 ⇒ OK（不重复转发）。
* **冲突拒绝**：同名不同 payload ⇒ `ERR_CONFLICT`，且**不改动**既有状态（先做无副作用校验 pass1，再提交 pass2）。
* **冻结**：`READY` 后再注册 ⇒ `ERR_FROZEN`（worker 无锁读注册表，注册期必须先于 worker）。
* `BITCODE/PERIOD_ARRAY/STATIC_VAR/SYMBOL` 转发到既有 `EJitRegistrationStore`，由 `EJit` 构造时消费；`FUNC_INDEX/LIFECYCLE` 走全局 `EJitFuncRegistry/EJitLifecycleRegistry`。
* 失败可由 `register_module`/`init` 的返回值上报，不藏在 void ABI 里。

---

## 7. funcIndex / dimType 中央分配

* 由 dlib 内的全局 `EJitFuncRegistry`（稠密 funcIndex，上限 `kEJitMaxFuncIndex=4096`）与 `EJitLifecycleRegistry`（dimType，上限 `kEJitMaxDimTypes=8`）统一分配，按**名字**幂等。
* **跨模块同名函数 ⇒ 同一 funcIndex**；不同函数 ⇒ 不同 funcIndex；同一 lifecycle ⇒ 同一 dimType；不同 lifecycle 永不复用 dimType。
* 无“取模哈希当索引”、无函数别名共用 funcIndex；容量耗尽 ⇒ 干净 `ERR_CAPACITY`；注册顺序不影响已分配索引（乱序、跨模块测试覆盖）。
* wrapper 通过回填的 `value0` 或后续查询获得全局槽，不依赖每核私有 registry 顺序。

---

## 8. 同步 / 异步链路（共享一份 cache / 一条 compileCold）

服务把**所有**编译走同一 `EJitTaskPool`（一份 queue/cache/dedup），三种模式由 `ejit_service_mode_t` 选择：

* `SYNC`：无后台 worker。`compile_or_get` 未命中时在调用核上 `pollOne` 内联编译，再做**仅查 cache** 的回查；成功返回代码，失败返回 `ERR_COMPILE_FAILED`（不再入队自旋）。复用同一 cache、同一 `compileNow/compileCold`。
* `ASYNC`：单后台 worker（host 适配用 `std::thread`，freestanding 用 `EJitSreTask`）。未命中入队返回 `PENDING` + fallback，worker 编译并发布。
* `ASYNC_MANUAL`：异步语义但无后台线程，由调用方/平台 tick 调 `worker_poll_*` 抽干——确定性测试与“平台自驱泵”用。

共性：先查 cache 再入队；队满干净回退并回滚 dedup；同 key 并发只编译一次；worker 内**不**用 `std::thread/mutex/condition_variable`（freestanding 核）。

---

## 9. worker 任务

* 复用 `EJitSreTask`（host=`std::thread`；sre=`SRE_TaskCreate/Delete/Delay` 官方 ABI，隔离适配）。
* 统一栈大小宏 `EJIT_SRE_TASKPOOL_WORKER_STACK_SIZE`（默认 1 MiB，平台可调，诊断回报最终值）。
* TaskCreate 失败向上传播为 `init` 失败；`SRE_TaskDelete` 即 join 契约；空闲 `yield`，不忙等；不访问半初始化对象；不起第二个 worker；重复 `init` 不重复建任务。

---

## 10. 跨核执行已编译指针的能力门（默认 OFF）

`ejit_service_config_t.shareCodePointers`（能力位，默认 0）。即便 dlib 单例意味着同地址空间，仍要求集成方**显式确认**而非自动猜：

跨核执行 `fnPtr` 的前提（全部满足才置 1）：同一地址空间；code pool 在各核同一 VA；重定位完成；RW→RX 完成；I/D cache 同步；code pool 生命周期覆盖所有调用方；全局变量地址在各核语义正确。

* OFF：非属主核不盲目执行共享指针，干净回退，不重复入队已完成结果。
* ON：服务返回共享 `fnPtr`，文档化 PC 异常风险。

---

## 11. identity / diagnostics

* `ejit_service_get_identity()`：`instanceAddress/queueAddress/cacheAddress/generation/workerStartCount/workerTaskId`，供多核验证 model A（§2）。
* `ejit_service_get_diagnostics()`：状态/模式/代数/init 次数/worker 启动次数/worker task id/worker 栈大小/共享指针能力/注册条目数/模块数/funcIndex 数/dimType 数/队列深度/pending 数/cache ready 数/cacheHits/enqueue/compile/publishFail/queueFull。
* 诊断默认零成本（`do{}while(0)` 宏，参数不求值，`-DEJIT_SERVICE_DIAG_ENABLE` 才接 `SRE_printf`）。

---

## 12. 构建与打包

* CMake 选项 `EJIT_WORKER_SERVICE`（默认 OFF），要求 `EJIT_SRE_TASKPOOL=ON`。
* `add_library(LLVMEJITWorkerService SHARED EJitWorkerService.cpp)`，`-fvisibility=hidden` 隐藏 C++ 内部；版本脚本 `EJitWorkerService.exports` 只导出 `ejit_service_*`，`--exclude-libs,ALL` 不泄漏任何 LLVM 内部符号。
* 链接既有 `LLVMEJIT`（其 `LINK_COMPONENTS` 传递带入相同 LLVM 组件，不额外捆绑后端）。
* 主机静态单测变体 `EJITWorkerServiceTests` 直接编译服务源 + taskpool 源 + `EJIT_WORKER_SERVICE_TESTING`（mock 编译器，无 ORC/无真实线程），仅链 `Support`。
* 不改默认 `LLVMEJIT`；OFF 时不向 `LLVMEJIT` 添加任何服务对象。
* freestanding：无 `dlopen/pthread/libstdc++ thread`；核目标与平台打包步骤分离。

---

## 13. 测试（`EJITWorkerServiceTests`，确定性、不依赖真实线程）

33 个用例覆盖：ABI 版本/struct 大小校验、重复 init 一个服务、重复 init 一个 worker、init 失败传播到 FAILED、幂等再注册、同名异 payload 拒绝、跨模块同函数同 funcIndex、不同函数不同 funcIndex、lifecycle 一致映射、容量耗尽拒绝、冻结后拒绝注册、SYNC 未命中→编译→命中、SYNC 编译失败传播、ASYNC 入队 pending、ASYNC worker 编译后取回、同 key 并发只编译一次、队满回退+dedup 回滚、activate/deactivate 门控、未知 lifecycle 拒绝、free_code 尽力而为、identity 多次同址、双业务模块同服务、诊断字段大小端逐字段、默认 OFF 共享指针、shutdown 先 join 后销毁、shutdown 后重 init 新代数、非法参数兜底等。其中仅“重复 init 一个 worker / shutdown join”用真实 host worker（host 适配，既有 `EJITTaskPoolTests` 同样实践），其余全部 SYNC/MANUAL 无线程。

---

## 14. 真机验证清单（尚未在目标机验证，诚实标注）

1. 在 Core0/Core1 各调 `ejit_service_get_identity()`，确认 `instanceAddress` 相同 ⇒ model A 成立（否则停止用本方案）。
2. 确认 `workerStartCount==1`、`workerTaskId` 为同一平台任务。
3. 置 `shareCodePointers=1` 前，逐项确认 §10 七个前提（同 VA、relocation、RW→RX、I/D cache、生命周期…）。
4. freestanding 构建确认 worker 用 `SRE_TaskCreate/Delete/Delay`，栈 = 配置值。
5. 大小端：在 aarch64_be 上确认所有诊断/identity 字段逐标量读取正确。

> 以上 1–5 在本机（x86-64 host）无法证明，**未做真机验证**；本原型只证明“逻辑可行 + 主机可编译可测 + 可打包为 dlib”。

---

## 15. 优缺点与建议

**优点**：复用既有编译链，零新增编译器/优化器；无自定义共享 section/owner 选举/指纹；单例与注册天然随 dlib 共享；model A 可运行期验证；默认 OFF 不影响主线；C ABI 干净、POD、大小端安全。

**缺点 / 已知限制**：成立性完全依赖平台“一份 dlib data/bss”（model B 不适用）；`free_code` 受 taskpool cache 无 per-key 驱逐限制，仅尽力而为；`shareCodePointers` 的真机安全前提需人工确认；进程级单例（v1 不支持运行期销毁后异核继续）；真机/aarch64_be 行为未验证。

**建议**：若平台能确认 model A，**推荐**以本 dlib 单例方案替代跨核 shared-state blob——更简单、可验证、可维护。若平台是 model B（每核独立 dlib 拷贝），本方案不适用，应回到“每核独立 JIT”或专门的跨核共享内存方案。
