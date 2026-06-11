# EmbeddedJIT JIT Overhead Notes

**Date**: 2026-06-11
**Branch**: `codex/ejit-jit-overhead` (off the EJITTests build repair)
**Scope**: review the runtime JIT path from `ejit_compile_or_get` to the
returned function pointer, document where overhead is (and is not), and add
host-independent coverage for the cheap early-exit and parse-once invariants.

This complements [EJIT_SPEC_AUDIT.md](EJIT_SPEC_AUDIT.md) and the fast-path
counter work in the runtime-fastpath change. It changes **no default behavior**.

## 1. Path overview (`EJitCompileDriver::getOrCompile`)

```
ejit_compile_or_get(cacheKey)
  └─ EJit::getOrCompile
       └─ EJitCompileDriver::getOrCompile
            1. cache_.getOrNull(cacheKey)        ── fast path: hit → return
            2. decode cacheKey → funcIdx + dims
            3. loader_.getFuncNameByFuncIdx       ── empty → return (missingFunc)
            4. loader_.getBitcodeByFuncIdx        ── error → return (missingBitcode)
            5. loader_.getOrCacheFuncMeta(funcIdx)── parse bitcode ONCE per funcIdx
            6. runtimeState_.isActive(dim)        ── inactive → return (windowInactive)
            7. syncEngine_ check                  ── missing → return (engineMissing)
            8. loadBitcodeModule (parse) → IR transform pipeline → lookup → cache.put
```

Stages 1, 3, 4, 6, 7 are cheap guards; the expensive work (bitcode parse, the
specialization/optimization pipeline, and machine-code emission) is only stage 8.

## 2. Findings

| Question (SPEC audit / review) | Finding |
|--------------------------------|---------|
| Does a cache **hit** bypass parse/opt/codegen? | **Yes.** `getOrNull` returns before any decode/parse. Proven by the `EJitCompileDriverCounters` tests: on a hit, `compileAttempts == 0`. |
| Is bitcode **re-parsed** unnecessarily? | Metadata extraction (`getOrCacheFuncMeta`) parses **once per funcIdx** and caches the result; subsequent misses for the same funcIdx reuse it. Proven by `EJitModuleLoader.FuncMetaParsedOnceAndCached` (`getParseCount()` stays 1). **However**, on the *first* miss for a funcIdx the bitcode is parsed **twice** — once for metadata (`getOrCacheFuncMeta`) and once for compilation (`EJitOrcEngine::loadBitcodeModule`), using independent `LLVMContext`s. This is a one-time first-miss cost, not on the hot path. Sharing the parse would require threading a parsed module (and its context lifetime) from the driver into the engine; deferred as a larger change. |
| Is an **unsupported signature** rejected early? | There is **no runtime signature validation**. The wrapper ABI is fixed at AOT time by `EJitWrapperGen` (PASS3), so the runtime never sees a mismatched signature to reject. The early-reject reasons that *do* exist are missing funcName, missing bitcode, and inactive window. |
| Is **missing funcIdx / missing bitcode** rejected before the expensive path? | Yes — stages 3/4 return before stage 8. Proven by `EJitCompileDriverCounters.UnknownFuncIdxEarlyReturn` (`compileAttempts == 0`). Note the `missingBitcode` branch is effectively unreachable with the current loader (name + data are registered together in one map); it is retained defensively. |
| Is the **object cache** a skeleton? | There is **no `llvm::ObjectCache`** (serialized object-file reuse). The "code cache" (`EJitCache`) stores **specialized function pointers** keyed by `cacheKey`, with LRU eviction and period-based invalidation. This is consistent with SPEC §3.2 (a Code Cache with LRU); it is **not a skeleton**, but it is **not** an object-file cache either — a cache miss recompiles from embedded bitcode. Naming in review notes should say "function-pointer code cache". |

## 3. Native-target limitation (local validation)

The configured build targets **X86 only** (`LLVM_TARGETS_TO_BUILD=X86`) while the
host is **AArch64**. `JITTargetMachineBuilder::detectHost()` therefore selects a
target with no built backend, so stage 8 (actual JIT compile + lookup) **cannot
run on this host**. The tests added here deliberately exercise only the
**host-independent control-flow accounting** (cache hit avoids compile,
parse-once caching, early-return attribution). End-to-end compilation/execution
must be validated on a host whose architecture matches a built backend (e.g. an
X86 host, or an AArch64 build of the runtime per SPEC §6).

## 4. Coverage added

- `EJitModuleLoader::getParseCount()` + `EJitModuleLoader.FuncMetaParsedOnceAndCached`
  — proves metadata bitcode parse happens at most once per funcIdx.
- (in the runtime-fastpath change) `EJitCompileDriverCounters.*` — proves cache
  hits never enter the compile path and early-returns are correctly attributed.

## 5. Remaining / deferred

- First-miss double parse (metadata + compile): low-risk to leave; a future
  change could pass the already-parsed module into the engine.
- Async mode is still a skeleton (see SPEC audit §7); JIT overhead under async
  is not measurable until it is wired.
- First-compile latency (SPEC §10, < 100 ms) is not benchmarked here because the
  compile stage is not runnable on this host.
