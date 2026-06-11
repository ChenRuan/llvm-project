# EmbeddedJIT SPEC Implementation Audit

**Audit date**: 2026-06-11
**Branch audited**: `ejit_dev_spec4_rc` (== `dong/ejit_dev_spec4`, HEAD `1f5af8c61df9`)
**Reference SPEC**: [SPEC4.md](SPEC4.md) (v1.1)
**Reference plan**: [PLAN4.md](PLAN4.md) (v2.1)

## 0. Purpose and method

This document maps each SPEC4 requirement to its current implementation, assigns
a status, records test coverage, and flags risks. It intentionally **does not
change any core logic** — it is a read-only audit to establish a shared baseline
before incremental work.

**Status legend**

| Status | Meaning |
|--------|---------|
| `done` | Implemented and exercised by at least one test. |
| `partial` | Implemented but incomplete, or correct only for a subset of cases. |
| `skeleton` | Code/types exist but the feature is not wired into the live path. |
| `unsupported` | Explicitly out of scope per SPEC ("future extension"). |
| `unclear` | Behavior present but not verifiable locally / under-specified. |

**Local verification constraints** — the configured build is
`build-ejit/` (Release, `LLVM_TARGETS_TO_BUILD=X86`, assertions ON, static libs,
`clang` enabled). Therefore:

- AOT passes (`opt`) and Clang Sema/CodeGen tests are locally verifiable via
  `opt` + `%clang_cc1` + `FileCheck`.
- **JIT machine-code execution** (actually running a specialized function and
  observing native behaviour) is **native-target dependent**. The X86 host can
  run it, but AArch64 (the real SPEC §6 target, Cortex-A) cannot be executed
  here — only cross-checked at the IR level. Any row that depends on running
  specialized code on the embedded target is flagged.
- There is **no `check-ejit-runtime` / `check-ejit-lit` / `check-ejit-all`
  aggregate target** in this tree. The only custom target is `check-ejit-size`
  (in [llvm/lib/ExecutionEngine/EJIT/CMakeLists.txt](../llvm/lib/ExecutionEngine/EJIT/CMakeLists.txt)).
  Runtime behaviour is covered by the `EJITTests` gtest suite
  ([llvm/unittests/ExecutionEngine/EJIT/EJitRuntimeTest.cpp](../llvm/unittests/ExecutionEngine/EJIT/EJitRuntimeTest.cpp)).

---

## 1. Clang attributes (SPEC §2)

Attributes are defined in [clang/include/clang/Basic/Attr.td](../clang/include/clang/Basic/Attr.td)
(`EjitMayConst`, `EjitPeriod`, `EjitPeriodArr`, `EjitPeriodArrInd`, `EjitEntry`,
`EjitPeriodLc`) and handled in [clang/lib/Sema/SemaEJIT.cpp](../clang/lib/Sema/SemaEJIT.cpp).

| SPEC item | Location | Status | Tests | Risk / Notes |
|-----------|----------|--------|-------|--------------|
| `ejit_may_const` on field; type = int/bool/float/struct/array; volatile excluded (§2.1) | `handleEjitMayConstAttr` | `done` | `clang/test/Sema/ext_attr_ejit.cpp`, `clang/test/CodeGen/ejit_volatile_field.c` | volatile is silently skipped at CodeGen (no per-load metadata), not diagnosed — matches SPEC but untested for the *field-of-array* and *nested* cases. |
| `ejit_period("static")` scalar global; not array; no conflict (§2.2.1) | `handleEjitPeriodAttr` | `done` | `ext_attr_ejit.cpp` | Custom (non-`static`) scalar period is `unsupported` by SPEC; Sema still *accepts* any name string (no runtime consumer for non-static scalar). Low risk. |
| `ejit_period_arr(name)` array (const size ≤100) or pointer-to-struct (§2.2.2) | `handleEjitPeriodArrAttr` | `done` | `ext_attr_ejit.cpp` | Size>100 → error; VLA → error; pointer-to-non-struct → error. The "≤1024 arrays globally" limit is **not enforced** in Sema. |
| `ejit_entry`; non-recursive (§2.3.1) | `handleEjitEntryAttr` + `RecursiveCallVisitor` | `partial` | `ext_attr_ejit.cpp` | Only **direct** self-recursion is detected. **Mutual recursion** (`f→g→f`) is not detected. Idempotency requirement (§2.3.1) is documentation-only. |
| `ejit_period_arr_ind(name)` param; integer; ≤4 per function (§2.3.2) | `handleEjitPeriodArrIndAttr` + `checkEjitPeriodArrIndLimit` | `done` | `ext_attr_ejit.cpp` | Limit enforced in `ActOnFunctionDeclarator`. |
| `ejit_period_lc(name)`; must have matching `ejit_period_arr_ind` (§2.3.3) | `handleEjitPeriodLcAttr` | `done` | `ext_attr_ejit.cpp` | `err_ejit_period_lc_no_index` diagnostic. |
| §9.1 ownership conflict (error) | `err_ejit_period_conflict` in period/period_arr handlers | `done` | `ext_attr_ejit.cpp` | |
| §9.2 modify may_const without `ejit_period_lc` (warning) | `warn_ejit_may_const_modified_without_lc` in `DiagnosticSemaKinds.td` | **`skeleton`** | none | The diagnostic **is defined** but is **never emitted** — no `clang/lib` code references it. So the warning is wired only as a string, not as behavior. |
| §9.4 metadata consistency (warning) | `runDiagnosticCheck` in `EJitAotModulePass` | `partial` | `ejit-aot-module-diagnostic.ll` | Implemented at **AOT pass** level (warns when an entry references a `period_arr` not declared via `period_arr_ind`), not in Sema. |

