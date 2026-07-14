# EJIT Code-Generation Performance Audit

Status: implementation and host validation complete; board validation pending

Baseline: `dong/ejit_dev_spec4` at `68ed1d88d8be`

Branch: `codex/ejit-codegen-perf-audit`

## Resume Checkpoint

Last updated: 2026-07-14

- The AArch64 code-pool path now explicitly selects `CodeModel::Small` when
  `EJIT_SRE_CODE_POOL_SIZE <= INT32_MAX`; other configurations retain Large.
- Controlled big-endian object, relocation, far-address JITLink, negative
  control, and native AArch64 timing experiments are complete and recorded
  below.
- The production macro combination (`SRE code pool + taskpool + shared
  taskpool`) builds `LLVMEJIT` successfully with `-j8`; focused code-pool tests
  pass 40/40.
- Working files are `EJitOrcEngine.cpp`, this document,
  `ejit_test/codegen_model_probe.ll`, and
  `ejit_test/codegen_model_probe_bench.c`. Nothing has been pushed.

To reproduce the production-combination build:

```bash
cd /home/ruanchen/worktrees/ejit-codegen-perf-audit
cmake -S llvm -B build_release_aarch64_be \
  -DEJIT_SRE_CODE_POOL=ON \
  -DEJIT_SRE_TASKPOOL=ON \
  -DEJIT_SRE_SHARED_TASKPOOL=ON
cmake --build build_release_aarch64_be --target LLVMEJIT -j8
```

## Scope

This audit looks beyond the shared-taskpool cache-query work and asks whether
the generated JIT function itself is systematically more expensive than the
equivalent AOT function. The primary suspects are target-machine configuration,
external symbol addressing, optimization-pipeline differences, helper inlining,
and global-variable access.

Out of scope:

- the `-O2`-only AsmLexer crash;
- cache slot-depth and fixed-dimension query optimizations covered by PR #82;
- code-pool permission behavior, dump diagnostics, and backend trimming;
- changing `may_const` semantics.

## Questions

1. Does `CodeModel::Large` add avoidable materialization or indirect branches?
2. Does clearing `dso_local` for every external declaration force GOT/PLT access
   where a registered absolute symbol could use a cheaper form?
3. Does the JIT pipeline produce different IR from the AOT pipeline after
   specialization?
4. Are helper calls left uninlined in the embedded bitcode?
5. Are global loads/stores more expensive in JIT code because the JIT text and
   application data are far apart?
6. Is the observed wrapper-vs-AOT delta actually inside the JIT body rather
   than in cache lookup and indirect dispatch?

## Initial Evidence

- `EJitOrcEngine::Create` unconditionally sets `CodeModel::Large`.
- `loadBitcodeModule` clears `dso_local` on every external function and global
  for AArch64 ELF modules, explicitly selecting GOT/PLT-style indirection.
- The name-filtered assembly dump uses a `TargetMachine` created from the same
  `JITTargetMachineBuilder`, so it can be used to compare actual JIT codegen
  configuration with an AOT control build.
- The EJIT/LLVM runtime library is built size-oriented (`-Os`), but generated
  JIT code uses `CodeGenOptLevel::Default` and preserves optimization-related
  attributes from the business bitcode. The controlled codegen probes use
  `-O2`; runtime-library and generated-function optimization levels must not be
  conflated.

## Candidate Directions

| Direction | Potential benefit | Main risk | State |
|---|---:|---|---|
| Smaller code model | fewer address-materialization instructions | relocation range failures | implemented, validating |
| Selective `dso_local` clearing | cheaper direct references | JIT text/data may be out of range | rejected without placement contract |
| Pipeline alignment | fewer instructions in JIT body | compile latency and code size | report only; needs real IR |
| Better helper inlining | remove calls and expose constants | code growth | report only; needs real IR |
| Near-data/code allocation | cheaper addressing and branch prediction | platform allocator constraints | future architecture work |
| Per-specialization module pruning | less optimization/codegen/link work and pool use | indirect reachability | highest-value follow-up |
| Remove unused unwind metadata | smaller objects and allocation | loss of stack-unwind diagnostics | report only |
| Target-specific CPU/features | better instruction selection | board compatibility | report only |

## Measurement Rules

- Compare identical source semantics and target triple.
- Separate direct AOT calls, direct JIT function-pointer calls, and wrapper JIT
  hits.
- Record IR, assembly, instruction counts, loads/stores, branches, and cycles.
- Do not count diagnostic printing in timed regions.
- Do not claim a benefit from a synthetic case without checking a realistic
  external-global and helper-call workload.
