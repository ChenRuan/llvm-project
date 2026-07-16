# SRE EJIT Body Performance Diagnosis

This directory isolates performance *inside* generated machine code. It is not
a wrapper/cache-lookup benchmark.

## Files

- `sre_jit_body_perf_diagnosis.c`: complete single-file test containing the
  EJIT/AOT pairs, original-image helper, and board entry.

Compile this one file with the normal EJIT attribute pipeline and link its object
into the final SRE image. Its entry matches the existing complex multi-core demo:

```c
int test_ejit_period(uint8_t cellIdx, uint8_t trpIdx,
                     uint8_t sliceIdx, uint8_t carrierIdx);
```

The four shell arguments are intentionally ignored. The real core id comes from
`g_ucLocalCoreID`, and dimensions are fixed inside the program. Start core 8
first so it initializes/elects the shared compile worker, then invoke the same
entry on the remaining cores. The elected worker skips the business benchmark
and stays alive. All business cores request instance 0, so they reuse exactly
one specialization.

The callback scenario passes the original-image helper address as a runtime
function-pointer argument. This keeps the helper out of the JIT bitcode closure
without requiring a second translation unit.

The test registers dense indices before `ejit_init`, triggers asynchronous
compilation once, then obtains the generated address through
`ejit_taskpool_compile_or_get_1d`. It keeps the returned read token for the full
measurement and releases it afterwards. Consequently, reported AOT/JIT cycles
contain only the indirect call and function body; wrapper lookup, hashing,
bucket scanning, and `release_read` are excluded.

## Reading The Results

Each scenario prints first-call, best batch average, mean batch average, code
addresses and AOT-to-JIT address distance.

| Scenario | What it isolates |
| --- | --- |
| `compute` | No global loads or external calls. Persistent JIT slowdown points to generated-code quality, placement, I-cache conflict, or an indirect-call effect. A slow first call that disappears after warmup points to cold I-cache / per-core first touch. |
| `pointer` | Repeated loads from the same runtime data, but its address is passed as an argument. This is the memory-access control group. |
| `global` | Repeated direct references to the source image global. A regression relative to `pointer` indicates global relocation/GOT/far-address materialization rather than data-cache latency. |
| `snapshot` | One direct global load, then local computation. If `global` is slow but `snapshot` is not, repeated global addressing/loading is the cause. |
| `callback1` | Repeated calls to one noinline function in the original image. This is the stable-target baseline. |
| `callback4` | Round-robin calls to four source-image helpers placed on distinct 4K-aligned text pages. This expands branch-target and instruction-page working sets. |
| `callback8` | Round-robin calls across all eight helper pages. Compare with `callback4` to detect scaling with layout spread. |
| `random8` | Pseudo-random calls across all eight helpers. A unique regression here points to indirect branch prediction rather than code distance alone. |

Useful comparisons:

1. Compare `compute first` with `compute best`. Only a first-call penalty supports
   an I-cache warmup hypothesis. A missing cache-maintenance operation usually
   causes stale instructions or faults, not a stable 20% steady-state penalty.
2. Compare the JIT/AOT ratios of `pointer`, `global`, and `snapshot`; do not use
   absolute cycles alone because all three contain the same arithmetic body.
3. Compare `callback1`, `callback4`, `callback8`, and `random8`. Growth from
   1→4→8 indicates instruction-page/target working-set sensitivity; a jump only
   in `random8` indicates indirect branch prediction sensitivity. The test logs
   every helper address, 4K page number, adjacent gap and total layout span, so
   confirm the linker preserved the intended spread before interpreting it.
4. Run several cores together. Per-core first-call regressions suggest per-core
   executable permission/I-cache preparation. A degradation only under
   concurrency suggests shared cache/memory-system contention.

The workload can be tuned at compile time with `PERF_INNER_ITERS`,
`PERF_CALLBACK_ITERS`, `PERF_WARMUP_CALLS`, `PERF_BATCH_CALLS`, and
`PERF_BATCHES`. Set `PERF_KEEP_WORKER_IDLE=0` only when intentionally measuring
on the worker core. Set `PERF_INSTANCE_ID` to test a different shared
specialization; keep the value below 32.

The test deliberately does not execute manual `dc`/`ic`/`isb` maintenance: that
would hide the defect being diagnosed. Permission and cache synchronization must
remain the responsibility of the production code-pool path.
