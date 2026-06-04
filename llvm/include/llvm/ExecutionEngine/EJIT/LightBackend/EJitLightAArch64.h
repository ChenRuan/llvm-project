//===- EJitLightAArch64.h - EmbeddedJIT light AArch64 raw emitter -*- C++ -*-=//
//
// EmbeddedJIT 实验性轻量 AArch64 后端 —— 原始指令发射器接口。
//
// 本文件声明的是 *内部* 的窄发射器 `raw::emit`，它把一个经过
// SPEC4/PASS7 特化后的 LLVM Function 直接翻译成 AArch64 机器码字节，
// 写入调用方提供的缓冲区。它不分配可执行内存、不做权限切换，也不
// 依赖任何 ORC / JITLink / EPC / SRE 设施。可执行内存由
// `EJitLightCodeAllocator.h` 中的 CodeAllocator 抽象负责。
//
// 该发射器从 EasyJIT (LLVM15) 的 light_codegen/light_aarch64.cpp 迁移
// 而来，迁移到 LLVM21 时仅调整了少量 API（主要是 Module::getTargetTriple
// 现在返回 const Triple&）。EasyJIT 中的 SRE 调试 hack（SRE_MmuMap /
// SRE_MemDbgAlloc / mmap 调试路径）**没有**被搬运。
//
// 端序模型：指令流永远按 little-endian 字节序写入（ARM ARM B2.6.2），
// 与目标数据端序无关；aarch64_be 只影响 LDR/STR 的数据语义，不影响
// 指令字节。详见 jit_design_doc/EJIT_LIGHT_BACKEND.md。
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_LIGHTBACKEND_EJITLIGHTAARCH64_H
#define LLVM_EXECUTIONENGINE_EJIT_LIGHTBACKEND_EJITLIGHTAARCH64_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace llvm {
class Function;
}

namespace llvm {
namespace ejit {
namespace light {
namespace raw {

// 原始发射器返回码。`NotAarch64` 覆盖任何发射器拒绝的 triple
// （非 aarch64* 或 ILP32 的 aarch64_32/arm64_32）。aarch64-* 与
// aarch64_be-* 都被接受，因此该状态不是“端序拒绝”。`NotAarch64LE`
// 作为源码级兼容别名保留。
enum class Status {
  Ok,
  Unsupported,
  TooLarge,
  NotAarch64,
  NotAarch64LE = NotAarch64,
};

struct Result {
  Status status = Status::Ok;
  std::size_t codeBytes = 0;
  std::string reason;
};

// 原始发射器使用的全局符号表项。`address` 是 host 进程内已经解析好的
// 绝对地址（用于把外部 GlobalVariable / inttoptr 常量地址内联进机器码）。
struct GlobalSymbol {
  const char *name;
  const void *address;
};

// 把 Fn 翻译为 AArch64 机器码写入 buf[0, bufCap)。成功时
// Result::status == Ok 且 Result::codeBytes 为写入字节数。任何不支持的
// IR 形状返回 Status::Unsupported 并在 reason 中说明；非 aarch64 triple
// 返回 Status::NotAarch64；缓冲区不足返回 Status::TooLarge。
//
// 该函数不分配内存、不切换权限、不刷新 icache —— 这些由上层
// CodeAllocator 负责。
Result emit(const llvm::Function &Fn, std::uint8_t *buf, std::size_t bufCap,
            const GlobalSymbol *globals = nullptr, std::size_t nglobals = 0);

} // namespace raw
} // namespace light
} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_LIGHTBACKEND_EJITLIGHTAARCH64_H