- Reject changes that merely hide relocation failures or weaken correctness.

## Findings Log

### F1. Removing the explicit Large model is a no-op

AArch64's `getEffectiveAArch64CodeModel` selects `CodeModel::Large` for a JIT
target when no model is specified. Therefore deleting
`JTMBOrErr->setCodeModel(CodeModel::Large)` would not change generated code.
Any experiment must explicitly select another model.

### F2. Small model may be range-safe if external references stay indirect

`loadBitcodeModule` clears `dso_local` on external functions and globals.
AArch64 codegen consequently emits GOT references for external data. AArch64
JITLink's `PLTTableManager` transforms a branch to an undefined symbol into a
near pointer-jump stub backed by a GOT entry. The real external address is thus
not required to be within the direct branch range of JIT text.

This suggests a potentially safe split:

- use the Small model for sections defined in the JIT object, which JITLink's
  `BasicLayout` places together in one code-pool allocation;
- keep all process-external declarations non-`dso_local`, so arbitrary process
  addresses continue to use GOT/PLT indirection.

The relocation and placement experiments in F4-F5 validate this split for the
current code-pool allocator. In particular, far external addresses continue to
link through GOT/PLT entries while object-local sections remain in one bounded
allocation.

### F3. JIT and AOT optimization pipelines are intentionally different

Release PASS1 pre-optimization runs `AlwaysInliner`, an O2 module inliner,
promotion, EarlyCSE, InstCombine, and SimplifyCFG before serializing bitcode.
The runtime `EJitOptimizer` then specializes dimensions and runs a compact
constant-propagation/CFG/loop pipeline, but does not inline. Debug PASS1 skips
pre-optimization entirely. Performance comparisons must therefore use Release
embedded bitcode or explicitly report that helpers may remain uninlined.

### F4. Controlled Large-versus-Small probe

`ejit_test/codegen_model_probe.ll` was compiled with LLVM 21.1.8 for
`aarch64_be-unknown-linux-gnu`, `-O2`, static relocation, and both code models.

| Probe | Large | Small | Difference |
|---|---:|---:|---:|
| external global load | 5 instructions | 5 instructions | none; both use GOT |
| tail call to external function | 1 | 1 | none; JITLink adds a local stub |
| external function-pointer call | 4 | 4 | none; both use GOT + indirect branch |
| local constant-table load | 7 | 5 | Small saves two address materializers |
| local switch-table lookup | 9 | 7 | Small saves two address materializers |
| total `.text` (original four probes) | 92 bytes | 76 bytes | -17.4% |

Large emits four `movz/movk` address relocations for each local table. Small
emits `adrp+add`. External data remains
`R_AARCH64_ADR_GOT_PAGE/R_AARCH64_LD64_GOT_LO12_NC`, and external calls remain
`R_AARCH64_CALL26/R_AARCH64_JUMP26` for JITLink's PLT pass.

After adding the external function-pointer boundary probe, the complete object
changed as follows: `.text` 108 to 92 bytes (-14.8%), `.rela.text` 312 to 216
bytes (-30.8%), `.eh_frame` 176 to 120 bytes (-31.8%), and total ELF size 2096
to 1928 bytes (-8.0%). These are probe-specific, but they also show reduced
JITLink relocation work rather than only shorter executable text.

### F5. Far external addresses do not invalidate the Small-model split

The Small-model big-endian object was linked with `llvm-jitlink` using a 2MiB
slab at `0xf000000000`, an external global at `0x100000000`, an external
function at `0x200000000`, and an external function-pointer variable at
`0x300000000`. Linking succeeded. The post-fixup graph showed GOT entries and a
12-byte pointer-jump stub in the slab next to JIT text. No direct PC-relative
edge targeted those far process addresses.

As a negative control, the same external global was marked `dso_local`. Small
then emitted `R_AARCH64_ADR_PREL_PG_HI21` plus a direct load, and the identical
far-address link failed with an out-of-range Page21 relocation. This confirms
both sides of the rule: Small is safe here because the loader clears
`dso_local`; removing that normalization to save one GOT load is not safe.

Other checked boundaries:

- external function-pointer variables remain GOT-based and link successfully
  at an arbitrary far address;
- a `GlobalAlias` must alias a definition, so a legal alias remains in the
  same LinkGraph allocation rather than silently aliasing an unresolved far
  declaration;
