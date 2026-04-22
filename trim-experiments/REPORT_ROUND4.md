# Round-4 Report — `llvm15_trim_llvm_backend`

Base for this round: `text = 21,249,059` (round-3 final, commit `4bb003853a74`).

## 1. New commits

| # | Commit | Title | Δ text (B) | Δ cum | Safety |
|---|--------|-------|-----------:|------:|--------|
| P10 | `9d5ec1d5a260` | `[CodeGen] Extend LLVM_CODEGEN_DISABLE_NONLINUX_EH to SelectionDAG/MF` | **−36,552** | −36,552 | ✅ |
| P11 | `8ffdf404ceb2` | `[Orc] Add LLVM_ORC_DISABLE_NON_ELF_PLATFORMS cmake option` | **−664** | −37,216 | ✅ |

**Round-4 net: −37,216 B.** **New minimum text = 21,211,843 B**.
Cumulative vs Phase-0 (26,819,671): **−5,607,828 B (−20.91%)**.

Validation at every step: `file` → statically linked · `ldd` → not a dynamic executable · `inc(4..7)=5..8` ✅

## 2. P10 — finish the `LLVM_CODEGEN_DISABLE_NONLINUX_EH` job

The round-3 option already wiped `SjLjEHPrepare.cpp.o` and the three prepare `addPass` sites, but two `.o`s remained in the link closure because of out-of-line symbol references:
- `WinEHPrepare.cpp.o` — pulled via `WinEHFuncInfo::WinEHFuncInfo()` / `~WinEHFuncInfo()` / `addIPToStateRange()`
- `WasmEHPrepare.cpp.o` — pulled via `~WasmEHFuncInfo()` / `calculateWasmEHInfo`

Changes (3 files, +33 lines):

| File | What it gates |
|---|---|
| `llvm/lib/CodeGen/SelectionDAG/FunctionLoweringInfo.cpp` | Win/Wasm header includes, `calculate{WinCXX,SEH,Clr}EHStateNumbers`, `calculateWasmEHInfo`, and the two MBB-remap blocks. `report_fatal_error` on unexpected personality. |
| `llvm/lib/CodeGen/MachineFunction.cpp` | `WinEHInfo` / `WasmEHInfo` ctor + dtor sites (the unconditional emission of calls to `WinEHFuncInfo::WinEHFuncInfo()` / `~WinEHFuncInfo()`). |
| `llvm/lib/CodeGen/SelectionDAG/SelectionDAGBuilder.cpp` | `WinEHFuncInfo::addIPToStateRange()` call in `lowerStartEH()`. |

### Result
- `libLLVMCodeGen.a(WinEHPrepare.cpp.o)` **removed** from link closure.
- `libLLVMCodeGen.a(WasmEHPrepare.cpp.o)` **removed** from link closure.
- text: 21,249,059 → **21,212,507** (−36,552 B).

### Trigger-path safety
All gated paths are on `isFuncletEHPersonality` / `isScopedEHPersonality` / `Personality == Wasm_CXX` branches that are dead on Linux/ELF C workloads. Each one falls through to a `report_fatal_error` instead of silently mis-lowering, so any regression is loud.

## 3. P11 — `LLVM_ORC_DISABLE_NON_ELF_PLATFORMS`

`libLLVMOrcJIT.a(ObjectFileInterface.cpp.o)` contains a 3-way `dyn_cast` dispatcher for MachO / ELF / COFF object files. The MachO branch drags in `MachOPlatform::isInitializerSection` (→ `MachOPlatform.cpp.o` ≈ 87 KB text) and `LookupAndRecordAddrs.cpp.o`; the COFF branch drags in `llvm/Object/COFF.h` types.

Changes (2 files, +21 lines):
- `llvm/lib/ExecutionEngine/Orc/CMakeLists.txt` — new option + `add_compile_definitions`.
- `llvm/lib/ExecutionEngine/Orc/ObjectFileInterface.cpp`:
  - Gate `#include "MachOPlatform.h"`, `"Object/COFF.h"`, `"Object/MachO.h"`.
  - Wrap both `getMachOObjectFileSymbolInfo` and `getCOFFObjectFileSymbolInfo` helper definitions.
  - Gate the MachO + COFF dispatcher arms in `getObjectFileInterface`. With the flag on, only the ELF helper is called; non-ELF objects fall through to the generic helper.

### Result
- `libLLVMOrcJIT.a(MachOPlatform.cpp.o)` **removed**.
- `libLLVMOrcJIT.a(LookupAndRecordAddrs.cpp.o)` **removed**.
- text: 21,212,507 → **21,211,843** (−664 B).

### Why the text delta is small
`--gc-sections` was already GC'ing ~99% of the code inside those `.o`s because nothing external was calling MachO/COFF entry points. What P11 actually removes is the symbol *references* that kept the `.o` members in the static-link closure. The `.o`-count drop (−2 members) is the more honest metric here; it's a structural cleanup more than a size win.

