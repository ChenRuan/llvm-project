# EJIT SVE Vectorization

## Scope

`EJIT_SRE_SVE_VECTORIZATION` is an opt-in build feature for scalable-vector
optimization on AArch64. It is enabled by the `ejit-minimal-aarch64_be` preset
and is off by default.

| Compilation path | L1 | L2 | L3 |
| --- | --- | --- | --- |
| Baseline JIT | scalar | SLP + partial unroll | L2 + LoopVectorize + LoopLoadElimination |
| PGO Tier-1 (Gen) | no vector passes | no vector passes | no vector passes |
| PGO Tier-2 (Use) | scalar | SLP + partial unroll | L2 + LoopVectorize + LoopLoadElimination |

Tier-1 deliberately stops after the shared light optimization and profile
instrumentation stages. It does not run vector passes, so Gen/Use profile layout
and CFG hashes remain symmetric. Tier-2 runs the vector stage only after
profile use, value specialization, the final StructField pass, and cleanup.

Here, "no vector passes" describes the optimizer pass policy, not a guarantee
that generated machine code contains no vector instructions. The shared JIT
TargetMachine remains SVE-capable when this option is enabled, so target
lowering may still select SVE instructions outside the vectorizer. The
`ejit-minimal-aarch64_be` preset assumes SVE-capable hardware; there is no
runtime SVE feature probe.

The vector stage is also gated by the module target triple. A build with the
option enabled skips the vector passes for non-AArch64 modules; a build with
the option disabled does not link LLVMVectorize and keeps the existing EJIT
path.

## Pipeline

The shared prefix is period-index replacement, InstCombine, StructField,
interprocedural constant propagation, a second StructField pass, and module
cleanup (`ReversePostOrderFunctionAttrs`, dead-argument elimination, and
GlobalDCE). Baseline and Tier-2 then run LowerExpect, the selected O1/O2/O3
function simplification, the final StructField cleanup, and the tier-specific
vector stage. A final GlobalDCE runs before code generation.

The optimizer owns an independent TargetMachine created from the same
JITTargetMachineBuilder as ORC. This supplies target-accurate TTI and keeps the
optimizer's analysis lifetime independent from assembly-dump TargetMachines.
On AArch64, eligible functions receive `+sve` before the vector passes run.

Vectorization is allowed, not forced. Dependences, aliasing, trip counts,
alignment, and the target cost model may leave a loop scalar. Deploy an SVE
enabled image only on hardware that supports SVE.

## Verification

After a Baseline or Tier-2 version is published, use the existing dump APIs:

```text
ejit_dump_func("function_name")
ejit_print_dumped("function_name")
```

The optimized IR may contain scalable types such as `<vscale x 4 x i32>` and
the AArch64 assembly may contain `whilelo`, `ptrue`, `ld1w`, `st1w`, or `incw`.
The exact instruction sequence depends on the loop and the target cost model.