- inline assembly controls its own relocation spelling. An inline-asm `adrp`
  naming a far process symbol can be out of range under either code model and
  is not made newly unsafe by selecting Small for compiler-generated code;
- TLS relocations exist in AArch64 JITLink, but TLS is outside the current SRE
  runtime contract and was not used to justify this change.

`EJitCodePoolMemoryManager` obtains one contiguous `BasicLayout` size and one
pool allocation for the whole graph. A single allocation larger than the pool
size is rejected. With the default 2MiB pool, every direct object-local Small
model edge is therefore far inside its architectural range.

The additional Small-model local references use the same AArch64 Page21 and
PageOffset12 JITLink fixup kinds already exercised on big-endian builds by the
existing external-global GOT path; this does not introduce a new endian-only
fixup implementation.

### F6. Implemented selection rule

For AArch64 builds with `EJIT_SRE_CODE_POOL`, the target machine now uses the
Small code model. All other targets and memory-manager configurations retain
Large. No new build macro or runtime knob was added: the optimization is tied
to the existing allocator invariant that makes it safe.

The selection also checks that configured pool size is at most `INT32_MAX`,
covering Small-model signed 32-bit unwind deltas. A future oversized pool
configuration automatically retains Large instead of violating the invariant.

### F7. A specialization currently code-generates unrelated definitions

The embedded blob is extracted for the union of entry functions in a source
module. At runtime `loadBitcodeModule` parses that complete blob and
`EJitOptimizer::runOptimizationPipeline` visits every defined function. ORC
then emits the whole remaining module although the specialization JITDylib is
looked up only by `origFnName`.

This is visible in an existing board dump: compiling `process_multi_dim`
produced final IR containing definitions for `process_multi_dim`,
`process_all_trps`, and `process_slice_loop`, and the object contained all
three bodies. The unused definitions consume optimizer, CodeGen, JITLink,
code-pool, sealing, and I-cache resources for every specialization.

The same log makes the waste quantifiable for that compile: `.text` was 100
bytes, the requested constant-return `process_multi_dim` occupied 8 bytes, and
the other two emitted bodies occupied the remaining 92 bytes. Thus 92% of that
graph's text was unrelated to the requested lookup. This is one small demo, not
a fleet-wide percentage, but it proves the mechanism with production code.

The synthetic probe gives a second bound. Internalize + GlobalDCE preserving
one entry reduced Small-model `.text` from 92 to 20 bytes, removed the 88-byte
`.rodata` section, and reduced `.eh_frame` from 120 to 40 bytes.

A promising follow-up is to internalize definitions except `origFnName` and
run GlobalDCE before the specialization pipeline. That preserves direct and
IR-visible address-taken helper closure automatically. It needs dedicated
coverage for indirect calls, global initializers, aliases, `llvm.used`, and
inline-assembly symbol references before adoption. This optimization should be
a separate commit from the code-model change.

### F8. Production bitcode carries unwind and generic-CPU attributes

Real board IR contains `nounwind sspstrong uwtable`,
`"frame-pointer"="non-leaf"`, `"target-cpu"="generic"`, and generic Armv8
features. Consequences:

- every emitted function contributes unwind data even though the freestanding
  runtime does not use C++ exceptions;
- non-leaf functions may retain frame-pointer setup useful for diagnostics but
  costly on a hot path;
- target-specific instruction selection is unavailable.

Removing unwind/frame information could reduce object size and some prologue
work, but it changes post-mortem behavior and is not proposed without platform
owner agreement. Selecting a concrete CPU may help, but no single compatible
board CPU contract is currently present in the configuration. These remain
report-only opportunities.

### F9. Runtime optimization levels currently collapse to one pipeline

`EJitOptimizer::runOptimizationPipeline` explicitly ignores its `level`
argument. L1/L2/L3 therefore differ in API/configuration only, not generated
IR. Release PASS1 also always uses an O2 inliner. This is not a current hot-path
regression when production always uses the same mode, but it is an API and
measurement caveat: changing `optLevel` cannot be used to trade compile time
for generated-code quality today.

### F10. Native AArch64 timing shows workload-dependent runtime benefit

The same probe was converted to little-endian AArch64 solely so it could run on
this server. Large and Small objects were linked into otherwise identical
non-LTO executables. Each round made 20 million out-of-line calls on a pinned
core and measured `cntvct_el0`; seven rounds were collected.

| Probe | Large median ticks/call | Small median ticks/call | Result |
|---|---:|---:|---:|
| local constant-table load | 0.0504 | 0.0504 | no measurable change |
| switch-table lookup | 0.0463 | 0.0442 | about -4.5% |