## 4. Size-delta timeline

| Stage | text (B) | Δ Phase-0 |
|---|---:|---:|
| Phase-0 | 26,819,671 | — |
| P1…P7 (rounds 1–2) | 21,530,131 | −5,289,540 |
| P8 CPU-specific | 21,509,027 | −5,310,644 |
| P9 EH/Outliner/SafeStack | 21,249,059 | −5,570,612 |
| **P10 NONLINUX_EH (SelDAG/MF)** | **21,212,507** | **−5,607,164** |
| **P11 ORC non-ELF** | **21,211,843** | **−5,607,828 (−20.91%)** |

## 5. Safety matrix (updated)

| Option | Round | Safe default for any JIT? |
|---|---|:-:|
| `LLVM_AARCH64_DISABLE_GISEL` | R1 | ✅ |
| `LLVM_DISABLE_INLINE_ASM_PARSER` | R1 | ⚠ review |
| `LLVM_DISABLE_DEBUG_INFO_EMISSION` | R1 | ⚠ review |
| `LLVM_DISABLE_IR_SYMBOL_TABLE_ASM_PARSING` | R1 | ✅ |
| `LLVM_PROFILEDATA_DISABLE_CORRELATOR` | R2 | ✅ |
| `LLVM_AARCH64_DISABLE_FASTISEL` | R2 | ✅ |
| `LLVM_AARCH64_DISABLE_HARDENING_PASSES` | R2 | ⚠ review |
| `LLVM_AARCH64_DISABLE_CPU_SPECIFIC_PASSES` | R3 | ✅ |
| `LLVM_CODEGEN_DISABLE_NONLINUX_EH` | R3+R4 | ✅ (fail-fast guarded) |
| `LLVM_CODEGEN_DISABLE_MACHINE_OUTLINER` | R3 | ✅ |
| `LLVM_CODEGEN_DISABLE_SAFESTACK` | R3 | ✅ |
| `LLVM_ORC_DISABLE_NON_ELF_PLATFORMS` | R4 | ✅ |

## 6. Can we keep going?

Short answer: **we are at the low-risk-obvious-trim bottleneck.** Round-4 net was only **−37 KB**, versus round-3's **−281 KB** and round-2's **~−2 MB**. The code that remains in the link closure is overwhelmingly exercised by the current scalar C JIT path.

### What's still in the picture — but with real risk
1. **`FunctionLoweringInfo.cpp` `sehEHStateNumbers` / statepoint helpers via ObjC retain/release bundle handling** — tiny, marginal.
2. **`FastISel.cpp` residual** — already disabled via cmake but `FastISel.cpp.o` still pulled because `SelectionDAGISel::TryToFoldFastISelLoad` or `FunctionLoweringInfo` references it. Gating would require header changes and risks non-local compile errors.
3. **SelectionDAG vector legalizers** (`LegalizeVectorTypes.cpp` ≈ very large, `LegalizeVectorOps.cpp`) — scalar-only add_int doesn't exercise them but they are called unconditionally from `LegalizeDAG`. Needs invariant proof first; high risk.
4. **`libLLVMCore.a` / `libLLVMAnalysis.a` pass-level opt-in** — potential ≥1 MB, but the refactor surface is huge and any JIT-facing IR-analysis regression would be hard to diagnose.
5. **GC / statepoint deep trim** — abandoned in round 3 (`ShadowStackGCLoweringPass` registration has implicit side effects that crash ORC). Would need to pre-analyse GC-strategy registration.

### Honest recommendation
- The **nine cmake options + three touch-up gates** we now have are a clean, committable patch series that an embedded-JIT consumer can adopt in one go. This is a good stopping point for the "low-risk source-level trim" phase.
- The remaining 21.2 MB is dominated by code actually used by the JIT (`LLVMCore`, `LLVMAnalysis`, `LLVMTransformUtils`, `LLVMScalarOpts`, `LLVMipo`, most of `LLVMCodeGen`, most of `LLVMSelectionDAG`, the AArch64 encoder/selector, `LLVMMC`, `LLVMSupport`). Any further wins require either **load-bearing refactors** (SelectionDAG vector types, statepoint, GC) or **feature-level opt-outs that change JIT semantics** (drop ipo/vectorize/loop passes). Both are outside the "don't touch the backbone" invariant.
- If the downstream target still needs more size, the next effective lever is almost certainly **LTO + `--icf=safe` on the final executable** (build-level), not more source gating — plus shrinking the runtime-side EasyJIT (front-end pipeline, `InlineParameters`, etc.).

### Verdict
Round-4 delivered two clean wins; round-5 diminishing returns unless we explicitly widen the risk budget.
