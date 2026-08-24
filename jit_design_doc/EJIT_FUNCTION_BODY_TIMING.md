# EJIT function-body cycle timing

This diagnostic mode compares the executed AOT and JIT bodies without charging
taskpool lookup, inline-cache probing, dispatch, read-token release, or timing
aggregation to the sample.

## Build

Pass the hidden AOT option when compiling translation units that contain
`ejit_entry` functions:

```text
-mllvm -ejit-function-body-timing
```

The option is off by default. With it off, no timestamp or recorder calls are
emitted and the inline-cache hit path keeps its frame-less `musttail` shape.

## Read results

After the workload has reached the window to compare, print the accumulated
samples on each participating core:

```c
ejit_function_body_cycles_print();
```

Example output on SRE:

```text
function_body_cycles: unit=cycles slots=128 dropped=0
  func=process_cell path=AOT count=1000 avg=842 min=790 max=1201 total=842000
  func=process_cell path=JIT count=1000 avg=516 min=488 max=901 total=516000
```

Use `ejit_function_body_cycles_reset()` immediately before a measurement window
to exclude warm-up. `ejit_function_body_cycles_get()` provides the same
`count/total/min/max` data without parsing logs; `avg` is `total / count`.

SRE samples use `SRE_CycleCountGet64()`. Host builds use steady-clock
nanoseconds for structural tests and must not be compared numerically with SRE
cycles. The fixed-capacity table does not allocate. `dropped` reports samples
lost to table capacity or a nested/concurrent recorder; the hot recorder uses a
single try-lock and never waits in the measured workload's return path.

The interval necessarily contains the cost of reading the ending cycle counter.
Use identical instrumentation for AOT and JIT, compare averages over many
samples, and treat very small functions near the two-read measurement floor
with caution.

Only normal returns are sampled. An AOT exit consisting of a `musttail` call and
its required adjacent return is left untouched and is not reported, because
inserting a recorder there would invalidate the IR or change tail-call
behaviour.