The counter frequency is lower than the CPU frequency, so fractional ticks are
expected and only the relative comparison is meaningful. The result does not
support claiming a universal speedup: the shorter address sequence can be
hidden behind a data load, while the switch case showed a repeatable gain. The
reliable benefits are smaller code and fewer relocations; board measurements on
real functions remain required for execution-time claims.

### F11. Remaining external-reference cost needs a placement or relaxation contract

Compared with AOT code whose final layout is known by the static linker, JIT
code pays for arbitrary process addresses:

- an external global base uses `adrp GOT; ldr pointer` before the value load;
- an external function call branches to a near JITLink stub, whose
  `adrp; ldr; br` sequence reads the real target from the GOT.

The overhead is normally amortized when several fields use one global base and
when a call target is warm, but it remains inside the generated JIT body. The
current pool allocator accepts only a size/alignment and supplies no address
hint, so the engine cannot promise that JIT text is close enough to application
text or data for direct references.

Two technically sound follow-ups exist:

1. extend the platform allocator with a near-placement contract for a known
   application text/data window; or
2. add a JITLink post-allocation relaxation that converts eligible GOT/stub
   references only after their final addresses are known and range-checked.

Both require substantially more platform/JITLink work. Clearing `dso_local`
selectively before allocation is rejected because it guesses the range too
early and the negative-control link demonstrated a real failure.

### F12. Indirect wrapper dispatch is not a 2000-cycle explanation by itself

The wrapper must ultimately call a runtime-selected pointer, normally with an
indirect branch. With a stable target, AArch64 branch prediction should make
that a small cost relative to a complex microsecond-scale function. Replacing
it with a direct branch would require call-site patching or a near executable
trampoline and would re-open permission, cross-core I-cache, and branch-range
problems. It is not a plausible standalone explanation for a persistent
roughly 2000-cycle inner-body delta; body IR/ASM and AOT/JIT path identity must
be checked first.

## Priority Assessment

1. **Per-specialization module pruning** likely has the largest compile-time
   and code-pool benefit when a blob contains many entries/helpers.
2. **Small model with code pool** is a narrow, low-complexity generated-code
   improvement for local tables and unwind records; external accesses do not
   regress.
3. **Pipeline additions** should be driven by before/after IR from a real slow
   function. Adding GVN/LICM/vectorization blindly increases worker CPU and
   library size.
4. **Near direct external references** are rejected for now. The current GOT
   load is the price of arbitrary process addresses; removing it would require
   a real code/data placement contract.

## Related Hot-Path Audit Cross-Check

The independent branch `codex/ejit-baseline-perf-audit` at commit
`c8501b3d9d2b` adds benchmarks and reports no production-code changes. Its
native AArch64 `-Os`, `NO_RECLAIM` measurements attribute approximately 216
instructions / 45 cycles to a 0D wrapper cache hit, of which about 137
instructions are in cache lookup and only about 9 instructions / 2 cycles are
the indirect JIT call. This supports the conclusion that an unexplained
multi-thousand-cycle application delta is not caused by `blr` alone.

The useful conclusions are:

- PR #82's unrolled seqlock fixed-dimension lookup addresses the dominant
  measured dispatch component.
- `NO_RECLAIM` avoids two reader-count atomic RMW operations and is materially
  cheaper than the reclaiming token path, but this relies on code never being
  physically reclaimed or overwritten while callable.
- Per-hit statistics add measurable overhead and must remain compiled out in
  the production configuration.
- Slot depth matters, but normal first-empty publication should keep common
  hits near slot 0/1.

Two qualifications must be retained when quoting its numbers:

1. The benchmark C ABI is a faithful copy of the production shell using a
   local runtime stub, not a direct call through the complete production
   `gEJIT` object graph. The 216-instruction result is therefore a strong
   representative estimate, not an exact board-binary count.
2. Its `trusted-wrapper` prototype is hit-only. On a miss it returns null and
   does not enqueue compilation. A production version must explicitly invoke
   the existing slow ABI on a true miss before taking the AOT fallback; the
   measured 24-instruction hit saving cannot be merged by copying the prototype
   unchanged.

There is also a stale source comment claiming an x86-64 host while the actual
measurement host and report are AArch64. This is documentation cleanup rather
than a measurement defect.

## Next Steps

1. Validate on board with a local-table/switch-heavy JIT function and inspect
   relocation/link failures before claiming a runtime-cycle improvement.
