# EasyJIT aarch64 static-binary LLVM-backend trim study

Scope: reduce the text size of `add_int` fully-static binary on aarch64 by
trimming the LLVM backend that `libEasyJitRuntime.a` drags in. Measurements
are done on `out/*`, link cmd reproducible via `link_static_add_int.sh`.

Invariants: `-static -no-pie -O2 -s -Wl,--gc-sections`, `--start-group ...
--end-group`, plus `minimal_libstdcpp.o + libsupc++.a + libgcc_eh.a +
libgcc.a`. `add_int` uses only the C API (`easy_jit_with_args`).

## Headline

| Phase | Description                                        | text (B)    | Δ vs baseline | Δ %   |
|:-----:|:---------------------------------------------------|:-----------:|:-------------:|:-----:|
| 0     | Reproducible baseline (all 37 archives, `--gc-sections`) | 26,819,671 | —             | —     |
| 2     | Stub `LLVMInitializeAArch64AsmParser` + drop AsmParser archive | 25,816,847 | −1,002,824 | **−3.74 %** |
| 3     | (cumulative) + `-ffunction-sections -fdata-sections` on EasyJitRuntime_static | 25,757,303 | −1,062,368 | **−3.96 %** |

All binaries run correctly:

```
inc(4) is 5
inc(5) is 6
inc(6) is 7
inc(7) is 8
```

Pre-study historical baseline `add_int_trim_static_minstd_full` was
32,795,018 B — roughly 6 MB of that was debug/meta sections that
`--gc-sections` alone removes. The honest starting point for backend
trimming is the 26.8 MB number above.

## Ablation: “drop one archive”

`ablation_dropone.sh` removed each LLVM archive individually from the link
line. Of 26 candidates, **only `libLLVMTextAPI.a` is freely droppable**
(−4,096 B). Everything else fails with unresolved refs from other
archives in the group, confirming the current 37-archive list is a tight
transitive closure.

Freely-dropped archives:
- `libLLVMTextAPI.a` … −4,096 B.

## Root-cause wins (Phase 2): force-reference stubbing

`runtime/InitNativeTarget.cpp` contains deliberately-unused references to
`LLVMInitialize*` symbols so the corresponding archive members are
retained when `--as-needed` is on. `LLVMInitializeAArch64AsmParser` alone
dragged in **`libLLVMAArch64AsmParser.a` ≈ 938 KB plus transitive
members** that the JIT never actually uses at runtime.

Fix applied in this study (not committed to runtime): provide a no-op
stub object linked **before** `--start-group` so the linker satisfies
the force-reference from the stub instead of pulling the archive:

```c
/* stubs/asmparser_stub.c */
void LLVMInitializeAArch64AsmParser(void) {}
```

Result: `text 26,819,671 → 25,816,847` (−1,002,824 B, −3.74 %).

Note that other `LLVMInitialize*` referenced there (TargetInfo,
TargetMC, Target, AsmPrinter) are genuinely needed by ORC JIT codegen
and cannot be stubbed without disabling JIT.

## Incremental source-level win (Phase 3): `-ffunction-sections`

Added to `runtime/CMakeLists.txt` for the `EasyJitRuntime_static` target
only (shared .so is unaffected). Unlocks per-function GC inside each
runtime TU.

Result: additional −59,544 B (−0.23 %). Marginal because most runtime
functions are transitively reachable from `Function::Compile`.

Committed upstream in this worktree:
`a63b27d  cmake: add -ffunction-sections -fdata-sections to EasyJitRuntime_static`.

## Remaining size breakdown (Phase 3 binary)

Per-archive text contribution (filtered, approximate):

| archive                     |      bytes |
|:----------------------------|-----------:|
| libLLVMCodeGen.a            |  4,795,934 |
| libLLVMAArch64CodeGen.a     |  4,472,003 |
| libLLVMSelectionDAG.a       |  2,803,708 |
| libLLVMCore.a               |  2,297,838 |
| libLLVMAnalysis.a           |  2,020,253 |
| libLLVMAArch64Desc.a        |  1,711,410 |
| libLLVMTransformUtils.a     |    987,435 |
| libEasyJitRuntime.a         |    963,629 |
| libLLVMGlobalISel.a         |    875,335 |
| libLLVMAsmPrinter.a         |    782,037 |
| libLLVMSupport.a            |    770,969 |
| libLLVMMC.a                 |    716,756 |
| libLLVMScalarOpts.a         |    551,185 |
| libLLVMObject.a             |    439,409 |
| libLLVMMCParser.a           |    283,954 |
| libLLVMBitReader.a          |    240,357 |
| libLLVMBitWriter.a          |    172,891 |
| libLLVMDebugInfoCodeView.a  |    172,052 |
| libLLVMOrcJIT.a             |    171,949 |
| ... (17 more smaller)       |            |

(`analyze_map.sh` over-attributes section sizes to the first .o in each
archive due to a known cosmetic bug; the totals per archive are
reliable.)

## Force-reference map: who pulls what

