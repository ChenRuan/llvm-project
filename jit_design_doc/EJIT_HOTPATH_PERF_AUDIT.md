# EmbeddedJIT Post-Compile Hot-Path Performance Audit

Branch: `codex/ejit-baseline-perf-audit`
Baseline: `dong/ejit_dev_spec4` @ `68ed1d88d8be`
Measurement host: **native aarch64** (`aarch64-unknown-linux-gnu`, clang 23),
production optimization level `-Os`, `perf` PMU available.

> Scope: this audit studies **only** the EmbeddedJIT hot path taken *after* a
> function is already JIT-compiled — the "wrapper cache hit" the business sees
> as *AOT ~9 µs vs JIT wrapper ~15 µs*. It deliberately does **not** touch the
> AsmLexer, the `-O2` crash, the 4K/2M permission model, IR/ASM dump, the
> AArch64 AsmParser, backend trimming, the SRE platform API, `may_const`
> correctness, online PGO, the light backend, or the global log framework.

---

## 0. TL;DR

* The complete **wrapper cache-hit path is ~216 aarch64 instructions / ~45
  cycles** in the production (`NO_RECLAIM` + shared-code-pointers) build — it is
  **not** under 100 instructions. Section 5 breaks down where every instruction
  goes.
* The single biggest lever, the cache-query cost, is **already addressed by
  PR #82** (unrolled fixed-dimension seqlock lookups). This audit confirms that
  finding independently and does **not** re-implement it.
* The remaining, PR #82-independent overhead is **plumbing**: the C-ABI shell
  (~47 ins) and the wrapper glue (~23 ins). A **trusted-wrapper fast C-ABI**
  removes ~24 instructions / ~5 cycles (~11 % of the wrapper-hit path),
  measured in both the token and the seqlock builds. It is presented as a
  **measured prototype**, not merged, because the change spans the C ABI and
  `EJitWrapperGen` which cannot be end-to-end correctness-tested in this
  environment (no board, no full clang build).
* Two design decisions are quantitatively **validated**: keeping statistics
  opt-in (`EJIT_STATS_ENABLE` off) saves ~13 cycles/hit, and the `NO_RECLAIM`
  seqlock read path costs ~2.2× fewer cycles than the token path even on a
  single core (and avoids cross-core cache-line bouncing).

---

## 1. Deliverables

| Item | File |
|---|---|
| Three-tier benchmark (its own `main`) | `llvm/unittests/ExecutionEngine/EJIT/EJitHotPathBench.cpp` |
| Faithful same-ABI C-shell + wrapper TU | `llvm/unittests/ExecutionEngine/EJIT/EJitHotPathBenchAbi.{h,cpp}` |
| CMake target `EJITHotPathBench` (+ `EJIT_TEST_STATS` opt-in) | `llvm/unittests/ExecutionEngine/EJIT/CMakeLists.txt` |
| This report | `jit_design_doc/EJIT_HOTPATH_PERF_AUDIT.md` |

Build (standalone, no full LLVM build required):

```
# production premise: seqlock + cross-core sharing
cmake -B build -DEJIT_TEST_NO_RECLAIM=ON …
ninja -C build EJITHotPathBench
# or directly, at -Os:
clang++ -std=c++17 -Os -I llvm/include -Illvm/unittests/ExecutionEngine/EJIT \
  -DEJIT_SRE_TASKPOOL -DEJIT_SRE_SHARED_TASKPOOL -DEJIT_SRE_TASKPOOL_TESTING \
  -DEJIT_SRE_TASKPOOL_BUCKETS=32 -DEJIT_SRE_TASKPOOL_QUEUE_CAPACITY=1024 \
  -DEJIT_SRE_TASKPOOL_WORKER_STACK_SIZE=1048576 \
  -DEJIT_SRE_TASKPOOL_NO_RECLAIM -DEJIT_SRE_SHARED_CODE_POINTERS \
  llvm/unittests/ExecutionEngine/EJIT/EJitHotPathBench.cpp \
  llvm/unittests/ExecutionEngine/EJIT/EJitHotPathBenchAbi.cpp \
  llvm/lib/ExecutionEngine/EJIT/EJitSharedTaskPool.cpp \
  llvm/lib/ExecutionEngine/EJIT/EJitSharedPlatform.cpp -lpthread -o hotbench
```

Run: `./hotbench [iters]` for the full distribution table, or
`perf stat -e instructions,cycles ./hotbench single <workload> <iters>` for
per-workload instruction/cycle attribution (`single empty` is the calibration
floor to subtract).

---

## 2. The real wrapper cache-hit call chain

