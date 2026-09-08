# `ejit_free_dim` — design spec

## 1. Problem

A parameter EJIT cannot fold blocks every `may_const` field reached through it,

including the ones that never depended on it.

```c

ejit_entry

void DlschInitAlgoParaRelated2Trp(uint32_t ejit_dim("trp") trpIndex,

                                  uint32_t                 slotNo)

{

    DlschTrpAlgoPara *p = &g_dlschTrpAlgoPara[trpIndex * 5 + slotNo % 5];

    // ~30 may_const fields. None can be specialized today.

}

```

PASS6 needs the **whole** GEP index constant. `trpIndex` folds, `slotNo` does not, so

the sum does not — every load fails with `non-const-offset`. That includes the 70–80% of

fields copied from a slot-independent base, which are identical in all five elements.

> `slotNo` does not make the data unstable. It makes the data **unaddressable**.

---

## 2. The attribute

No arguments.

```c

#define ejit_free_dim __attribute__((ejit_free_dim))

ejit_entry

void DlschInitAlgoParaRelated2Trp(uint32_t ejit_dim("trp") trpIndex,

                                  uint32_t ejit_free_dim   slotNo);

```

**Applies to**: an integer parameter, 32 bits or fewer, of an `ejit_entry` function.

**Asserts**: every `ejit_may_const` field whose address depends on this parameter holds

the **same value for every value the parameter takes**. The data is *free of* this

dimension — hence the name.

**Does not assert** anything about the parameter itself. `slotNo` changes every TTI. The

claim is about the memory it addresses, not about the argument.

Because the values do not vary along the axis, the JIT evaluates addresses at a fixed

**witness of 0** — equivalent to dropping the `+ slotNo % 5` term. One specialization

serves every slot value.

---

## 3. The one rule

> **The witness is used only inside PASS6's address arithmetic. It never enters the IR.**

The parameter is **not** RAUW'd. The same GEP feeds the stores:

```c

*p = *baseTrpAlgoPara;      // the init function WRITES through this address

```

Substituting `slotNo → 0` in the IR would send all twenty TTIs' stores into element

`trp*5 + 0`, and elements 1–4 would never be updated again. Silent memory corruption, not

a missed optimization.

So the witness authorises **replacing a `may_const` load's result** and nothing else. The

GEP, the stores, and every other use keep the live parameter. A second reason to hold this

line: if the GEP were rewritten and PASS6 then failed to fold the load — unregistered

global, unsupported type — the result would be a runtime load from the wrong element.

Failure must degrade to *no optimization*, never to *wrong address*.

Corollary: the witness picks **which memory to trust**. Witness 0 always reads slot 0's

copy. If the contract holds, all five agree and the choice is invisible; if it does not,

the wrong value is baked in silently. See §7.

---

## 4. Not a dimension on the wire

`ejit_free_dim` is not part of the specialization identity:

| | `ejit_dim` | `ejit_free_dim` |

|---|---|---|

| in the cache key | yes | **no** |

| inline-cache axis | yes | **no** |

| `dimType` slot | one per period | **none** |

| clones per entry | one per instance | **one, total** |

| counts against the 4-dim budget | yes | no (capped separately at 4) |

| invalidated by | its period | its siblings' periods |

It exists only as metadata read by the JIT optimizer. **No runtime ABI change, no wrapper

change, no `dimType` reservation, no activation gate, no version bookkeeping.**

Invalidation comes free: the clone is keyed by the surviving lifecycle dims, so

`ejit_deactivate("trp", n)` bumps a version, the lookup's snapshot compare fails, and the

clone is retired exactly as today.

---

## 5. What changes

| Layer | Change |

|---|---|

| `Attr.td` | `EjitFreeDim : InheritableParamAttr`, spelling `ejit_free_dim`, no args |

| Sema | `handleEjitFreeDimAttr`: integer, ≤ 32 bits, enclosing function is `ejit_entry`, conflicts with `ejit_dim` / `ejit_bound_ptr`. Warn when the entry has no lifecycle dim — nothing could then invalidate the frozen values |

| CodeGen | emit `!{!"ejit_free_dim", !"", i32 argIndex}` from the **same parameter-ordered loop** as the other dims |

