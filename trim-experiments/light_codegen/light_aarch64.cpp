// Experimental narrow AArch64 emitter.
//
// We walk LLVM IR instructions by opcode and, for each supported shape, write
// one or more 32-bit ARMv8 little-endian instructions. No register allocator:
// for the tiny subset we need, each IR SSA value lives in a statically mapped
// x-register slot (index = param position or monotonically increasing spill
// counter), and we actively reject shapes we can't trivially lower.
//
// This is intentionally dumb. It is meant to answer the question "can you
// skip SelectionDAG + LLVMCodeGen + AArch64CodeGen entirely for the IR shape
// EasyJIT hands to ORC?", not to be a competitive backend.

#include "light_aarch64.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Module.h"

#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <unordered_map>

using namespace light;

namespace {

// ---------- AArch64 encoders (just what this PoC needs) ----------

// ADD/SUB (immediate, 32/64-bit), imm in [0, 4095]. shift=0.
static uint32_t encAddSubImm(bool sub, bool is64, unsigned rd, unsigned rn,
                             unsigned imm12) {
  uint32_t op = sub ? 0x51000000u : 0x11000000u;
  if (is64) op |= 0x80000000u;
  return op | ((imm12 & 0xFFFu) << 10) | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}

// ADD/SUB (shifted reg), shift=0, amount=0 — i.e. plain reg-reg form.
static uint32_t encAddSubReg(bool sub, bool is64, unsigned rd, unsigned rn,
                             unsigned rm) {
  uint32_t op = sub ? 0x4B000000u : 0x0B000000u;
  if (is64) op |= 0x80000000u;
  return op | ((rm & 0x1Fu) << 16) | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}

// AND/ORR/EOR (shifted reg). kind: 0=AND, 1=ORR, 2=EOR.
static uint32_t encLogicReg(unsigned kind, bool is64, unsigned rd, unsigned rn,
                            unsigned rm) {
  static const uint32_t base[3] = {0x0A000000u, 0x2A000000u, 0x4A000000u};
  uint32_t op = base[kind];
  if (is64) op |= 0x80000000u;
  return op | ((rm & 0x1Fu) << 16) | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}

// MUL = MADD with Ra=xzr(31).
static uint32_t encMul(bool is64, unsigned rd, unsigned rn, unsigned rm) {
  uint32_t op = 0x1B000000u | (31u << 10); // Ra=31
  if (is64) op |= 0x80000000u;
  return op | ((rm & 0x1Fu) << 16) | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}

// MOV (wide immediate, imm16<<shift16) — simplified to 16-bit unsigned imm.
static uint32_t encMovz16(bool is64, unsigned rd, uint16_t imm16) {
  uint32_t op = 0x52800000u; // MOVZ, hw=0
  if (is64) op |= 0x80000000u;
  return op | ((uint32_t)imm16 << 5) | (rd & 0x1Fu);
}

// MOV (register) = ORR Xd, XZR, Xm — unified form.
static uint32_t encMovReg(bool is64, unsigned rd, unsigned rm) {
  return encLogicReg(1, is64, rd, 31u, rm);
}

static constexpr uint32_t kRet = 0xD65F03C0u; // ret (lr)

// LSL imm (ubfm), LSR imm (ubfm), ASR imm (sbfm).
// For the narrow subset we only emit shift-by-constant on w/x.
static uint32_t encLslImm(bool is64, unsigned rd, unsigned rn, unsigned sh) {
  unsigned regBits = is64 ? 64u : 32u;
  unsigned immr = (regBits - sh) & (regBits - 1);
  unsigned imms = (regBits - 1) - sh;
  uint32_t op = 0x53000000u; // UBFM (32)
  if (is64) op |= 0x80400000u; // UBFM (64) with N=1
  return op | ((immr & 0x3Fu) << 16) | ((imms & 0x3Fu) << 10)
           | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
static uint32_t encLsrImm(bool is64, unsigned rd, unsigned rn, unsigned sh) {
  unsigned regBits = is64 ? 64u : 32u;
  unsigned immr = sh & (regBits - 1);
  unsigned imms = regBits - 1;
  uint32_t op = 0x53000000u;
  if (is64) op |= 0x80400000u;
  return op | ((immr & 0x3Fu) << 16) | ((imms & 0x3Fu) << 10)
           | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}
static uint32_t encAsrImm(bool is64, unsigned rd, unsigned rn, unsigned sh) {
  unsigned regBits = is64 ? 64u : 32u;
  unsigned immr = sh & (regBits - 1);
  unsigned imms = regBits - 1;
  uint32_t op = 0x13000000u; // SBFM (32)
  if (is64) op |= 0x80400000u;
  return op | ((immr & 0x3Fu) << 16) | ((imms & 0x3Fu) << 10)
           | ((rn & 0x1Fu) << 5) | (rd & 0x1Fu);
}

// ---------- tiny writer helper ----------
struct Writer {
  uint8_t *buf; size_t cap; size_t pos = 0;
  bool emit(uint32_t w) {
    if (pos + 4 > cap) return false;
    std::memcpy(buf + pos, &w, 4);
    pos += 4;
    return true;
  }
};

// ---------- value → register map ----------
//
// Strategy: every SSA value that is actually needed lives in a dedicated
// x-register. We assign registers in the order they are produced:
//   - param i -> x_i  (i in [0,7])
//   - first instruction result -> x9
//   - second -> x10, ... up to x15
// We never spill; if we run out, we bail out with Unsupported. That is fine
// for the target IR shape (1 BB, <=6 instructions).

struct RegMap {
  std::unordered_map<const llvm::Value *, unsigned> reg;
  unsigned nextScratch = 9;
  unsigned assignScratch(const llvm::Value *v) {
    if (nextScratch > 15) return ~0u;
    unsigned r = nextScratch++;
    reg[v] = r;
    return r;
  }
  bool getReg(const llvm::Value *v, unsigned &out) const {
    auto it = reg.find(v);
    if (it == reg.end()) return false;
    out = it->second;
    return true;
  }
};

// Return the 12-bit unsigned imm if V is a ConstantInt in [0, 4095], else -1.
static int asImm12(const llvm::Value *V) {
  auto *CI = llvm::dyn_cast<llvm::ConstantInt>(V);
  if (!CI) return -1;
  const auto &AP = CI->getValue();
  if (AP.isNegative() || AP.getActiveBits() > 12) return -1;
  return (int)CI->getZExtValue();
}

} // namespace

// ---------- main entry ----------

Result light::emit(const llvm::Function &Fn, uint8_t *buf, size_t cap) {
  Result r;
  Writer W{buf, cap};

  if (Fn.getParent()->getTargetTriple().find("aarch64") != 0) {
    r.status = Status::NotAarch64LE;
    r.reason = "triple is not aarch64*";
    return r;
  }
  // Reject aarch64_be explicitly — see report.
  if (Fn.getParent()->getTargetTriple().find("aarch64_be") == 0) {
    r.status = Status::NotAarch64LE;
    r.reason = "aarch64_be not supported by PoC emitter";
    return r;
  }
  if (Fn.isDeclaration() || Fn.empty()) {
    r.status = Status::Unsupported; r.reason = "no body"; return r;
  }
  if (Fn.size() != 1) {
    r.status = Status::Unsupported; r.reason = "multi-BB not supported"; return r;
  }
  if (Fn.arg_size() > 8) {
    r.status = Status::Unsupported; r.reason = "> 8 args"; return r;
  }

  RegMap RM;
  unsigned argIdx = 0;
  for (const auto &A : Fn.args()) {
    if (!A.getType()->isIntegerTy()) {
      r.status = Status::Unsupported; r.reason = "non-integer arg"; return r;
    }
    RM.reg[&A] = argIdx++;
  }

  const llvm::BasicBlock &BB = Fn.getEntryBlock();
  for (const llvm::Instruction &I : BB) {
    auto unsupported = [&](const char *why) {
      r.status = Status::Unsupported;
      r.reason = std::string("unsupported: ") + why + " (" + I.getOpcodeName() + ")";
    };

    if (auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(&I)) {
      if (!BO->getType()->isIntegerTy()) { unsupported("non-integer binop"); return r; }
      unsigned bits = BO->getType()->getIntegerBitWidth();
      if (bits != 32 && bits != 64) { unsupported("binop width"); return r; }
      bool is64 = (bits == 64);

      unsigned rd = RM.assignScratch(&I);
      if (rd == ~0u) { unsupported("scratch exhausted"); return r; }
      unsigned rn;
      if (!RM.getReg(BO->getOperand(0), rn)) { unsupported("op0 not live"); return r; }

      // reg-imm12 shapes: add/sub
      int imm = asImm12(BO->getOperand(1));
      switch (BO->getOpcode()) {
      case llvm::Instruction::Add:
        if (imm >= 0) { if (!W.emit(encAddSubImm(false, is64, rd, rn, imm))) { r.status=Status::TooLarge; return r; } break; }
        goto reg_reg;
      case llvm::Instruction::Sub:
        if (imm >= 0) { if (!W.emit(encAddSubImm(true,  is64, rd, rn, imm))) { r.status=Status::TooLarge; return r; } break; }
        goto reg_reg;
      case llvm::Instruction::Mul:
      case llvm::Instruction::And:
      case llvm::Instruction::Or:
      case llvm::Instruction::Xor: {
      reg_reg:
        unsigned rm;
        if (!RM.getReg(BO->getOperand(1), rm)) {
          // Materialize small immediate into xN with MOVZ (imm<=0xFFFF).
          auto *CI = llvm::dyn_cast<llvm::ConstantInt>(BO->getOperand(1));
          if (!CI || CI->getValue().getActiveBits() > 16 || CI->getValue().isNegative()) {
            unsupported("op1 not reg nor small imm"); return r;
          }
          unsigned rtmp = 16; // use x16 as 1-shot materializer
          if (!W.emit(encMovz16(is64, rtmp, (uint16_t)CI->getZExtValue()))) {
            r.status = Status::TooLarge; return r;
          }
          rm = rtmp;
        }
        bool ok = false;
        switch (BO->getOpcode()) {
        case llvm::Instruction::Add: ok = W.emit(encAddSubReg(false, is64, rd, rn, rm)); break;
        case llvm::Instruction::Sub: ok = W.emit(encAddSubReg(true,  is64, rd, rn, rm)); break;
        case llvm::Instruction::Mul: ok = W.emit(encMul(is64, rd, rn, rm));               break;
        case llvm::Instruction::And: ok = W.emit(encLogicReg(0, is64, rd, rn, rm));       break;
        case llvm::Instruction::Or:  ok = W.emit(encLogicReg(1, is64, rd, rn, rm));       break;
        case llvm::Instruction::Xor: ok = W.emit(encLogicReg(2, is64, rd, rn, rm));       break;
        default: break;
        }
        if (!ok) { r.status = Status::TooLarge; return r; }
        break;
      }
      case llvm::Instruction::Shl:
      case llvm::Instruction::LShr:
      case llvm::Instruction::AShr: {
        auto *CI = llvm::dyn_cast<llvm::ConstantInt>(BO->getOperand(1));
        if (!CI) { unsupported("variable shift"); return r; }
        unsigned sh = (unsigned)CI->getZExtValue();
        if (sh >= (is64 ? 64u : 32u)) { unsupported("shift oversize"); return r; }
        bool ok = false;
        if (BO->getOpcode() == llvm::Instruction::Shl)       ok = W.emit(encLslImm(is64, rd, rn, sh));
        else if (BO->getOpcode() == llvm::Instruction::LShr) ok = W.emit(encLsrImm(is64, rd, rn, sh));
        else                                                 ok = W.emit(encAsrImm(is64, rd, rn, sh));
        if (!ok) { r.status = Status::TooLarge; return r; }
        break;
      }
      default: unsupported("binop kind"); return r;
      }
      continue;
    }

    if (auto *RI = llvm::dyn_cast<llvm::ReturnInst>(&I)) {
      if (llvm::Value *RV = RI->getReturnValue()) {
        unsigned rs;
        if (RM.getReg(RV, rs)) {
          if (rs != 0) {
            bool is64 = RV->getType()->isIntegerTy(64);
            if (!W.emit(encMovReg(is64, 0, rs))) { r.status = Status::TooLarge; return r; }
          }
        } else if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(RV)) {
          if (CI->getValue().getActiveBits() > 16 || CI->getValue().isNegative()) {
            unsupported("ret large const"); return r;
          }
          bool is64 = CI->getType()->isIntegerTy(64);
          if (!W.emit(encMovz16(is64, 0, (uint16_t)CI->getZExtValue()))) { r.status=Status::TooLarge; return r; }
        } else {
          unsupported("ret operand"); return r;
        }
      }
      if (!W.emit(kRet)) { r.status = Status::TooLarge; return r; }
      r.codeBytes = W.pos;
      return r; // done
    }

    // trunc/zext/sext: since we don't distinguish w/x physical widths at the
    // register level (AArch64 already aliases w<i> with x<i>), we just reuse
    // the source register as the destination SSA value.
    if (auto *CI = llvm::dyn_cast<llvm::CastInst>(&I)) {
      if (CI->getOpcode() == llvm::Instruction::Trunc ||
          CI->getOpcode() == llvm::Instruction::ZExt ||
          CI->getOpcode() == llvm::Instruction::SExt) {
        unsigned rn;
        if (!RM.getReg(CI->getOperand(0), rn)) { unsupported("cast src"); return r; }
        RM.reg[&I] = rn;
        continue;
      }
      unsupported("cast kind"); return r;
    }

    unsupported("opcode"); return r;
  }

  r.status = Status::Unsupported;
  r.reason = "fell off BB with no ret";
  return r;
}

void *light::compile(const llvm::Function &Fn, Result &out) {
  const size_t pageSize = (size_t)sysconf(_SC_PAGESIZE);
  void *page = ::mmap(nullptr, pageSize, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (page == MAP_FAILED) { out.status = Status::TooLarge; out.reason = "mmap"; return nullptr; }

  out = emit(Fn, (uint8_t *)page, pageSize);
  if (out.status != Status::Ok) { ::munmap(page, pageSize); return nullptr; }

  // flush i-cache for the region we wrote + flip to PROT_READ|PROT_EXEC.
  __builtin___clear_cache((char *)page, (char *)page + out.codeBytes);
  if (::mprotect(page, pageSize, PROT_READ | PROT_EXEC) != 0) {
    ::munmap(page, pageSize);
    out.status = Status::TooLarge; out.reason = "mprotect"; return nullptr;
  }
  return page;
}