Verified against the source (file:line):

```
business call
  └─ AOT wrapper (EJitWrapperGen.cpp: jit_entry)
       ├─ load __ejit_funcidx_<name> global
       ├─ jit_call: load dimType globals, extract instanceId from args
       │    └─ ejit_taskpool_compile_or_get_0d/1d/2d          (EJitRuntime.cpp:586/614/646)
       │         ├─ null-init *outFn,*outBucket
       │         ├─ gEJIT ? gEJIT->sharedTaskPool() : nullptr  (two dependent loads)
       │         ├─ dim-range check (fixed-dim entries)
       │         └─ tp->tryCacheHit0D/1D/2D                    (EJitSharedTaskPool.cpp:1415/1430/1453)
       │              ├─ state_ && initState==Ready            (acquire load)
       │              ├─ instance-enabled check (1D/2D)
       │              ├─ cacheLookup0D / cacheLookupSeq        (:604 / :363)   ← dominant cost
       │              │    ├─ key = hash(funcIndex,dims)
       │              │    ├─ bucket = key % 32
       │              │    ├─ bucketTryRead / bucketSeqBegin   (:57 / :74)
       │              │    ├─ generation.loadAcquire
       │              │    ├─ slot scan (≤16): state, generation, identityHash,
       │              │    │    funcIndex/numDims, dims, versions
       │              │    └─ resolveMatchedSlot               (:417)
       │              │         ├─ self = EJitCoreId::current()  (external bl on real platform)
       │              │         ├─ owner = ownerCoreId.loadRelaxed
       │              │         ├─ fn = Slot.fnPtr.loadAcquire
       │              │         ├─ owner  → return {fn, noTokenHit|token}
       │              │         └─ peer   → executableCoreMask bit → return / peerPrepareSlot
       │              └─ classifyHit                           (:1341, always_inline)
       │         └─ write *outFn,*outBucket, taskpoolStatus()
       ├─ jit_dispatch: status==OK && fn ? indirect-call fn(args)
       │    └─ ejit_taskpool_release_read(bucket)              (releaseRead: token=RMW, NO_RECLAIM=no-op)
       └─ jit_fallback: original AOT body
```

---

## 3. Method

* Native aarch64, `-Os`, pinned with `taskset -c 3`.
* **ns distribution**: warm up, then time `B` batches of `K=4000` iterations
  each (`B·K ≥ 4,000,000`), report mean / min / p50 / p95 / p99 of the per-batch
  ns/iter — a stable distribution without per-call timer cost in the loop.
* **instructions / cycles**: `perf stat` around a tight single-workload loop
  (`single` mode), 50,000,000 iterations, minus the `empty` loop (0.10 ins /
  0.11 cyc — effectively the floor).
* A `volatile` sink consumes every looked-up pointer and every return value so
  the optimizer cannot elide the call; disassembly (§5) confirms the path is
  actually executed. No `volatile` global is *written* inside the timed region,
  so the measured cost is the hot path, not synthetic store traffic.
* The three tiers are kept strictly separate. The C-ABI tier calls a **faithful
  reproduction** of the production `ejit_taskpool_compile_or_get*` shell (same
  validation, same `tryCacheHit` dispatch, same status mapping, same double
  out-param write-back); the only difference is the pool is resolved from a stub
  instead of the full `gEJIT` object graph, which is irrelevant to the hot path.
  The wrapper tier is a real cross-TU function doing a real indirect call. This
  is the "same-ABI host wrapper" the brief permits when the true target binary
  cannot run here. **No internal `tryCacheHit` number is ever presented as a
  C-ABI or wrapper number.**

Limitation, stated honestly: the host is aarch64 (so the architecture, `-Os`,
instruction counts and cycle counts are directly representative), but it is not
the customer board, the indirect call is a plain `blr` to a small local
function (no BTI/PAC, perfectly predicted), and `EJitCoreId::current()` is a
thread-local rather than the platform `mrs`. The board-specific `blr`/`current()`
costs are analyzed in §7 and flagged for on-board confirmation.

---

## 4. Measured results (production `NO_RECLAIM` + shared build, `-Os`)

### 4.1 Per-op instructions & cycles (perf, 50 M iters, net of empty loop)

