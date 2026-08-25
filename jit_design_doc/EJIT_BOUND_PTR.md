# EJIT dimension-bound pointer snapshots

`EJIT_BOUND_PTR(period)` associates one pointer parameter with an existing
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

On a real JIT cache miss, the wrapper copies the complete `CellConfig` object
into the compile request before returning. The asynchronous worker therefore
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

- The function must have exactly one matching `EJIT_DIM(period)` parameter.
- The first implementation supports one `EJIT_BOUND_PTR` parameter per entry.
- Helper propagation accepts pointer casts and constant-offset GEPs. It stops
  conservatively at indirect calls, address-taken helpers, or a helper argument
  that receives different pointer sources at different call sites.
- The copy is shallow. Pointer fields inside the object are not followed.
- An `ejit_may_const` field must remain stable while its period instance is
  active. Change it only as part of the normal deactivate/update/reactivate
  lifecycle so the period version invalidates old specialized code.
- The default snapshot cap is 256 bytes. Configure
  `EJIT_BOUND_PTR_MAX_BYTES` to a positive multiple of 8 when a larger object
  is required. An oversize request falls back to AOT without queuing work.
- Volatile, atomic, bit-field, union, dynamically indexed, and unmarked field
  loads are never folded from the snapshot.

The snapshot is carried inline in `EJitCompileRequest`. This increases shared
queue storage by `EJIT_BOUND_PTR_MAX_BYTES * queue capacity`, but avoids heap
allocation, ownership handoff, cleanup races, and dangling-pointer failure
paths on the worker side.
