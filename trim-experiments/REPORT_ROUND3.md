# Round-3 Report — `llvm15_trim_llvm_backend`

Base: `llvmorg-15.0.4` · Target: `aarch64-linux-gnu` · Test: static-link `add_int.c` JIT with 36 archives + `asmparser_stub.o`.

---

## 1. New commits in this round

| # | Commit | Title | Δ text (B) | Safety |
|---|--------|-------|-----------:|--------|
| P8 | `8971d59f3ec9` | `[AArch64] Add LLVM_AARCH64_DISABLE_CPU_SPECIFIC_PASSES cmake option` | **−21,104** | ✅ safe |
| P9 | `4bb003853a74` | `[CodeGen] Add embedded-JIT trim options for EH / Outliner / SafeStack` | **−259,968** | ✅ safe |

**Round-3 net: −281,072 B (−1.31%)**
**New minimum `text`: 21,249,059 B**
Cumulative vs Phase-0 (26,819,671 B): **−5,570,612 B (−20.77%)**
Cumulative vs historical 32.80 MB: ≈ **−35.2%**

---

## 2. P8 — `LLVM_AARCH64_DISABLE_CPU_SPECIFIC_PASSES`

### What it gates
All seven per-CPU / per-tuning passes in the AArch64 backend:

| Source file | Role |
|---|---|
| `AArch64A53Fix835769.cpp` | Cortex-A53 erratum 835769 |
| `AArch64A57FPLoadBalancing.cpp` | Cortex-A57 FP load balancing |
| `AArch64BranchTargets.cpp` | BTI (Branch Target Identification) |
| `AArch64FalkorHWPFFix.cpp` | Qualcomm Falkor HW prefetcher fix |
| `AArch64FalkorMarkStridedAccesses` | Falkor strided accesses marker |
| `AArch64SVEIntrinsicOpts.cpp` | SVE intrinsic optimizer |
| `AArch64CondBrTuning.cpp` | Conditional-branch tuning |

Changes: 1 `option()` in `llvm/lib/Target/AArch64/CMakeLists.txt`; 7 `initialize*Pass` calls + 6 `addPass` sites in `AArch64TargetMachine.cpp` wrapped in `#ifndef LLVM_AARCH64_DISABLE_CPU_SPECIFIC_PASSES`.

### Verified drops
Six `.cpp.o` members removed from `libLLVMAArch64CodeGen.a`'s link closure: `A53Fix835769`, `A57FPLoadBalancing`, `BranchTargets`, `FalkorHWPFFix`, `FalkorMarkStridedAccesses`, `CondBrTuning`.

### Residual
`SVEIntrinsicOpts.cpp.o` is still pulled by `AArch64TargetTransformInfo.cpp.o` (via `AArch64TTIImpl::areInlineCompatible`). Dropping it requires gating the TTI call too — deferred.

### Why the size delta is small (−21 KB for 6 object files)
These per-CPU passes are small and heavily reference-counted from common utilities; most of their code size was already attributed to shared helpers that remain pulled by other passes.

---

## 3. P9 — `LLVM_CODEGEN_DISABLE_{NONLINUX_EH, MACHINE_OUTLINER, SAFESTACK}`

### What it gates

**`LLVM_CODEGEN_DISABLE_NONLINUX_EH`** — removes Win/Wasm/SjLj exception preparation from the CodeGen pipeline:
- `initialize{SjLj,Wasm,Win}EHPreparePass` registrations
- `switch` cases for `ExceptionHandling::{SjLj, WinEH, Wasm}` in `TargetPassConfig::addPassesToHandleExceptions`

**`LLVM_CODEGEN_DISABLE_MACHINE_OUTLINER`** — drops machine-level function outlining:
- `initializeMachineOutlinerPass` registration
- `addPass(&MachineOutlinerID)` block in `addMachinePasses`

**`LLVM_CODEGEN_DISABLE_SAFESTACK`** — drops SafeStack (separate stack for unsafe locals):
- `initializeSafeStackLegacyPassPass` registration
- `createSafeStackPass()` in `addISelPrepare`

Changes (3 files, +32 lines):
- `llvm/lib/CodeGen/CMakeLists.txt` (+16 / 3 options + `add_compile_definitions`)
- `llvm/lib/CodeGen/CodeGen.cpp` (+8 / 4 `#ifndef` blocks)
- `llvm/lib/CodeGen/TargetPassConfig.cpp` (+8 / 2 `#ifndef` blocks)

### Verified drops
`SjLjEHPrepare.cpp.o`, `MachineOutliner.cpp.o`, `SafeStack.cpp.o`, `SafeStackLayout.cpp.o` all removed from `libLLVMCodeGen.a` link closure.

### Residual
`WinEHPrepare.cpp.o` + `WasmEHPrepare.cpp.o` remain: pulled from `SelectionDAG/FunctionLoweringInfo.cpp` via `calculate{WinCXX,SEH,Clr}EHStateNumbers` / `calculateWasmEHInfo`. Dropping them would require the same `#ifndef LLVM_CODEGEN_DISABLE_NONLINUX_EH` gate in `FunctionLoweringInfo.cpp` — candidate for round 4.

---

## 4. Attempted but abandoned — `LLVM_CODEGEN_DISABLE_GC_AND_STATEPOINT`

Originally planned as the 4th option in this round. Covered `FixupStatepointCallerSaved`, `GCMachineCodeAnalysis`, `GCModuleInfo`, `ShadowStackGCLowering`, `StackMapLiveness` initializations plus the corresponding `addPass` sites.

### Failure mode
All four options enabled → text **21,198,835 B** but **segmentation fault at runtime**.

### Bisection result