| tier | workload | ins/op | cyc/op |
|---|---|--:|--:|
| baseline | empty loop | 0.10 | 0.11 |
| baseline | direct AOT body | ~2 | ~2 |
| baseline | direct JIT fnPtr (indirect call) | 9.1 | 2.4 |
| internal | tryCacheHit0D + release | 146.2 | 30.6 |
| cabi | `bench_cabi_0d` (full shell) | 193.3 | 40.1 |
| cabi | `bench_cabi_1d` | ~ | ~ |
| cabi | `bench_cabi_generic` 3D | — | — |
| wrapper | **wrapper_hit (end-to-end)** | **216.3** | **45.3** |
| wrapper | wrapper_hit **trusted fast-ABI** | **192.3** | **40.4** |
| wrapper | wrapper_fallback (miss → AOT) | 554.5 | 108.4 |

### 4.2 ns distribution (p50, batched, `-Os`)

| tier | workload | p50 ns | p95 ns | p99 ns |
|---|---|--:|--:|--:|
| internal | owner 0D slot0 | 10.55 | 11.60 | 13.24 |
| internal | owner 0D slot1 | 11.28 | 12.37 | 12.99 |
| internal | owner 0D slot5 | 14.79 | 16.07 | 17.67 |
| internal | owner 0D slot15 | 24.47 | 25.89 | 27.43 |
| internal | peer 0D slot0 | 11.25 | 12.35 | 13.03 |
| internal | peer 0D slot15 | 25.09 | 26.49 | 27.95 |
| internal | owner 1D | 14.21 | 15.49 | 16.78 |
| internal | owner 2D | 17.24 | 18.62 | 20.12 |
| cabi | cabi_0d | 13.40 | 14.63 | 15.31 |
| cabi | cabi_1d | 17.65 | 18.99 | 20.69 |
| cabi | cabi_2d | 21.78 | 23.18 | 24.55 |
| cabi | cabi_generic 3D | 26.28 | 27.82 | 29.55 |
| wrapper | wrapper_hit | 14.98 | 16.27 | 17.49 |
| wrapper | wrapper_hit trusted | 13.56 | 14.71 | 15.24 |
| wrapper | wrapper_fallback | 38.67 | 40.21 | 41.81 |
| baseline | direct AOT / fnPtr / empty | ~2.0 | | |

Observations:

* **Slot depth** (0/1/5/15): each extra occupied slot in a bucket adds ~1 ns;
  slot 15 is 2.3× slot 0. The common case is slot 0/1 (see §6); the deep-slot
  tail is real but not the steady state.
* **owner vs peer**: the peer memoized path adds only ~0.7 ns (one extra
  `executableCoreMask` acquire load + bit test).
* **0D → 1D → 2D → 3D**: each dimension adds ~3–4 ns (one more hash multiply,
  one more identity compare, one more version compare).
* **AOT vs JIT-direct vs wrapper-hit**: the AOT body and a direct call of the
  same compiled function are ~2 ns; the *entire* extra cost of going through the
  wrapper (cache query + C ABI + indirect dispatch + release) is the ~13 ns net
  the business observes as the AOT/JIT gap — and §5 shows it is dominated by the
  cache query, not the indirect branch.

---

## 5. Where the ~216 instructions go (answer to "is it < 100?")

**No — the full wrapper hit is ~216 instructions / ~45 cycles.** Layered
attribution (net of the empty loop):

| layer | ins | cyc | notes |
|---|--:|--:|---|
| indirect JIT call itself | ~9 | ~2 | the `blr` is cheap and well-predicted |
| cache query (tryCacheHit0D → lookup → resolveMatchedSlot) | ~137 | ~28 | **dominant**; the baseline `NO_RECLAIM` 0D path runs the *generic* `cacheLookupSeq`, **PR #82** unrolls it |
| C-ABI shell | ~47 | ~9.5 | out-param null-init + double write-back, `gEJIT→sharedTaskPool()` double load, status switch |
| wrapper glue | ~23 | ~5 | funcIndex load, status decode, branch layout, release call |

The critical structural finding: in the **baseline** the `NO_RECLAIM` 0D hit
uses the *generic* `cacheLookupSeq` (≈146 dynamic ins) even though a token build
uses the unrolled `cacheLookup0D` (≈126 ins). PR #82 closes exactly this gap by
adding `cacheLookupSeq0D/1D/2D`; this audit reproduces the gap but does not
re-implement the fix (see §9).

---

## 6. Directional audit findings

Each direction from the brief, with the current state, the measured cost, and
the disposition.

### 6.1 Wrapper (EJitWrapperGen)
* The fixed-dimension entries (`_0d/_1d/_2d`, scalar args, no `dims[]` alloca)
  are already emitted for `DimCount ≤ 2` and are the common path — confirmed.
* The wrapper does **not** rebuild a `dims[]` array or reload funcIndex/dimType
  redundantly on the fixed-dim path.