---

## 2. CodeGen metadata (SPEC §3.5, §4.1)

| Item | Location | Status | Tests | Risk / Notes |
|------|----------|--------|-------|--------------|
| `!ejit.metadata` on functions (`ejit_entry`, `ejit_period_lc`, `ejit_period_arr_ind`) | `emitEjitFunctionMetadata` in [clang/lib/CodeGen/CGEJIT.cpp](../clang/lib/CodeGen/CGEJIT.cpp) | `done` | `clang/test/CodeGen/ejit_metadata.c`, `ejit_pipeline.c` | Distinct MDNode; arg index recorded for `period_arr_ind`. |
| `!ejit.metadata` on globals (`ejit_period`, `ejit_period_arr` + size) | `emitEjitGlobalMetadata` | `done` | `ejit_metadata.c`, `ejit_multi_period_arr.c` | Array size recorded; pointer-type arrays record size 0. |
| `ejit_may_const_field` byte-offset list on the GV (fallback) | `collectMayConstFieldOffsets` (recurses nested structs) | `done` | `ejit_nested_struct.c` | Recurses into nested records; skips bitfields. |
| Per-load `!ejit.may_const` metadata | `EmitLoadOfScalar` / `EmitLValueForField` in [clang/lib/CodeGen/CGExpr.cpp](../clang/lib/CodeGen/CGExpr.cpp) | `done` | `ejit_metadata.c`, `ejit_volatile_field.c` | Propagated through `LValueBaseInfo::isEjitMayConst()`; volatile fields excluded. See §6 for robustness gaps. |

---

## 3. AOT passes PASS1–PASS5 (SPEC §4.1, PLAN §3.6)

Source: [llvm/lib/Transforms/EmbeddedJIT/](../llvm/lib/Transforms/EmbeddedJIT/).

| PASS | Location | Status | Tests | Risk / Notes |
|------|----------|--------|-------|--------------|
| PASS1 `EJitRegisterBitcode` — closure extraction, AOT pre-opt, bitcode embed, symbol auto-register | `EJitRegisterBitcode.cpp` | `partial` | `ejit-register-bitcode*.ll` (4 files) | **`preOptimizeBitcode()` runs only under `NDEBUG`** (release). In debug builds it is a no-op (cyclic link dep `LLVMPasses ↔ LLVMEmbeddedJIT`). Consequence: debug-built bitcode is not pre-inlined, and the JIT inline step is also disabled (see PASS6/optimizer), so debug specialization may miss cross-helper `may_const` folding. `reAnnotateMayConst()` restores dropped per-load metadata from the GV offset list. |
| PASS2 `EJitRegisterPeriod` — emit `ejit_register_period_array` / `ejit_register_static_var` | `EJitRegisterPeriod.cpp` | `done` | `ejit-register-period*.ll` (3 files) | Layout registration removed (per PLAN note). |
| PASS3 `EJitWrapperGen` — single-function mixed wrapper | `EJitWrapperGen.cpp` | `done` | `ejit-wrapper-gen-*.ll` (4 files) | Builds `jit_entry/jit_fallback/jit_dispatch`; `cacheKey = funcIdx<<32 | dims`; each dim trunc'd to i8; adds `noinline` (`-ejit-noinline-entry`, default on); idempotency guard. Declares `ejit_compile_or_get(i64, ptr)`. |
| PASS4 `EJitPeriodHandler` — insert deactivate@entry / activate@returns | `EJitPeriodHandler.cpp` | `partial` | `ejit-period-handler*.ll` (4 files) | Warns when `ejit_period_lc` lacks a matching `ejit_period_arr_ind`. **When multiple arrays share one period name, `LC.ArrayGV` keeps only the last match** — array-pointer passed to `(de)activate_array` may be wrong for the multi-array-same-name case. Needs a test. |
| PASS5 `EJitAotModulePass` — coordinate PASS2→3→4 + diagnostic | `EJitAotModulePass.cpp` | `done` | `ejit-aot-module*.ll` (4 files) | PASS1 is a separate early pass; skips modules with no EJIT metadata. |