| Configuration | text | Result |
|---|---:|---|
| EH only | 21,462,707 | ✅ runs |
| EH + Outliner + SafeStack | 21,249,059 | ✅ runs |
| + GC/Statepoint (all gates) | 21,198,835 | ❌ SIGSEGV |
| Only `ShadowStackGCLoweringPass` init gated | — | ❌ SIGSEGV |
| Only `addPass` sites gated (inits kept) | ≈ same | ❌ SIGSEGV + no size win (anchor objects still pulled) |

### Root cause (hypothesis)
`ShadowStackGCLoweringPass` (and/or `GCMachineCodeAnalysis`) has implicit registration side effects the ORC/codegen pipeline relies on during pipeline construction. A proper gate requires teaching `TargetPassConfig::addPassesToHandleExceptions` and the GC-strategy lookup paths to skip gracefully, not just suppressing registration. Out of scope for the "low-risk only" invariant of this round.

### Conclusion
Option removed from CMakeLists.txt; tree is clean. Re-attempt only with a proper invariant analysis of GC-strategy registration.

---

## 5. Priority 3 (SelectionDAG) — deferred

The round-3 brief said Priority 3 was "only if Priorities 1 & 2 are stable". Priority 2 partially failed (GC gate), so Priority 3 was not attempted.

Additionally, first-look investigation of `StatepointLowering.cpp.o` showed that `StatepointLoweringState` is a **direct member** of `SelectionDAGBuilder` (`SelectionDAGBuilder.h:135`) — meaning the object is pulled via ctor/dtor regardless of any gate on the intrinsic switch cases. Gating it safely would require conditionally removing the member field and every `LowerAsSTATEPOINT` reference, which is precisely the "SelectionDAG backbone refactor" the brief excluded.

---

## 6. Size-delta timeline (entire branch)

| Stage | text (B) | Δ Phase-0 |
|---|---:|---:|
| Phase-0 baseline | 26,819,671 | — |
| P1 GISel | 25,982,467 | −837,204 |
| P2 InlineAsmParser | 25,729,079 | −1,090,592 |
| P3 DebugInfoEmission | 23,989,719 | −2,829,952 |
| P4 IRSymbolTable | 22,947,431 | −3,872,240 |
| P5 ProfDataCorrelator | 22,765,723 | −4,053,948 |
| P6 FastISel | 22,130,935 | −4,688,736 |
| P7 AArch64Hardening | 21,530,131 | −5,289,540 |
| **P8 CPU-specific** | **21,509,027** | **−5,310,644** |
| **P9 EH/Outliner/SafeStack** | **21,249,059** | **−5,570,612 (−20.77%)** |

Validation at every step: `file` → statically linked; `ldd` → not dynamic; runtime → `inc(4..7)=5..8`. ✅

---

## 7. Safety matrix

| Option | Scope | Safe default for any JIT? |
|---|---|:-:|
| `LLVM_AARCH64_DISABLE_GISEL` | GlobalISel path | ✅ |
| `LLVM_DISABLE_INLINE_ASM_PARSER` | Inline-asm parsing | ⚠ review |
| `LLVM_DISABLE_DEBUG_INFO_EMISSION` | DWARF emission | ⚠ review |
| `LLVM_DISABLE_IR_SYMBOL_TABLE_ASM_PARSING` | Module asm parser | ✅ |
| `LLVM_PROFILEDATA_DISABLE_CORRELATOR` | Debug-info correlator | ✅ |
| `LLVM_AARCH64_DISABLE_FASTISEL` | FastISel on AArch64 | ✅ |
| `LLVM_AARCH64_DISABLE_HARDENING_PASSES` | SLH / ReturnAddrSigning / stack tagging | ⚠ review |
| `LLVM_AARCH64_DISABLE_CPU_SPECIFIC_PASSES` | per-CPU errata / tuning | ✅ |
| `LLVM_CODEGEN_DISABLE_NONLINUX_EH` | SjLj / Win / Wasm EH | ✅ |
| `LLVM_CODEGEN_DISABLE_MACHINE_OUTLINER` | Function outlining | ✅ |
| `LLVM_CODEGEN_DISABLE_SAFESTACK` | SafeStack pass | ✅ |

---

## 8. Candidates for round 4

1. **`FunctionLoweringInfo.cpp` EH helpers** — gate `calculate{WinCXX,SEH,Clr}EHStateNumbers` + `calculateWasmEHInfo` behind `LLVM_CODEGEN_DISABLE_NONLINUX_EH`. Directly drops the last two Win/Wasm EH `.o`s.
2. **`SelectionDAGBuilder` statepoint member** — only if we accept a bounded refactor: gate `StatepointLoweringState StatepointLowering;` + the 4 intrinsic switch cases + `visitInvoke` path under a new `LLVM_CODEGEN_DISABLE_GC_AND_STATEPOINT`. Estimated drop: whole `StatepointLowering.cpp.o` ≈ moderate. Needs careful testing.
3. **`libLLVMCodeGen.a` GC strategy registration** — re-approach the abandoned GC gate by first mapping out `GCStrategy`/`GCMetadata` anchor references, then gating registration and the matching `TargetPassConfig` switch together.
4. **`libLLVMCore.a` / `libLLVMAnalysis.a` opt-in trimming** — pass-level opt-in; high potential (~1 MB) but high risk and much wider surface. Only after the CodeGen side is fully squeezed.
5. **`libLLVMOrcJIT.a` internals** — MachO/COFF-specific loaders, PerfSupport. Pure win for an ELF-only aarch64 JIT if gated.

Recommendation for round 4: items **1 → 5 → 3** in that order (safe first, risky GC last).