* The one clear residual is that the wrapper pays the **full public C-ABI
  contract** (out-param null-init, double write-back, status decode) on every
  hit even though it always passes valid pointers and only needs the fnPtr. See
  §8 (trusted fast-ABI).
* Hot/cold split: the AOT fallback body is already a separate block; a miss
  costs ~554 ins / ~108 cyc (§4.1) — 2.4× a hit — because it re-enters
  `compileOrGet` (enqueue/dedup) every call. For an *already-compiled* steady
  state this is off the hot path, but a workload with frequent misses/disabled
  instances/version bumps pays it repeatedly. **Recommendation (not
  implemented): a wrapper that, after the first fallback, remembers "not ready"
  cheaply would help miss-heavy phases** — needs careful invalidation and is out
  of this round's testable scope.

### 6.2 C-ABI shell (EJitRuntime.cpp)
* `if (outFn) *outFn = nullptr; … if (outFn) *outFn = fnPtr;` is a redundant
  double store + redundant null guard for the trusted wrapper caller.
* `gEJIT` → `gEJIT->sharedTaskPool()` is two dependent loads + two null checks
  per call; the pool pointer is stable after init.
* Disposition: addressed by the **additive** trusted fast-ABI (§8); the public
  ABI is left byte-for-byte unchanged.

### 6.3 Shared taskpool state checks
* `initState==Ready` (acquire) is read once per hit; `ownerCoreId` is a relaxed
  load (already relaxed by commit `ffb907e`); `codeSharingEnabled` is a
  compile-time constant in the production build (also `ffb907e`) — no per-hit
  runtime load. **No redundant Ready/mode/version re-reads found on the hit
  path.**
* `resolveMatchedSlot` is tight (~40 ins) and returns `SharedLookup`
  **register-packed** (`x0`=fnPtr, `x1`=flags+bucket) — no struct spill. The
  stack frame it does build is forced solely by the `EJitCoreId::current()`
  call. No cheap win found here without changing semantics.
* Bucket layout is `alignas(64)` (one bucket per cache line); no false sharing
  between buckets was observed.

### 6.4 Cache query
* Bucket index is `key % 32`; with 32 buckets the compiler lowers it to a mask
  — confirmed in disassembly. Hash is the golden-ratio multiply.
* Slot-depth distribution: the common hit is slot 0/1; deep slots (5, 15) are
  measured for worst-case but must not drive the design. Confirmed the linear
  16-slot scan cost is ~1 ns/slot.
* The fixed-dim direct paths for 0D/1D/2D exist. **The unrolled seqlock variants
  are PR #82's contribution** and are the right fix for the `NO_RECLAIM` gap in
  §5.

### 6.5 Read token / releaseRead  (key)
* Token build: `bucketTryRead` does `readers.fetchAdd`, re-checks `writeFlag`;
  `releaseRead` does `readers.fetchSub` — **two atomic RMWs per hit on the
  shared `readers` line** (`__aarch64_ldadd` outlined helpers here).
* `NO_RECLAIM` build: read is **load-only** (`writeFlag` acquire load +
  seqlock); `releaseRead` compiles to **1 instruction (ret)**.
* Measured (single core): token internal0d **67.3 cyc** vs `NO_RECLAIM`
  **30.6 cyc** — the token path is **~2.2× the cycles despite fewer
  instructions** because the atomic RMWs serialize. Under 30+ cores hitting one
  bucket the token `readers` line also bounces; the seqlock path never writes
  it. **This is the quantitative justification that the `NO_RECLAIM` premise
  (code pool never physically reclaims) is worth it, and that no per-hit read
  token is needed when publications are never freed.** The code already enforces
  "no physical release" structurally in that build (the releaser is never
  installed); the seqlock only refuses to *start* a read during a publish and
  re-checks `publishSeq` after, so a slot overwrite/version bump/deactivate can
  never hand back stale code.

### 6.6 Indirect function call
* A direct call of the compiled function through an opaque pointer is
  **9.1 ins / 2.4 cyc** — the indirect `blr` is essentially free here. The
  business "~200-cycle fn_call" is therefore **not** the indirect branch; it is
  the surrounding cache query + C-ABI + the callee body, or a
  BTI/PAC/mispredict effect specific to the board (§7).

### 6.7 JIT machine-code quality
* Not re-measured end-to-end here (needs the full compile pipeline + board). The
  benchmark isolates the *dispatch* cost from the *callee body* cost by
  providing a tiny leaf and a larger loop callee, so a future board run can
  attribute the callee-body delta separately. No `may_const` semantics were
  touched.