2. Start per-specialization module pruning as a separate change; do not mix it
   into the low-risk code-model patch.

## Validation Results

All build commands used at most eight jobs.

- Production combination: `LLVMEJIT` built and linked successfully as
  `build_release_aarch64_be/lib/libLLVMEJIT.a`.
- `EJITCodePoolTests`: 40/40 passed.
- `EJITSharedTaskPoolTests`: 55/69 passed. The 14 failures are the baseline's
  configuration-dependent peer-code-sharing and peer-4K tests when
  `EJIT_SRE_SHARED_CODE_POINTERS=OFF`; this change does not touch shared
  taskpool code or that gate.
- Full `EJITTests` compiled but could not link in this host code-pool build
  because the platform symbols `SRE_MemDbgAlloc`, `enable_ex`, and
  `split_2m_to_4k` are intentionally declared-only. No host weak fallback was
  added merely to make the test link.
- LLVM 21.1.8 big-endian Large/Small objects reproduced the recorded section
  sizes, and `llvm-jitlink` successfully linked the Small object with the code
  slab and all external symbols placed at deliberately distant addresses.
- `git diff --check`, C/C++ clang-format checks, and the source diff are clean.

## Reproduction Commands

Use LLVM 21 tools from a matching build:

```bash
LLC=/path/to/llvm-21/bin/llc
OBJDUMP=/path/to/llvm-21/bin/llvm-objdump
JITLINK=/path/to/llvm-21/bin/llvm-jitlink

for model in large small; do
  "$LLC" ejit_test/codegen_model_probe.ll -O2 -filetype=obj \
    -code-model="$model" -relocation-model=static \
    -o "/tmp/ejit-code-model-$model.o"
  "$OBJDUMP" -dr "/tmp/ejit-code-model-$model.o"
done

"$JITLINK" -noexec -entry=probe_external_global \
  -slab-allocate=2Mb -slab-address=0xF000000000 -slab-page-size=4096 \
  -abs external_value=0x100000000 \
  -abs external_helper=0x200000000 \
  -abs external_function_pointer=0x300000000 \
  /tmp/ejit-code-model-small.o
```

The native benchmark uses a little-endian copy of the probe IR, links
`ejit_test/codegen_model_probe_bench.c` against each object without LTO, and
runs each binary pinned to one CPU. It is intentionally manual because its
absolute timings are host-specific.

## Follow-up Work Package: Per-Specialization Pruning

Keep this separate from the Small-model commit. A production-quality change
should meet all of the following:

1. Preserve only the requested `origFnName` as an externally visible JITDylib
   symbol; internalize other definitions in the isolated specialization
   module.
2. Run GlobalDCE before the per-function specialization passes so unrelated
   functions do not consume optimizer time, then run it again after branch
   folding so helpers made unreachable by specialization are removed.
3. Preserve IR-visible indirect closure through function-pointer globals,
   aliases, global initializers, `llvm.used`, and `llvm.compiler.used`.
4. Cleanly skip or conservatively preserve modules containing function/module
   inline assembly whose textual symbol references cannot be proven.
5. Add tests for direct helpers, recursive SCCs, address-taken helpers,
   initializer tables, aliases, dead secondary entries, external declarations,
   and inline-assembly fallback.
6. Measure parser/optimizer/CodeGen/JITLink time separately and compare emitted
   text/rodata/unwind/GOT/stub sizes on a multi-entry realistic blob.
7. Measure final lipo archive growth from pulling Internalize/GlobalDCE code;
   reject the change if runtime binary growth outweighs the recurring compile
   and pool savings for the target workload.
8. Keep external symbol registration and lookup behavior unchanged, and verify
   A-calls-B where B is not inlined remains functional.

As an early size warning, the current AArch64 Release `LLVMipo` archive members
are about 81KiB for `Internalize.cpp.o` and 79KiB for `GlobalDCE.cpp.o` before
transitive symbols and final section GC. This is not the final lipo delta, but
it confirms that pruning is not a zero-size runtime change and must be measured.

An alternative is AOT-time per-entry extraction: PASS1 already computes the
transitive closure, so it could serialize one blob per entry and leave no
runtime pruning code. This avoids adding IPO code to the embedded runtime, but
duplicates shared helper IR/constants across blobs and increases the AOT image.
The right choice depends on how often one translation unit contains multiple
entries. Measure both the number of distinct blob pointers in the registry and
entries per blob before choosing between runtime pruning and per-entry blobs.