| target archive            | first-pulled by                                                |
|:--------------------------|:---------------------------------------------------------------|
| libLLVMBitWriter.a        | `EasyJitRuntime.a(Utils.cpp.o) → WriteBitcodeToFile`           |
| libLLVMLinker.a           | `EasyJitRuntime.a(Function_compile.cpp.o) → createStripDeadPrototypesPass` + `DevirtualizeConstant.cpp.o → Linker::linkModules` + `InlineParameters.cpp.o → Linker::linkModules` |
| libLLVMipo.a              | `EasyJitRuntime.a(Function_compile.cpp.o) → createGlobalDCEPass` + `createFunctionInliningPass` |
| libLLVMCFGuard.a          | `libLLVMBitWriter.a(BitcodeWriter.cpp.o)`                      |
| libLLVMDebugInfoCodeView.a| `libLLVMAsmPrinter.a(CodeViewDebug.cpp.o)`                     |
| libLLVMDebugInfoDWARF.a   | `libLLVMAsmPrinter.a(DwarfDebug.cpp.o)`                        |
| libLLVMRemarks.a          | `libLLVMCore.a(LLVMRemarkStreamer.cpp.o)`                      |
| libLLVMProfileData.a      | `libLLVMOrcJIT.a(ExecutorProcessControl.cpp.o) → runAsMain`    |
| libLLVMMCParser.a         | `libLLVMAsmPrinter.a(AsmPrinterInlineAsm.cpp.o)` + `libLLVMMC.a(MachObjectWriter.cpp.o)` |
| libLLVMGlobalISel.a       | `libLLVMAArch64CodeGen.a(AArch64PreLegalizerCombiner.cpp.o) → GISelCSEAnalysisWrapperPass::ID` |

## Next-phase candidates (not yet executed)

Ranked by expected payoff vs. difficulty:

1. **Source-level: replace `easy::CloneModuleWithContext` bitcode-roundtrip hack.**
   `Utils.cpp::CloneModuleWithContext` writes bitcode and re-parses it
   just to move a module across `LLVMContext`. It is the *only*
   consumer of `WriteBitcodeToFile` in EasyJIT runtime. Possible
   replacements:
   - keep a single `LLVMContext` per compile (restructure
     `CompileAndWrap`) — best, removes need entirely;
   - use `ThreadSafeModule::cloneToNewContext` — same cost, still
     pulls `BitWriter`;
   - re-implement clone via `ValueToValueMap` walk — medium effort,
     no new dependency.
   Expected drop: `libLLVMBitWriter.a` (≈173 KB) + `libLLVMCFGuard.a`
   (transitive) ≈ **180–220 KB**.

2. **Source-level: replace heavy IPO passes in `Function_compile.cpp`.**
   `createGlobalDCEPass` and `createStripDeadPrototypesPass` are added
   *after* `createInternalizePass`, as cleanup. At JIT time on an
   already-internalized module they can be approximated by a
   per-function DCE + removing dead declarations with a short local
   loop (same effect on our modules). `createFunctionInliningPass` is
   harder to remove — it inlines the wrapper. If only the Inliner
   remains, `libLLVMipo.a` can still be kept but GlobalDCE +
   StripDeadPrototypes TUs stop being reachable.
   Expected drop: partial shrink of `libLLVMipo.a` (~40–60 KB).

3. **LLVM patch: stub out AsmPrinter DWARF/CodeView emission.**
   `DwarfDebug.cpp.o` (177 KB) + `CodeViewDebug.cpp.o` (134 KB) +
   `DwarfUnit` + `DwarfCompileUnit` + `AccelTable` + `DIEHash` +
   `DbgEntityHistoryCalculator` + … ≈ **570 KB** inside
   `libLLVMAsmPrinter.a`, plus transitive pulls of
   `libLLVMDebugInfoDWARF.a` and `libLLVMDebugInfoCodeView.a`
   (~270 KB combined). A JIT that always feeds a stripped module
   never executes these paths. Possible patch: patch `AsmPrinter.cpp`
   to never instantiate `DwarfDebug`/`CodeViewDebug` when the module
   has no debug-info metadata, then stub those TUs.
   Expected drop: **up to ~800 KB**.

4. **LLVM patch: turn off GlobalISel inside AArch64 backend.**
   `AArch64CodeGen` unconditionally references `GISelCSEAnalysisWrapperPass::ID`
   through the pre-legalizer combiner pass, pulling `libLLVMGlobalISel.a`
   (875 KB). AArch64 compiles fine with SelectionDAG only at -O2.
   Patch scope: guard the `G_*` passes in
   `AArch64TargetMachine::addIRPasses`/`addPreLegalizeMachineIR` behind a
   build-time option; remove the pass-ID force-reference.
   Expected drop: **~850 KB**.

5. **LLVM patch: split `libLLVMMCParser`.**
   `AsmPrinterInlineAsm.cpp` calls `createMCAsmParser` for inline-asm in
   ordinary IR. JIT input has no inline asm. Stubbing
   `createMCAsmParser` (and the `AsmPrinter::emitInlineAsm` call site)
   would break the force reference into MCParser.
   Expected drop: **~280 KB**.

## Reproducer files in this directory

| file                        | purpose                                       |
|:----------------------------|:----------------------------------------------|
| `link_static_add_int.sh`    | reproducible static link driver               |
| `archives_baseline.txt`     | 37 archives used as baseline                  |
| `archives_phase2.txt`       | 35 archives (AsmParser removed)               |
| `stubs/asmparser_stub.c`    | Phase-2 stub for `LLVMInitializeAArch64AsmParser` |
| `analyze_map.sh`            | per-archive text attribution from ld -Map     |
| `ablation_dropone.sh`       | automates “drop one archive” study            |
| `out/baseline`              | Phase-0 binary (26,819,671 B)                 |
| `out/p2_stubasm`            | Phase-2 binary (25,816,847 B)                 |
| `out/p3_fsec`               | Phase-3 binary (25,757,303 B)                 |

## Commits produced in this round

- `a63b27d` (easy-jit-llvm15, branch `llvm15_trim`): enable
  `-ffunction-sections -fdata-sections` on `EasyJitRuntime_static`.

Branch for deeper LLVM-side trims: `llvm15_trim_llvm_backend` on
`/home/ruanchen/workspace/llvm-project-15.0.4` (off `llvmorg-15.0.4`).
No LLVM source patches applied yet — see "Next-phase candidates" above.