> **Doc discrepancy**: PLAN4 §3.6 states `!ejit.may_const` is guaranteed by
> "固定 metadata kind + copyMetadataForLoad 白名单 + GV offset 回退" (a
> *copyMetadataForLoad whitelist*). **No such core-LLVM modification exists** in
> `llvm/lib/IR` or `llvm/lib/Transforms/Utils` on this branch. The real
> preservation mechanism is: (1) per-load metadata from Clang, (2) PASS1
> `reAnnotateMayConst` after pre-opt, (3) PASS6 GV-offset fallback at JIT time.
> The doc should be corrected (tracked for the may_const PR).

---

## 4. JIT pipeline & runtime PASS6/PASS7 (SPEC §4.2, PLAN §3.6)

Source: [llvm/lib/ExecutionEngine/EJIT/](../llvm/lib/ExecutionEngine/EJIT/).

| Item | Location | Status | Tests | Risk / Notes |
|------|----------|--------|-------|--------------|
| `ejit_compile_or_get(uint64 cacheKey, void** out_pfn)` C ABI | `EJitRuntime.cpp` | `done` | `EJitRuntimeTest.cpp` | Matches SPEC §3.5 v2.0 signature. `out_pfn` reserved (currently mirrors return). |
| Hot path: cache hit → single hash → return | `EJitCompileDriver::getOrCompile` → `EJitCache::getOrNull` | `done` | `EJitRuntimeTest.cpp` | One `unique_lock`, one hash, O(1) LRU bump via embedded iterator. Cache hit does **not** re-enter parse/opt/codegen. |
| Cold path: decode → funcName → bitcode → meta → active check → load → lookup → put | `EJitCompileDriver::getOrCompile` | `partial` | `EJitRuntimeTest.cpp` | Early `nullptr` on missing funcName / missing bitcode / inactive window. **First miss per funcIdx parses bitcode twice** — once in `getOrCacheFuncMeta` (period-name extraction) and once in `loadBitcodeModule` (compile). Meta is cached afterwards. |
| PASS6 `EJitStructFieldPass` — `may_const` load → constant | `EJitStructFieldPass.cpp` | `done` (X86); `unclear` (AArch64 exec) | `EJitRuntimeTest.cpp` | `isMayConstLoad`: per-load metadata first, GV-offset fallback second. 3 patterns: direct GV / GEP chain / indirect pointer. Constant materialized from process memory (endian-aware for ints). Correct native execution only verifiable on X86 here. |
| PASS7 OrcJIT/JITLink engine — load module, run transform, lookup | `EJitOrcEngine.cpp` | `done` (X86); `unclear` (AArch64) | `EJitRuntimeTest.cpp` | LLJIT + per-specialization JITDylib; `Large` code model; IR transform layer runs the specialization pipeline. AArch64 ELF path clears `dso_local` on decls to force GOT/PLT — **not executable locally**. |
| JIT pipeline order: param-subst → InstCombine → Inline → StructField → L1/L2/L3 | `EJitOptimizer::runPipeline` | `partial` | `EJitRuntimeTest.cpp` | **JIT Inline step is disabled** (relies on PASS1 AOT pre-opt having inlined). Combined with PASS1 pre-opt being release-only, debug builds have *neither* AOT inline *nor* JIT inline. L1 always; L2 adds SimplifyCFG; L3 unroll + re-run StructField/InstCombine. Pipelines pre-built/cached in ctor. |
| Optimization levels L1/L2/L3 (SPEC §4.4) | `EJitOptimizer::runOptimizationPipeline` | `partial` | none specific | L1=SCCP+ADCE+SimplifyCFG, L2=+SimplifyCFG cleanup, L3=+LoopFullUnroll. The "function inline = L2/L3" row of SPEC §4.4 table is **not** realized as a JIT-time inline (it is an AOT-time inline). |

---

## 5. Runtime registry, cache key, period array (SPEC §3.3–3.5, §2.2)

