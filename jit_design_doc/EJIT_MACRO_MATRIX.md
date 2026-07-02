# EmbeddedJIT Macro / Build-Switch Matrix

**Status**: Phase 2 — documentation + build-entry aggregation + centralized
SRE default constants.
**Scope**: This document inventories the EmbeddedJIT (EJIT) compile-time macros
and the `build.sh` / CMake switches that drive them, classifies them, and
records why several macros cannot be deleted yet. It is intentionally
non-invasive: it does **not** change any runtime behavior, does **not** remove
any macro, and does **not** alter the meaning of any existing CMake option.
Phase 2 additionally centralizes SRE-facing default constants in
`EJitSreConfig.h`, so adapter `.cpp` files do not each carry their own fallback
`#ifndef` blocks.

> **Baseline note (important)**: This document is based on `ejit_dev_spec4`,
> which already contains the cross-core shared taskpool implementation. Shared
> taskpool macros are therefore documented as real, load-bearing bring-up
> switches. Phase 2 does **not** delete, fold, runtime-ize, or otherwise alter
> those paths.

---

## 1. How the layers fit together

```
build.sh flag  ─►  CMake option (cache var)  ─►  add_definitions(-DMACRO)  ─►  #ifdef in C/C++
   (convenience)        (llvm/CMakeLists.txt,            (compile-time)            (source)
                         llvm/lib/.../EJIT/CMakeLists.txt)
```

