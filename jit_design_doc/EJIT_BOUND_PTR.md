# EJIT dimension-bound pointer snapshots

`EJIT_BOUND_PTR(period)` associates one or more pointer parameters with existing
`EJIT_DIM(period)` parameter. It lets EJIT specialize fields whose values are
stable for that period instance without requiring a global array name.

```c
typedef struct {
  uint32_t ejit_may_const algorithm;
  uint32_t ejit_may_const scale;
  uint32_t runtime_bias;
} CellConfig;

EJIT_ENTRY uint32_t process(
    EJIT_DIM(cell) uint8_t cell,
    EJIT_BOUND_PTR(cell) const CellConfig *config,
    uint32_t input) {
  return input * config->scale + config->runtime_bias;
}
```

On a real JIT cache miss, the wrapper copies each complete pointee into one
runtime-owned snapshot before returning. The asynchronous worker therefore
never retains or dereferences the caller's pointer. Stack-local objects are
safe to destroy immediately after the entry call.

Only loads of `ejit_may_const` fields are replaced from the snapshot. Other
fields, such as `runtime_bias` above, remain ordinary loads through the pointer
passed to each invocation of the JIT function.

The annotation belongs only on the `ejit_entry` parameter. When that pointer is
passed through non-inlined direct helpers, EJIT tracks the corresponding formal
argument through the call chain and specializes marked field loads in those
helpers from the same owned snapshot. Helpers do not need `ejit_entry` or a
repeated `EJIT_BOUND_PTR` annotation.

## Contract and limits

- Every bound pointer must have exactly one matching `EJIT_DIM(period)` parameter.
- An entry may have multiple `EJIT_BOUND_PTR` parameters. They are represented by
  independent descriptors and byte ranges, even when they use the same period.
- An entry may bind at most 8 pointer parameters. This is a bound on the small
  wrapper descriptor table only; it does not limit the size of any snapshot.
- Helper propagation accepts pointer casts and constant-offset GEPs. It stops
  conservatively at indirect calls, address-taken helpers, or a helper argument
  that receives different pointer sources at different call sites.
- The copy is shallow. Pointer fields inside the object are not followed.
- An `ejit_may_const` field must remain stable while its period instance is
  active. Change it only as part of the normal deactivate/update/reactivate
  lifecycle so the period version invalidates old specialized code.
- There is no fixed snapshot byte cap. The queue carries a pointer to a
  runtime-owned allocation containing a descriptor table and the copied bytes;
  the queue slot size is independent of the pointee size. Arithmetic overflow,
  invalid input, or allocation failure falls back to AOT without queuing work.
- Snapshot allocation is platform-configurable. Hosted builds use `malloc` /
  `free` by default. Freestanding/SRE builds intentionally have no default
  allocator. On SRE, every producer core that may execute an entry wrapper
  must call `ejit_set_bound_snapshot_allocator()` before that core's own
  `ejit_init()`; configuring only the fixed worker core is insufficient. Each
  producer must use the same paired allocator/free contract: the returned
  storage, the `freeFn` code, and `ctx` must all be accessible and callable at
  the same virtual address from the owner worker. The callbacks, context, and
  allocator backing storage must remain valid until all queued snapshots have
  drained. If no valid pair is installed, bound-pointer requests fail cleanly
  and use AOT; an incomplete pair passed to the setter is rejected and leaves
  the previous configuration unchanged.
  The runtime does not assume that an SRE code-pool allocator can release
  arbitrary snapshot storage.
- The cache key remains the function index plus dimension tuple rather than
  the snapshot bytes. This avoids one version per observed object value, but
  requires all bound objects for a lifecycle instance to be stable and
  consistent with that key.
- Volatile, atomic, bit-field, union, dynamically indexed, and unmarked field
  loads are never folded from the snapshot.

The `EJitCompileRequest` carries only a same-address-space owned-snapshot handle.
The producer transfers ownership after a successful queue enqueue; the worker
releases it on every compile, drop, and publish path. Pending shared batch
metadata is sanitized before the snapshot is released. This keeps shared queue
storage bounded and avoids both unbounded stack copies and dangling caller
pointers. The copy is shallow and is not an atomic multi-object transaction:
callers must synchronize writers, and overlapping/aliased input pointers are
copied in descriptor order into independent ranges.

The snapshot allocation itself is one contiguous block and is not silently
truncated. Dynamic allocation happens on the first cache-miss path that queues
the specialization, not on steady-state cache hits. A very large or
unavailable allocation is a normal AOT fallback; platform integrations should
budget allocator capacity and allocation latency for this miss path rather
than adding a hidden byte threshold here.

The current branch keeps the shared taskpool ABI at v18. When rebasing onto a
combined line that has advanced the shared ABI, coordinate the required bump
to v19 or later together with the corresponding producer/worker images.
