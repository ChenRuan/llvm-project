# EJIT IR Pipeline Runtime-Performance Audit

## Scope

This audit prioritizes steady-state execution time. JIT compilation is
asynchronous, so compile latency and moderate code-size growth are not used as
rejection criteria. The accompanying change implements the two pass additions
that survived this audit: final `InstSimplify` and target-aware loop
vectorization. Broader O2/SLP/GVN additions remain excluded.

Baseline: `dong/ejit_dev_spec4` at `d60b61f8da65`.

## Current pipeline

After period indices and `may_const` loads are specialized, the current fixed
pipeline is:

```text
LowerExpectIntrinsic
InstCombine, SCCP, SimplifyCFG, InstCombine, SimplifyCFG, ADCE
LoopSimplify
LoopFullUnroll, IndVarSimplify, LoopDeletion
Promote
StructFieldPass
InstCombine, SCCP, SimplifyCFG, ADCE
```

The requested optimization level is ignored. In particular, the post-JIT
pipeline has no target-aware loop vectorization and ends with ADCE, which can
expose simplifications after the last InstCombine/InstSimplify opportunity.

`EJitPassBuilder` also registers the generic `TargetIRAnalysis()` rather than
the active `TargetMachine::getTargetIRAnalysis()`. This is sufficient for the
current scalar pipeline, but not a sound profitability model for target-aware
vectorization.

## Evidence from captured EJIT IR

Three complete `IR after runOptimizationPipeline` modules from the complex
board test were extracted and compiled for `aarch64_be-target-linux-gnu`.
The current source pipeline has the same ordering as the captured build; the
newer `may_const` work changes which loads can be specialized, not the cleanup
ordering examined here.

Running a complete `default<O2>` pipeline after the EJIT pipeline produced no
uniform benefit: one module was unchanged, one shrank by 8 bytes, and one grew
by 4 bytes. Therefore, appending full O2 is not recommended.

### Residual scalar simplification

One real post-pipeline function retained this sequence:

```llvm
%sum = add i32 %a, %b
%iszero = icmp eq i32 %sum, 0
%copy = add i32 0, %sum
%result = select i1 %iszero, i32 0, i32 %copy
ret i32 %result
```

The result is always `%sum`. A final `InstSimplify` removes the conditional
selection without perturbing another captured module. For the affected
AArch64 function it removes one instruction and the flag-dependent `csel`
(84 to 80 bytes). A final `InstCombine` removes two instructions (84 to 76
bytes), but also grew another captured function by one instruction due to a
different select/add canonicalization. The conservative recommendation is:

```text
... cleanup SCCP, SimplifyCFG, ADCE, InstSimplify
```

This is a small but directly demonstrated runtime cleanup. If `InstCombine`
is preferred, it needs a broader machine-code benchmark before adoption.

## Targeted loop-vectorization experiment

The probe in `ejit_test/ejit_ir_pipeline_loop_probe.c` models a runtime-sized
loop over two arrays after lifecycle and `may_const` branches have already
been specialized:

```c
sum += lhs[i] * 3u + rhs[i] * 5u;
```

After the existing AOT-preoptimization approximation and the exact current
runtime pass sequence, the loop remains scalar. The scalar AArch64 function is
48 bytes. The smallest tested profitable extension is:

```text
LoopSimplify
LoopRotate
LoopVectorize
InstCombine
SimplifyCFG
```

It produces a NEON loop using `ld1`, `umull`/`umlal`, vector additions, and
`addv`, with a scalar remainder. The function grows to 352 bytes. Adding
`SLPVectorizer` does not change this probe's code or performance; SLP alone is
effectively identical to the scalar baseline, so it should not be added based
on this evidence.

Removing both `restrict` qualifiers produced the same vector code and speedup
for this read-only reduction. The result therefore does not depend on an
idealized no-alias annotation. Loops that also write through potentially
aliasing pointers may require runtime alias checks and need separate business
benchmarks.

Native AArch64 measurements were pinned to one Neoverse-N2 CPU and used
`cntvct_el0`. Absolute counter ticks are not CPU cycles, but the relative
results were stable across repeated runs:

| elements | scalar ticks/call | vector ticks/call | speedup |
|---------:|------------------:|------------------:|--------:|
| 4        | 0.168             | 0.136             | 1.23x   |
| 8        | 0.321             | 0.161             | 1.99x   |
| 16       | 0.645             | 0.224             | 2.87x   |
| 32       | 1.301             | 0.369             | 3.52x   |
| 64       | 2.607             | 0.652             | 4.00x   |
| 256      | 10.464            | 2.263             | 4.62x   |
| 1024     | 42.185            | 8.696             | 4.85x   |
| 4096     | 168.016           | 34.508             | 4.87x   |

The three captured complex-test specializations contain no remaining
vectorizable loops, so this pass does not improve that particular demo. Its
value is for real JIT functions that retain medium or large data loops after
specialization.

## Required production wiring

Do not add `LoopVectorizePass` to the current manager with generic TTI.
`EJitOrcEngine` already owns `dumpTM`, built from the same
`JITTargetMachineBuilder` as LLJIT. Pass that target machine (or its TTI
callback) into `EJitOptimizer`, then register:

```cpp
FAM.registerPass([&] { return TM->getTargetIRAnalysis(); });
FAM.registerPass([&] { return LoopAccessAnalysis(); });
```

The implementation also needs the Vectorize component in the trimmed link.
That increases the runtime image, but does not change shared-taskpool or code
pool ABI. Validate both little-endian AArch64 and `aarch64_be` output, and
compare final machine code for representative business functions before
enabling it unconditionally.

## Implemented scope

1. Add final `InstSimplify` after cleanup ADCE. This is low risk and is backed
   by a real EJIT post-pipeline dump.
2. Add target-aware `LoopRotate + LoopVectorize`, reusing the TargetMachine
   already created by `EJitOrcEngine`; no second TargetMachine is created.
3. Do not append full O2, SLP, GVN, or EarlyCSE merely because they are absent.
   Full O2 was inconsistent on captured modules, SLP did not help the loop
   probe, and duplicate-load experiments were already folded by AArch64
   CodeGen without GVN/EarlyCSE.
4. The remaining board acceptance item is to dump and diff one real loop-heavy
   business function and measure its final AArch64 code. Synthetic/native
   measurements prove the mechanism and potential, not every workload.
