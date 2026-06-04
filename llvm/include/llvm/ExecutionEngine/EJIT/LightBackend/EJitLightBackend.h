//===- EJitLightBackend.h - EmbeddedJIT light backend public API -*- C++ -*-=//
//
// EmbeddedJIT 实验性轻量 AArch64 执行后端的对外 API。
//
// 这是 EmbeddedJIT 在 ORC/JITLink 之外新增的 *可选* 执行后端。它把
// 经过 SPEC4/PASS7 特化优化后的 LLVM IR 直接发射为 AArch64 机器码并返回
// 函数指针，绕过 ORC / JITLink / EPC / hosted mmap / dlopen / pthread 等
// 复杂依赖，用于裸核 / SRE / 极小包场景。
//
// 它的能力边界比 LLVM CodeGen 窄很多（见 isLightBackendCandidate 与
// jit_design_doc/EJIT_LIGHT_BACKEND.md 的支持/不支持列表）。默认执行路径
// 仍然是 ORC；本后端只能通过 CMake option(EJIT_ENABLE_LIGHT_BACKEND) 编译，
// 并通过 runtime backend mode 显式启用。
//
// 重要：light 后端生成的机器码可能内联了绝对地址（snapshot / 全局符号
// 地址），因此**不能直接跨进程共享函数指针**。详见设计文档。
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_LIGHTBACKEND_EJITLIGHTBACKEND_H
#define LLVM_EXECUTIONENGINE_EJIT_LIGHTBACKEND_EJITLIGHTBACKEND_H

#include "llvm/ExecutionEngine/EJIT/LightBackend/EJitLightCodeAllocator.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace llvm {
class Function;
}

namespace llvm {
namespace ejit {
namespace light {

// 对外的全局符号表项：名字 + 已解析的 host 绝对地址。
struct GlobalSymbol {
  const char *Name = nullptr;
  uint64_t Address = 0;
};

// light 后端编译状态。比 raw::Status 多区分了内存 / 容量 / 内部错误，
// 方便上层 runtime 决定 AUTO 模式下是否回退 ORC。
enum class Status {
  Ok,
  Unsupported,   // IR 形状超出 light 后端能力（可在 AUTO 下回退 ORC）
  NotAArch64,    // 目标 triple 非 aarch64*/arm64*（可回退 ORC）
  NoMemory,      // CodeAllocator 无法分配
  CodeTooLarge,  // 代码超出缓冲容量
  InternalError, // 不应发生的内部错误
};

struct CompileResult {
  Status status = Status::InternalError;
  std::string reason;
  size_t codeBytes = 0;
};

// 把 F（应当已经过 SPEC4/PASS7 特化）编译为 AArch64 机器码，使用
// Allocator 申请并定稿可执行内存，返回可调用的函数指针。
//
// 成功：返回函数指针，且若 OutResult 非空则填入 status=Ok / codeBytes。
// 失败：返回 Error（同时若 OutResult 非空则填入对应 status + reason）。
//
// 该函数不 abort、不 longjmp；所有错误都通过 Expected/Error + CompileResult
// 返回。
Expected<void *> compileAArch64Light(llvm::Function &F,
                                     ArrayRef<GlobalSymbol> Globals,
                                     CodeAllocator &Allocator,
                                     CompileResult *OutResult);

// 快速候选检查：在真正调用发射器之前，粗筛掉明显不支持的 IR 形状
// （vector、varargs、aggregate-by-value ABI、invoke/landingpad、indirectbr、
// switch、不支持的调用/intrinsic 等），给出清晰 reason。
//
// 返回 true 表示“看起来可以交给 light 后端试一试”；返回 false 时 Reason
// （若非空）写入拒绝原因。
//
// 注意：candidate 检查 **不能替代** 发射器内部的严格校验——发射器仍可能
// 在更细的粒度上拒绝。两层校验都需要。
bool isLightBackendCandidate(llvm::Function &F, std::string *Reason);

} // namespace light
} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_LIGHTBACKEND_EJITLIGHTBACKEND_H