| AOT passes | **none.** Every dim-enumerating loop filters by tag, so an unknown tag is skipped by construction; PASS1 clones the whole `!ejit.metadata` node into the blob |

| `EJitOptimizer` | `preReplaceSpecializationIndices` must **explicitly skip** the tag — it may never RAUW. `runStructFieldPass` parses the free-dim nodes off the entry and builds the assumed map |

| `EJitStructFieldPass` | `DenseMap<const Argument *, uint64_t> AssumedArgs`; `accumulateFullOffset` / `computeGEPOffset` fall back to a bounded evaluator when an index is non-constant; new diagnostic reason `assumed-offset` |

| Runtime | **none** |

### The evaluator

Bounded recursive constant-fold over the index expression with `AssumedArgs` supplying the

argument's value. Leaves: `ConstantInt`, mapped `Argument`. Opcodes:

`add sub mul shl and or xor urem udiv srem sdiv zext sext trunc select`. Depth-capped, no

memory reads, bail on anything else.

Substitution rather than subexpression deletion, deliberately: by the time PASS6 runs

IPSCCP has folded `trpIndex`, so the expression is `add(15, urem(slotNo, 5))` or whatever

InstCombine rewrote it into. Folding is immune to that; pattern-matching a term is not.

Deletion is also undefined for shapes like `(slotNo + trp) * 5`, where substitution is not.

---

## 6. What it buys

Against specializing on the slot axis instead:

- **One clone, not twenty.** Warm-up drops by the same factor — the in-flight dedup table

  is keyed by `funcIndex` only, so clones are gained one per compile-duration and other

  requests are dropped rather than queued.

- **No `maxCodeMemory` multiplication** (2 MB default).

- **The `D = 16` cliff never applies.** `EJIT_ICACHE_DIM_SIZE` is 16; with 20 slot values,

  a slot axis would leave slots 16–19 with no inline-cache cell at all, calling into the

  runtime every TTI. With no slot axis there is nothing to overflow.

- **No orphaned specializations.** A dimension with no lifecycle has no invalidation path;

  a free dim adds no axis, so nothing is orphaned.

---

## 7. Verification — ships with it, not after

Nothing in the compiler or the runtime can check the contract. It must be measurable.

Extend `EJIT_VERIFY_SUBSTITUTION` (`EJitVerify.h`): at each `may_const` site folded via an

assumed offset, also read the value at the **actual** runtime index and compare. A mismatch

is an unambiguous contract violation with a function name and field offset attached.

Run the workload under this build before trusting any `ejit_free_dim` annotation, and keep

it in the integration-test configuration.

---

## 8. What it does not fix

Freeing the index makes blocked fields **reachable**; it says nothing about whether they

are **stable in time**.

- Fields written from live load metrics stay unsafe to freeze, and are unaffected by this

  attribute in either direction.

- `may_const` freezes a whole field. A bitmap mixing a slot-invariant bit with a

  load-driven bit still cannot be marked at all.

- Annotate the **raw** parameter, never a pre-reduced one: `slotNo % 5` would merge slots

  that share a buffer element but sit at different points in the TDD pattern.

---

## 9. Tests

| Layer | Coverage |

|---|---|

| `clang/test/Sema/ejit_free_dim.cpp` | non-integer, > 32 bits, non-`ejit_entry`, conflict with `ejit_dim` / `ejit_bound_ptr`, no-lifecycle warning, dim-budget interaction |

| `clang/test/CodeGen/ejit_free_dim.c` | metadata node shape; parameter-ordered emission alongside `ejit_dim` |

| `llvm/test/Transforms/EmbeddedJIT/` | wrapper emits **no** dim for a free param; icache table shape unchanged |

| `EJitRuntimeTest` | evaluator over `a*5 + b%5`, `select`, and a bail case; optimizer does **not** RAUW a free-dim argument; free dim absent from the cache key |

| `ejit_test/` | end-to-end: five slot values produce one clone; **stores still land in the correct element**; `ejit_deactivate` on the sibling period retires it |

| verify build | §7 mismatch detector fires on a deliberately non-uniform field |
