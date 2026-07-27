//===-- EJitDirectCallStubs.h - AArch64 direct PLT stub rewrite plugin ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// ORC ObjectLinkingLayer plugin that optimizes AArch64 far calls after external
// symbols have been resolved.
//
// Background: the JIT code slab is SRE_MemAlloc'd ~1.5-2.2GB above AOT .text,
// beyond AArch64 BL's +-128MB reach (so a register-indirect long-call sequence
// is unavoidable) but within ADRP's +-4GB reach. JITLink normally routes
// external calls through a PointerJumpStub (ADRP x16,page;
// LDR x16,[x16,#off]; BR x16) that loads the
// target address from a GOT entry - one data memory load + one indirect branch
// per call. This plugin rewrites such stubs, when the target is within +-4GB,
// into a direct form (ADRP x16,page; ADD x16,x16,#off; BR x16) that addresses
// the target directly, dropping the GOT data load. The indirect branch remains
// unavoidable unless the slab can be placed within BL's +-128MB range.
//
// With EJIT_INLINE_LONG_CALLS, CodeGen emits ADRP+LDR+BLR at the call site. The
// plugin resolves the GOT edge and rewrites the LDR to ADD, producing
// ADRP+ADD+BLR with no separate stub. If the same helper is called repeatedly,
// CodeGen may hoist ADRP+LDR and reuse the resolved register, leaving only BLR
// at subsequent calls.
//
// Safety: if a target is beyond +-4GB (the slab base is uncontrolled), its
// address is unavailable, or the relocation/instruction shape is unexpected,
// the original GOT form is retained. AArch64 ELF only; no-op on other targets.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITDIRECTCALLSTUBS_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITDIRECTCALLSTUBS_H

#include "llvm/ExecutionEngine/JITLink/JITLink.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/LinkGraphLinkingLayer.h"

namespace llvm {
namespace ejit {

/// Rewrite inline AArch64 GOT address materializations that target callable
/// symbols from ADRP+LDR to ADRP+ADD. Exposed for deterministic LinkGraph unit
/// tests; production invokes it through EJitDirectCallStubsPlugin.
Error relaxAArch64InlineGOTCalls(jitlink::LinkGraph &G);

/// Plugin installed on the ORC ObjectLinkingLayer (when
/// EJIT_DIRECT_CALL_STUBS or EJIT_INLINE_LONG_CALLS is defined) to run the
/// selected AArch64 rewrites. The rewrite runs as a PreFixup JITLink pass: by
/// then external symbols have been resolved to absolute addresses and block
/// content is still mutable, but fixups have not yet been applied.
class EJitDirectCallStubsPlugin : public orc::LinkGraphLinkingLayer::Plugin {
public:
  void modifyPassConfig(orc::MaterializationResponsibility &MR,
                        jitlink::LinkGraph &G,
                        jitlink::PassConfiguration &Config) override;

  Error notifyFailed(orc::MaterializationResponsibility &MR) override {
    return Error::success();
  }
  Error notifyEmitted(orc::MaterializationResponsibility &MR) override {
    return Error::success();
  }
  Error notifyRemovingResources(orc::JITDylib &JD,
                                orc::ResourceKey K) override {
    return Error::success();
  }
  void notifyTransferringResources(orc::JITDylib &JD, orc::ResourceKey DstKey,
                                   orc::ResourceKey SrcKey) override {}

  static std::shared_ptr<EJitDirectCallStubsPlugin> create() {
    return std::make_shared<EJitDirectCallStubsPlugin>();
  }
};

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITDIRECTCALLSTUBS_H
