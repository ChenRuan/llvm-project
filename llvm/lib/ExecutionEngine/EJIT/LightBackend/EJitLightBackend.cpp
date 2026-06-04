//===- EJitLightBackend.cpp - light backend public driver -----------------===//
//
// EmbeddedJIT 轻量 AArch64 后端的对外驱动：候选检查 + 编译驱动。
//
// 这里把纯发射器 raw::emit 与 CodeAllocator 抽象粘起来，对外暴露
// compileAArch64Light / isLightBackendCandidate（见 EJitLightBackend.h）。
// 所有错误都通过 Expected/Error + CompileResult 返回，绝不 abort。
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/LightBackend/EJitLightBackend.h"
#include "llvm/ExecutionEngine/EJIT/LightBackend/EJitLightAArch64.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>
#include <vector>

using namespace llvm;

namespace llvm {
namespace ejit {
namespace light {

//===----------------------------------------------------------------------===//
// 候选检查
//===----------------------------------------------------------------------===//

// 把 raw::Status 映射到对外的 light::Status。
static Status mapRawStatus(raw::Status S) {
  switch (S) {
  case raw::Status::Ok:
    return Status::Ok;
  case raw::Status::Unsupported:
    return Status::Unsupported;
  case raw::Status::TooLarge:
    return Status::CodeTooLarge;
  case raw::Status::NotAarch64:
    return Status::NotAArch64;
  }
  return Status::InternalError;
}

static bool typeUsesVector(Type *T) {
  if (!T)
    return false;
  if (T->isVectorTy())
    return true;
  return false;
}

bool isLightBackendCandidate(Function &F, std::string *Reason) {
  auto reject = [&](const char *R) {
    if (Reason)
      *Reason = R;
    return false;
  };

  if (F.isDeclaration() || F.empty())
    return reject("function has no body");

  // 1) Triple 必须是 aarch64*/arm64*（非 ILP32）。这里只做粗筛，发射器内
  //    会再次严格 gate。
  {
    const std::string T = F.getParent()->getTargetTriple().str();
    bool ok = (T.rfind("aarch64", 0) == 0 || T.rfind("arm64", 0) == 0);
    bool ilp32 =
        (T.rfind("aarch64_32", 0) == 0 || T.rfind("arm64_32", 0) == 0);
    if (!ok || ilp32) {
      if (Reason)
        *Reason = "triple is not aarch64*/arm64* (or is ILP32): " + T;
      return false;
    }
  }

  // 2) varargs / 复杂返回。
  if (F.isVarArg())
    return reject("varargs not supported");

  // 3) 参数 / 返回类型粗筛：vector 一律拒绝；aggregate-by-value 拒绝。
  if (typeUsesVector(F.getReturnType()))
    return reject("vector return type not supported");
  if (F.getReturnType()->isAggregateType())
    return reject("aggregate return type not supported");
  for (Argument &A : F.args()) {
    Type *AT = A.getType();
    if (typeUsesVector(AT))
      return reject("vector argument not supported");
    if (AT->isAggregateType())
      return reject("aggregate-by-value argument not supported");
  }

  // 4) 指令级粗筛：vector / 异常 / 间接跳转 / switch / 不支持的调用。
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      // vector 结果或 vector 操作数。
      if (typeUsesVector(I.getType()))
        return reject("vector instruction result not supported");
      for (Value *Op : I.operands())
        if (Op && typeUsesVector(Op->getType()))
          return reject("vector instruction operand not supported");

      switch (I.getOpcode()) {
      case Instruction::Invoke:
      case Instruction::Resume:
      case Instruction::LandingPad:
      case Instruction::CatchSwitch:
      case Instruction::CatchRet:
      case Instruction::CleanupRet:
      case Instruction::CatchPad:
      case Instruction::CleanupPad:
        return reject("exception handling not supported");
      case Instruction::IndirectBr:
        return reject("indirectbr not supported");
      case Instruction::Switch:
        return reject("switch not supported (lower to branches first)");
      case Instruction::VAArg:
        return reject("va_arg not supported");
      default:
        break;
      }

      // 调用：只允许少量已知 intrinsic（memcpy/fmuladd），其余 call 拒绝。
      if (auto *CB = dyn_cast<CallBase>(&I)) {
        Function *Callee = CB->getCalledFunction();
        if (!Callee)
          return reject("indirect call not supported");
        if (Callee->isIntrinsic()) {
          switch (Callee->getIntrinsicID()) {
          case Intrinsic::memcpy:
          case Intrinsic::fmuladd:
          case Intrinsic::dbg_declare:
          case Intrinsic::dbg_value:
          case Intrinsic::lifetime_start:
          case Intrinsic::lifetime_end:
            break;
          default:
            if (Reason)
              *Reason = ("unsupported intrinsic: " + Callee->getName()).str();
            return false;
          }
        } else {
          // 普通函数调用：light 后端当前不发射 BL/重定位，拒绝。
          if (Reason)
            *Reason = ("unsupported call to: " + Callee->getName()).str();
          return false;
        }
      }
    }
  }