### 6.8 Statistics / diagnostics overhead
* Disassembly: with `EJIT_STATS_ENABLE` **off**, `classifyHit` is 33
  instructions with **zero** atomic/counter code. With it **on**, it is 46
  instructions with **two `bl __aarch64_ldadd8_acq_rel`** counter increments.
* Measured cost of stats **on**: +13.7 cyc on the bare 0D query
  (30.6 → 44.3 cyc, **+45 %**) and +9.4 cyc on the wrapper hit (45.2 → 54.6).
* `EJIT_DIAG_*` verbose calls are guarded by a level check and disappear at the
  default level. **Conclusion: statistics being opt-in (default off) is correct
  and materially important; no counter code remains in the disassembly when
  off.**

---

## 7. The board-specific unknowns (must be confirmed on hardware)

* `EJitCoreId::current()` is an **external `bl`** on the real platform
  (`ejit_sre_current_core_id()`), called once per hit inside
  `resolveMatchedSlot`. Its cost (a function call, possibly an `mrs MPIDR_EL1`)
  is not visible on the host (thread-local). If it is expensive it is a real
  per-hit tax; it cannot be inlined cross-TU without LTO.
* BTI/PAC on the customer build add landing pads / pointer-auth to the indirect
  `blr` that the host does not model.
* Branch prediction of a *single* stable fnPtr (as here) is best-case; a site
  that alternates between fnPtrs would mispredict more.

---

## 8. Optimization candidate: trusted-wrapper fast C-ABI (measured, not merged)

An **additive** internal entry (`bench_cabi_hit_0d` in the benchmark) returns
the fnPtr directly (nullptr on any non-hit), skipping the out-param null-init +
double write-back and the status switch; the wrapper then just tests
`fn != null`. The public `ejit_taskpool_compile_or_get*` ABI is untouched.

Measured (perf, 50 M iters):

| build | wrapper_hit | wrapper_hit trusted | Δ |
|---|--:|--:|--:|
| `NO_RECLAIM` | 216.3 ins / 45.3 cyc | 192.3 ins / 40.4 cyc | **−24 ins / −4.9 cyc (~11 %)** |
| token | 192.4 ins / 74.3 cyc | 168.4 ins / 72.2 cyc | −24 ins / −2.1 cyc |

Risk / why it is **not** in a production commit this round:
* It requires a new additive C-ABI symbol in `EJitRuntime.cpp` **and** a change
  to `EJitWrapperGen.cpp` to emit it for fixed-dim hits.
* Neither the C ABI nor the wrapper generator is covered by the unit tests that
  are buildable here (they need `gEJIT` / a full clang build), and this
  environment cannot run the customer target. Per the brief's rule that only
  reliably **validated** changes enter production code, it is delivered as a
  **measured prototype + recommendation**, pending an on-board + full-clang
  correctness run.

---

## 9. Relationship to PR #78 / PR #82

* **PR #82** (`codex/ejit-cache-query-audit`): adds unrolled fixed-dimension
  **seqlock** lookups (`cacheLookupSeq0D/1D/2D`), identityHash-first slot scan,
  slot-depth diagnostics, and a cache-query microbenchmark. This audit
  **independently confirms** that the generic `cacheLookupSeq` is the dominant
  cost in the baseline `NO_RECLAIM` 0D path (§5) and therefore that PR #82's fix
  is the right lever — but **does not re-implement it**. This audit's
  contribution is complementary: the *full three-tier* benchmark (internal +
  real C-ABI + wrapper end-to-end), the token-vs-seqlock and stats-on/off
  quantification, and the C-ABI/wrapper-plumbing analysis PR #82 does not cover.
* **PR #78** (`may_const` test): no overlap; `may_const` semantics were not
  touched.

---

## 10. Test & build validation

* `EJITTaskPoolTests`: **82 / 82 pass** (real CMake build).
* `EJITSharedTaskPoolTests` (default token config): **55 / 69 pass**; the 14
  failures are **pre-existing on the baseline** — they are cross-core
  code-sharing / FourK / PeerPrepare tests that require
  `EJIT_SRE_SHARED_CODE_POINTERS`, which commit `ffb907e` made a **compile-time**
  gate and the default test target does not define. They are mutually exclusive
  with the "sharing off" tests across configs and are **not** caused by this
  audit (which changes **no** runtime library code). This is documented here per
  the "no unexplained pre-existing failures" requirement.
* This audit adds **only** benchmark/diagnostic files and a report; the shared
  taskpool library is unchanged, so the test result is identical to baseline by
  construction.

## 11. Push status

Not pushed. No PR opened. Local commits only, working tree clean.