`build.sh` is only a convenience front-end: every flag expands into one or more
`-DEJIT_*` CMake cache variables. The CMake layer is the single source of truth
for macro semantics. This document does not change that mapping; the Phase-1
`build.sh` additions (see [§6](#6-aggregate-convenience-switches-buildsh-only))
are pure aliases that expand into the **existing** CMake options.

---

## 2. Category A — Compile-time macros that MUST be kept

These select mutually-incompatible ABI / code-generation / platform paths. They
cannot become pure runtime flags without shipping both code paths in one binary,
which defeats the size and freestanding goals of EmbeddedJIT.

| Macro | Driven by | Why it must stay a compile-time macro |
|-------|-----------|----------------------------------------|
| `EJIT_FREESTANDING` | CMake `EJIT_FREESTANDING` (+ `build.sh --freestanding`) | Removes OS threads, file I/O, and logging; swaps in bare-metal mutex behavior and excludes `EJitSyncCompiler.cpp` / `EJitAsyncCompiler.cpp` from the library. It also flips `<future>`/`std::promise` guards in JITLink headers. A runtime flag cannot un-link `std::thread`. **52 `#ifdef` sites.** |
| `EJIT_SRE_TASKPOOL` | CMake `EJIT_SRE_TASKPOOL` (+ `build.sh --sre-taskpool`) | Compiles the taskpool sources (`EJitTaskPool.cpp`, `EJitSreQueue.cpp`, `EJitWorker.cpp`, `EJitSreTask_*.cpp`) into the library and reroutes `ejit_compile_or_get`. Toggling it changes which translation units exist. **23 `#ifdef` sites.** |
| `EJIT_SRE_SHARED_TASKPOOL` | CMake `EJIT_SRE_SHARED_TASKPOOL` (+ `build.sh --sre-shared-taskpool`) | Enables the cross-core shared taskpool layer: shared state, owner election, shared queue/cache/dedup/control state, and single-worker coordination. It requires `EJIT_SRE_TASKPOOL=ON` and changes runtime wiring, so it must remain compile-time until the shared path is fully productized. |
| `EJIT_SRE_CODE_POOL` | CMake `EJIT_SRE_CODE_POOL` (+ `build.sh --sre-code-pool`) | Routes JIT code memory through the SRE 2 MiB code pools and (implicitly) enables `EJIT_SRE_ENABLE_EX` RX sealing. Changes the memory-manager path used to hand back executable pointers. **9 `#ifdef` sites.** |
| `EJIT_TRIM_LLVM_BACKEND` | CMake `EJIT_TRIM_LLVM_BACKEND` | Disables heavyweight debug-info / GlobalISel / optional AArch64 SME-SVE passes at build time. Purely a link/size decision. |
| `EJIT_TRIM_LLVM_BACKEND_EXPERIMENTAL` | CMake option (+ `build.sh --trim-llvm-backend-experimental`) | Bare-metal backend trim (non-ELF formats, non-AArch64 targets, DWARF/CFI). Implies `EJIT_TRIM_LLVM_BACKEND`. Build-time only. |
| `EJIT_SRE_SHARED_TASKPOOL_PLATFORM` | CMake-derived define when `EJIT_SRE_SHARED_TASKPOOL=ON` and `EJIT_FREESTANDING=ON` | Selects the real platform-facing shared-taskpool hooks instead of host/test seams. It affects core-id/current-core behavior and shared-section expectations, so it is not a runtime flag. |
| `EJIT_DEFAULT_TRIPLE` (from `EJIT_DEFAULT_TARGET_TRIPLE`) | EJIT lib CMake (+ `build.sh --target-triple=`) | Baked-in target triple that replaces host detection; required by freestanding. It is a compile-time string define on `LLVMEJIT`. |

**Rule**: none of these may be deleted or runtime-ized in Phase 2.

---

## 3. Category B — Numeric parameter macros (candidates for FUTURE runtime config)

These are integers that size fixed tables / pools. They could *in principle*
migrate to a runtime config struct later, but only after the owning subsystem's
data structures stop using them for static sizing. They are **not**
runtime-ized in Phase 2.

| Macro | Driven by | Default | Future note |
|-------|-----------|---------|-------------|
| `EJIT_SRE_CODE_POOL_SIZE` | CMake `EJIT_SRE_CODE_POOL_SIZE` | `2097152` (2 MiB) | Must remain a multiple of the enable_ex granularity; currently a compile-time constant. Not yet exposed in `build.sh`. |
| `EJIT_SRE_CODE_POOL_PTNO` | CMake (+ `build.sh --sre-code-pool-ptno=`) | `8` | SRE memory partition number handed to `SRE_MemDbgAlloc`. Already reachable from `build.sh`. |
| `EJIT_SRE_TASKPOOL_BUCKETS` | CMake (+ `build.sh --sre-taskpool-buckets=`) | `32` | Dedup/cache bucket count. |
| `EJIT_SRE_TASKPOOL_QUEUE_CAPACITY` | CMake (+ `build.sh --sre-taskpool-queue-capacity=`) | `1024` | Async queue capacity, rounded to power of two. |
| `EJIT_SRE_TASKPOOL_MAX_FUNC_INDEX` | CMake `EJIT_SRE_TASKPOOL_MAX_FUNC_INDEX` | `4096` | Flat dedup-table capacity (`inFlight_[]`). Sizes a static array today → cannot be runtime-ized without reworking that table. Not yet exposed in `build.sh`. |
| `EJIT_SRE_TASKPOOL_WORKER_STACK_SIZE` | CMake (+ `build.sh --sre-taskpool-worker-stack-size=`) | `1048576` | Shared worker stack size. It is already a build parameter but still participates in platform task creation and should not be changed by macro cleanup. |
| `EJIT_SRE_SHARED_TASKPOOL_CACHE_SLOTS` | CMake `EJIT_SRE_SHARED_TASKPOOL_CACHE_SLOTS` | `16` | Fixed-slot shared cache size. Static shared-state layout depends on it. |

**Rule**: leave as compile-time; do not runtime-ize a batch of these in Phase 2.

---

## 4. Category C — Macros that could be ALIASED / AGGREGATED later

These are not deletable, but callers frequently want to enable them **together**
for a given bring-up scenario. Phase 1 added `build.sh` convenience aliases that
bundle them (see [§6](#6-aggregate-convenience-switches-buildsh-only)); the
underlying CMake options and macro semantics are unchanged.

| Underlying CMake option | Existing explicit `build.sh` flag | Bundled by aggregate profile |
|-------------------------|-----------------------------------|------------------------------|
| `EJIT_SRE_CODE_POOL` | `--sre-code-pool` / `--no-sre-code-pool` | `--ejit-sre-code-pool-profile`, `--ejit-sre-async-profile` |
| `EJIT_SRE_TASKPOOL` | `--sre-taskpool` / `--no-sre-taskpool` | `--ejit-sre-profile`, `--ejit-sre-async-profile` |
| `EJIT_SRE_SHARED_TASKPOOL` | `--sre-shared-taskpool` / `--no-sre-shared-taskpool` | Not bundled in Phase 2; enable explicitly so cross-core behavior is never switched on by surprise. |
| `EJIT_SRE_SHARED_CODE_POINTERS` | `--sre-shared-code-pointers` / `--no-sre-shared-code-pointers` | Not bundled in Phase 2; requires platform same-VA and per-core executable permission/coherency validation. |

---

## 5. Shared-taskpool cleanup boundary

The shared taskpool exists in this baseline and has already been exercised on
target hardware. That makes it a poor candidate for a first-round macro
cleanup. In this phase:

- do not merge `EJitTaskPool` and `EJitSharedTaskPool`;
- do not remove `EJIT_SRE_SHARED_TASKPOOL`;
- do not fold `EJIT_SRE_SHARED_TASKPOOL_PLATFORM` into a runtime flag;
- do not put shared code-pointer enablement into an aggregate profile;
- do not change owner election, shared queue, shared cache, dedup, activation
  state, worker startup, or code-permission behavior.

Future cleanup can introduce a facade or higher-level build profile after the
shared path has a stable product configuration, but that is intentionally out of
scope for this conservative cleanup phase.

---

## 6. Centralized SRE defaults introduced in Phase 2

`llvm/include/llvm/ExecutionEngine/EJIT/EJitSreConfig.h` is the single local
header for SRE-facing default constants:

- `EJIT_SRE_CODE_POOL_SIZE`
- `EJIT_SRE_CODE_POOL_PTNO`
- `EJIT_SRE_CODE_POOL_MID`
- `EJIT_SRE_TASK_PRIORITY`
- `EJIT_SRE_TASKPOOL_WORKER_STACK_SIZE`
- fixed page-size constants used by the SRE adapter (`2MiB`, `4KiB`)

This is a code-layout cleanup only. The CMake cache variables and compile-time
macros remain the source of values, and the generated binary sees the same
constants as before. The goal is to avoid repeating fallback `#ifndef` blocks
and magic defaults in multiple SRE adapter files.

---

## 7. Bring-up macros that CANNOT be deleted yet (justification)

The task specifically asks why the following cannot be removed. Note that some
are compile-time paths still under active bring-up and some are simply not
present here.

- **`EJIT_FREESTANDING`** — Load-bearing. It gates 52 `#ifdef` sites, removes
  entire translation units from the library, injects bare-metal mutex behavior,
  and toggles `<future>`/`std::promise` guards inside JITLink headers. Deleting
  it would either force OS-thread dependencies into bare-metal targets or delete
  the freestanding target entirely. Keep.

- **`EJIT_SRE_CODE_POOL`** — Load-bearing. It swaps the JIT executable-memory
  manager for the SRE 2 MiB pool + `enable_ex` RX-seal path (`EJIT_SRE_ENABLE_EX`
  is implied). Removing it would drop the sealed-code-memory path required by the
  target platform. Keep. (Permission / 4K-2M sealing logic is explicitly out of
  scope for this phase.)

- **`EJIT_TRIM_LLVM_BACKEND`** — Load-bearing for size-constrained builds. It is
  the switch that trims debug-info/GlobalISel/optional AArch64 passes, and it is
  implied by `EJIT_TRIM_LLVM_BACKEND_EXPERIMENTAL`. Removing it would either bloat
  the trimmed builds or silently break the experimental trim. Keep.

- **`EJIT_SRE_SHARED_TASKPOOL_PLATFORM`** — Load-bearing for the SRE
  cross-core shared-taskpool path. It selects platform-facing behavior for
  current-core identity / shared-state placement instead of host test seams.
  Turning this into a runtime flag would require shipping both platform and host
  seams in one binary and would make freestanding link behavior less explicit.
  Keep.

- **`EJIT_DIAG_ENABLE`** — Diagnostic logging gate in
  `llvm/include/llvm/ExecutionEngine/EJIT/EJitDiag.h`. When undefined, the
  `EJIT_DIAG(...)` macros compile to no-ops; when defined they emit logging. It is
  a manual/per-TU compile define (not wired into a CMake option), used during
  bring-up and debugging. Deleting it would remove the ability to turn EJIT trace
  logging on without editing source. Keep as a compile-time gate.

---

## 8. Other compile-time defines observed (manual / test-only)

Recorded for completeness; not wired to a CMake option and **not** changed here.

| Macro | Where | Nature |
|-------|-------|--------|
| `EJIT_DISABLE` | source guard | Manual define to compile EJIT out of a TU. |
| `EJIT_SRE_ENABLE_EX` | implied by `EJIT_SRE_CODE_POOL` in CMake | RX-seal call; toggled with the code pool. |
| `EJIT_SRE_SHARED_CODE_POINTERS` | CMake/build.sh shared-taskpool option | Allows non-owner cores to consume shared cache function pointers; keep explicit because it relies on same-VA mapping and per-core executable permission/coherency. |
| `EJIT_CODE_POOL_4K_SEAL` | code-pool branch / CMake option when present | 4K sealing strategy switch; do not fold into profiles until platform validation is complete. |
| `EJIT_SRE_TASKPOOL_TESTING` | taskpool unit tests | Test-only define. |
| `EJIT_SRE_TASKPOOL_PLATFORM_IPC_LOCK` | taskpool source | Platform IPC-lock path; manual define. |
| `EJIT_SRE_CODE_POOL_MID` | code-pool source | Manual define. |
| `EJIT_FREESTANDING_MUTEX` | EJIT lib CMake option | Injects no-op mutex headers for toolchains lacking `<mutex>`. |

---

## 9. `build.sh` flag precedence (authoritative)

`build.sh` parses flags **left to right**; for any switch that maps to the same
underlying variable, **the last occurrence on the command line wins**. This is
the only precedence rule and it applies uniformly to explicit flags and to the
Phase-1 aggregate profiles.

Consequences:

- Aggregate profiles (`--ejit-sre-profile`, `--ejit-sre-async-profile`,
  `--ejit-sre-code-pool-profile`) simply *set* the same `EJIT_SRE_*` variables
  that the explicit flags set. They add **no** new macro and change **no** macro
  meaning.
- To override one feature of a profile, place the explicit flag **after** the
  profile. Examples:

  ```bash
  # taskpool + code pool, but force code pool back off:
  ./build.sh release aarch64 --ejit-sre-async-profile --no-sre-code-pool

  # code pool only, but also enable taskpool:
  ./build.sh release aarch64 --ejit-sre-code-pool-profile --sre-taskpool
  ```

- Old explicit flags remain fully supported and are the recommended way to be
  unambiguous. Profiles are conveniences, not replacements.

### 9.1 Aggregate profile expansions (Phase 1)

| Aggregate flag | Expands to (existing switches only) |
|----------------|-------------------------------------|
| `--ejit-sre-code-pool-profile` | `EJIT_SRE_CODE_POOL=ON` |
| `--ejit-sre-profile` | `EJIT_SRE_TASKPOOL=ON` |
| `--ejit-sre-async-profile` | `EJIT_SRE_TASKPOOL=ON` + `EJIT_SRE_CODE_POOL=ON` |

> **Async note**: sync vs async is a **runtime** choice (`ejit_set_compile_mode`
> / `Config.compileMode`), not a build macro. `--ejit-sre-async-profile` only
> bundles the *build* features typically needed for the async SRE bring-up
> (taskpool scheduler + sealed code pool); it does not itself select async at
> runtime and it defines no new macro.

---

## 10. Runtime behavior statement

Phase 2 changes are **documentation**, **pure `build.sh` convenience aliases**,
and **centralized SRE default constants**. No CMake option semantics change, no
macro is added or removed, and the default build (`./build.sh <debug|release>
<arch>` with no EJIT flags) uses the same values as before. **Runtime behavior
is unchanged.**