  return true;
}

//===----------------------------------------------------------------------===//
// 编译驱动
//===----------------------------------------------------------------------===//

Expected<void *> compileAArch64Light(Function &F,
                                     ArrayRef<GlobalSymbol> Globals,
                                     CodeAllocator &Allocator,
                                     CompileResult *OutResult) {
  auto setResult = [&](Status S, const std::string &R, size_t Bytes) {
    if (OutResult) {
      OutResult->status = S;
      OutResult->reason = R;
      OutResult->codeBytes = Bytes;
    }
  };

  // 1) 候选粗筛（清晰 reason 优先于发射器半路拒绝）。
  std::string CandReason;
  if (!isLightBackendCandidate(F, &CandReason)) {
    setResult(Status::Unsupported, CandReason, 0);
    return createStringError(inconvertibleErrorCode(),
                             "light backend candidate check failed: %s",
                             CandReason.c_str());
  }

  // 2) 把对外的 GlobalSymbol{Name,Address} 转成 raw::GlobalSymbol{name,address}。
  std::vector<raw::GlobalSymbol> RawGlobals;
  RawGlobals.reserve(Globals.size());
  for (const GlobalSymbol &G : Globals)
    RawGlobals.push_back(
        raw::GlobalSymbol{G.Name, reinterpret_cast<const void *>(
                                      static_cast<uintptr_t>(G.Address))});

  // 3) 先用一段栈上探测缓冲跑发射器，拿到精确字节数（也顺带校验 IR 形状）。
  //    light 后端代码极小，16KiB 足够；超出即 CodeTooLarge。
  static constexpr size_t kProbeCap = 64u * 1024u;
  std::vector<uint8_t> Probe(kProbeCap, 0);
  raw::Result PR = raw::emit(F, Probe.data(), Probe.size(),
                             RawGlobals.empty() ? nullptr : RawGlobals.data(),
                             RawGlobals.size());
  if (PR.status != raw::Status::Ok) {
    setResult(mapRawStatus(PR.status), PR.reason, PR.codeBytes);
    return createStringError(inconvertibleErrorCode(),
                             "light emit failed (%s): %s",
                             F.getName().str().c_str(), PR.reason.c_str());
  }

  // 4) 申请真正的可执行内存，复制（重新发射）到该缓冲。
  Expected<MutableArrayRef<uint8_t>> Buf = Allocator.allocate(PR.codeBytes, 64);
  if (!Buf) {
    std::string EM = toString(Buf.takeError());
    setResult(Status::NoMemory, EM, PR.codeBytes);
    return createStringError(inconvertibleErrorCode(),
                             "light backend allocate failed: %s", EM.c_str());
  }

  // 直接把探测缓冲的字节拷过去即可（发射是确定性的、与缓冲地址无关：指令流
  // 内联的是全局/snapshot 的绝对地址，不含相对本函数地址的 PC 相对重定位，
  // 故位置无关，可安全复制）。
  std::memcpy(Buf->data(), Probe.data(), PR.codeBytes);

  if (Error E = Allocator.finalize(*Buf)) {
    std::string EM = toString(std::move(E));
    Allocator.release(*Buf);
    setResult(Status::NoMemory, EM, PR.codeBytes);
    return createStringError(inconvertibleErrorCode(),
                             "light backend finalize failed: %s", EM.c_str());
  }

  setResult(Status::Ok, "", PR.codeBytes);
  return reinterpret_cast<void *>(Buf->data());
}

} // namespace light
} // namespace ejit
} // namespace llvm
