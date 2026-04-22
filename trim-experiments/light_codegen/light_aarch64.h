// light_aarch64.h — experimental narrow codegen for post-EasyJIT-specialized IR.
//
// Supports a *tiny* subset (round-5 PoC):
//   - one function, one basic block
//   - integer params (i1/i8/i16/i32/i64) in x0..x7
//   - ops:  add/sub/mul/and/or/xor/shl/lshr/ashr   (reg-reg or reg-imm12)
//   - sext/zext/trunc between i32/i64
//   - ret i32 / ret i64 / ret void
// Everything else -> LightCodegenError::Unsupported (fallback to full backend).
//
// The emitter writes AArch64 little-endian instruction words into a user-
// supplied buffer, returning the byte length on success.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace llvm { class Function; }

namespace light {

enum class Status { Ok, Unsupported, TooLarge, NotAarch64LE };

struct Result {
  Status status = Status::Ok;
  std::size_t codeBytes = 0;
  std::string reason; // human-readable when not Ok
};

// Emit machine code for Fn into [buf, buf+bufCap). Only a handful of IR
// patterns are supported; see the header comment above. Never emits calls
// or accesses memory.
Result emit(const llvm::Function &Fn, std::uint8_t *buf, std::size_t bufCap);

// Allocate an RWX page, emit Fn into it, return a callable function pointer
// (or nullptr on failure, with reason populated). The page is leaked on
// purpose — this is an experimental PoC, not a production allocator.
void *compile(const llvm::Function &Fn, Result &out);

} // namespace light