| Item | Location | Status | Tests | Risk / Notes |
|------|----------|--------|-------|--------------|
| `PeriodArrayRegistry` (period→arrays, byName, byBaseAddr, staticVars) | `EJitRuntimeState.cpp` | `done` | `EJitRuntimeTest.cpp` | Three indices kept in sync at registration. |
| `ejit_activate/deactivate(name, idx)` — fan-out to all arrays of name | `EJitRuntimeState::activate/deactivate` | `done` | `EJitRuntimeTest.cpp` | |
| `ejit_activate_array/deactivate_array(name, ptr, idx)` — single array | `EJitRuntime.cpp` + `EJitRuntimeState` | `done` | `EJitRuntimeTest.cpp` | Validates `ptr` is registered & name matches, else `EJIT_ERR_INVALID_PARAM`. |
| `ejit_activate_all/deactivate_all(name)` | `EJitRuntimeState` | `done` | `EJitRuntimeTest.cpp` | |
| `ejit_is_active`; `static` always active (§2.1) | `EJitRuntimeState::isActive` | `done` | `EJitRuntimeTest.cpp` | Period-level: returns true if **any** array under the name is active at idx. |
| Out-of-bounds idx silently skipped (§3.3) | `uint8_t` typing + per-array map | `done` | `EJitRuntimeTest.cpp` | `cellIdx` is `uint8_t`; smaller arrays simply have no entry at that idx. |
| Cache key = `funcIdx(32b) | d[3..0](8b each)` (§2.3.2) | `EJitCache::buildCacheKey`; wrapper in PASS3 | `done` | `EJitRuntimeTest.cpp` | funcIdx = FNV-1a 32-bit of name (`hashFuncName`), collisions fatal at registration. |
| Code cache: iterator-embedded LRU + size/entry limits + period invalidation | `EJitCache.cpp` | `done` | `EJitRuntimeTest.cpp` | `hits_/misses_/evictions_` counters exist (exposed via `ejit_get_stats`). `periodIndex_` maps `name=idx` → keys for O(1) invalidation. |
| `ejit_deactivate*` triggers cache invalidation (§4.2) | `EJitRuntime.cpp` calls `invalidateByPeriod` | `done` | `EJitRuntimeTest.cpp` | |
| Symbol auto-registration (§3.4) | PASS1 `generateSymbolRegisters` + `EJitRegistrationStore` + static tables | `done` | `EJitRuntimeTest.cpp` (static-table path) | Two ingestion paths: constructor staging store, and weak static registry tables (`__ejit_registry_*`). |

---

## 6. `may_const` end-to-end (cross-cutting — focus of a dedicated PR)

| Stage | Location | Status | Notes |
|-------|----------|--------|-------|
| Field flag propagation | `CGExpr.cpp EmitLValueForField` | `done` | Sets `LValueBaseInfo` may_const bit for non-volatile fields. |
| Per-load metadata emission | `CGExpr.cpp EmitLoadOfScalar` | `partial` | Relies on `BaseInfo.isEjitMayConst()`. **Robustness across helper-function pointer params, repeated loads of one field, and InstCombine rewrites is untested.** |
| AOT survival | PASS1 `reAnnotateMayConst` | `partial` | Restores per-load metadata from GV offset list after pre-opt; but only matches loads whose pointer traces to the root GV via constant GEPs. |
| JIT consumption | PASS6 `isMayConstLoad` | `done` | Dual mechanism (metadata + GV offset). |
| Missing metadata → clean fallback | PASS6 returns no replacement; AOT fallback runs | `done` | Safe-by-design (drop = miss one optimization, not incorrect). |
| Wrong/spurious metadata → no mis-replacement | PASS6 still requires GV resolution + offset match | `partial` | A load tagged `may_const` whose pointer does **not** resolve to a registered GV is simply not replaced — but this is **not covered by a negative test**. |

> This row set is the rationale for the dedicated *may_const* PR: behavior is
> believed correct but **under-tested**, and the design has a documented-but-absent
> mechanism (whitelist).

---

## 7. Async / object cache / signature validation (SPEC §4.3, §3.5)

| Item | Location | Status | Notes |
|------|----------|--------|-------|
| Async compile mode (§4.3) | `EJitAsyncCompiler.cpp` exists (worker thread, queue, dedup, in-flight set) | **`skeleton`** | **Not wired.** `EJitAsyncCompiler` is fully written but **never instantiated**. `EJit::getOrCompile` → `EJitCompileDriver::getOrCompile` is **always synchronous**; `config_.compileMode` is parsed and stored (`set/getCompileMode`) but never branched on in the dispatch path. SPEC §4.3 async semantics (first call returns NULL → fallback, background compile, next call hits cache) are **not realized at runtime**. |
| Sync compile mode | `EJitCompileDriver::getOrCompile` | `done` (X86) | The only live path. |
| LLVM `ObjectCache` (serialized object reuse) | — | **not present** | There is **no `llvm::ObjectCache`**. The "code cache" (`EJitCache`) stores **function pointers** keyed by cacheKey, not serialized object files. Bitcode is re-parsed/re-compiled on every cache miss. This is consistent with SPEC (which only requires a Code Cache with LRU), but the term "object cache" in review notes should be read as the **function-pointer code cache**, not an object-file cache. |
| Signature validation / "unsupported signature" early reject | — | **not present** | The runtime does **not** validate the entry signature. The wrapper ABI is fixed at AOT time by PASS3, so there is no runtime "unsupported signature" rejection path. Any early-reject test must target missing funcName / missing bitcode / inactive window instead. |
| Memory limits / LRU eviction (§7) | `EJitCache` (maxEntries/maxTotalSize/maxSingleFuncSize) | `done` | Defaults raised to 4096 entries / 2MB total (per recent commits). |
| Embedded slab/memory manager (PLAN §custom memory manager) | OrcEngine uses default LLJIT allocator | `unclear` | PLAN mentions a 512KB slab allocator; the current engine uses LLJIT's default object linking layer memory manager. The bespoke slab manager is **not obviously present** — needs confirmation before claiming SPEC §6 memory-budget compliance. |

---

## 8. Target platform & non-functional (SPEC §6, §10)

| Item | Status | Notes |
|------|--------|-------|
| ARM Cortex-A / AArch64 target | `unclear` (locally) | Engine has an explicit AArch64-ELF code path, but this build targets X86 only (`LLVM_TARGETS_TO_BUILD=X86`) and cannot execute AArch64 output. Cross-IR checks only. |
| First-compile latency < 100ms (§10) | `unclear` | No latency benchmark in-tree that is run locally. |
| Specialized-code perf > AOT (§10) | `unclear` | Not measured here; depends on running on target. |
| Reliability: all JIT failures fallback, never crash (§8) | `partial` | All located error paths return `nullptr` → wrapper falls back to AOT body. Async error semantics N/A (skeleton). Not all failure injections are tested. |

---

## 9. Summary of findings (prioritized)

1. **Async mode is a skeleton** (§7). High-visibility SPEC §4.3 feature with full
   class implementation but no wiring. Either wire it (larger change) or document
   clearly as not-yet-active. *Do not* silently leave `set_compile_mode(ASYNC)`
   looking effective.
2. **`may_const` is under-tested** (§6) and depends on a documented-but-absent
   "whitelist" mechanism. Add negative/robustness lit tests; correct PLAN4 wording.
3. **Double bitcode parse on first miss** (§4). Low-risk optimization: reuse the
   meta-extraction parse, or extract period names without a full parse.
4. **Debug builds skip both AOT pre-opt and JIT inline** (§3, §4). Specialization
   across helper functions may silently degrade in debug builds. Document and/or
   gate tests on release.
5. **PASS4 multi-array-same-name** keeps only the last `ArrayGV` (§3). Potential
   wrong array pointer for `(de)activate_array`. Needs a test, then a minimal fix.
6. **No `check-ejit-*` aggregate targets**; runtime coverage is a single gtest file.
   Test-infra PR should add convenience targets and broaden coverage.
7. **CLAUDE.md is stale** — claims the runtime "not yet created on this branch";
   it exists and is substantial.
8. **SPEC §9.2 write-without-lifecycle warning is a skeleton** — the diagnostic
   `warn_ejit_may_const_modified_without_lc` is defined in `DiagnosticSemaKinds.td`
   but never emitted by any `clang/lib` code. Either implement the emission or
   mark the diagnostic as not-yet-active.

## 10. Cross-references to follow-up PRs

| Finding | Follow-up PR |
|---------|--------------|
| #2 may_const tests + whitelist doc fix | `codex/ejit-may-const-audit` |
| #3 double-parse, fast-path counters | `codex/ejit-runtime-fastpath` |
| #1, #4 JIT overhead, early-return coverage, async skeleton doc | `codex/ejit-jit-overhead` |
| #5, #6, #8 broader Sema/CodeGen/Transforms/runtime tests | `codex/ejit-test-coverage` |

*This audit changes no source code. All rows reflect the state at HEAD
`1f5af8c61df9`.*
