# Pointer-period element-zero witness folding

Implementation follow-up to [PR254](https://github.com/dongjianqiang2/llvm-project/pull/254),
on top of PR252. This is independent of PR255's frontend nested-array metadata fix.

## Purpose and usage

Permit a marked load such as `p[slot % 5].flag` or
`p->slots[slot % 10].flag` to fold when the pointer's allocation size is unknown.
The slot must already be an `ejit_free_dim`, and the load/field must already
carry the existing `may_const` authorization. The compiler evaluates the
address with the free dimension set to zero, reads that representative field,
and replaces only the load's result. The argument, live payload accesses and
stores keep their real slot values.

No new SDK hook, public registration API, initialization function, CMake option
or Clang flag is required. Existing `ejit_init_pgo` usage is unchanged. Rebuild
the runtime; business bitcode with PR249 pointer-layout metadata can be reused.
Missing frontend field metadata is a separate problem, not repaired by this
runtime fallback.

## Deployment contract

The application explicitly guarantees all of the following, including for
marked loads in branches not taken by the call that triggered compilation:

1. The registered pointer slot and the entire first pointee element are live,
   initialized, coherent and readable by the compiler worker throughout its
   read. A non-null pointer alone is not proof of this property.
2. The marked values are equal for all supported free-dimension values in the
   active configuration. The representative value is not a guess at the most
   frequent slot.
3. Existing period/lifecycle synchronization protects the pointer and the
   marked data. Updates, pointer rebinding and destruction must invalidate old
   specializations and synchronize with outstanding calls and compilation.
   A static-period object must remain valid and invariant for its lifetime.
4. A surviving helper may lack its own entry metadata, as in the legacy
   pointer-folding path. The consuming entry still owns the lifecycle contract;
   missing helper metadata does not exempt the application from it.

These are trusted annotation/deployment promises, not properties inferred from
layout metadata. The fallback adds no speculative memory probe, fault catcher,
automatic lifetime tracking or runtime equality test.

## Selection rules

- Preserve the existing constant-address legacy path and strict winners.
- Explicit bounds always select strict checking. Allocator-discovered bounds,
  including a known invalid/overflowing extent, also select strict checking.
  No lightweight retry rescues a strict rejection.
- Only an unknown-bounds, witness-derived pointer candidate can use this path.
  A size-lookup callback returning zero means unknown; installing such a
  callback does not prevent fallback for static buffers it cannot describe.
- With `ejit_pointer_layout`, the complete access must lie within element zero
  and within one authorized field interval. Do not normalize the address by
  the element stride. Nonzero outer elements, including `p[free + K]` for
  nonzero K, remain unsupported without bounds.
- Integer poison/wrap checks and every intermediate flagged `inbounds` GEP
  remain checked. The guaranteed first-element extent is enough for these
  checks; it is not published as an allocation size. A chain that leaves this
  extent and comes back is conservatively rejected.
- Null bases, malformed layouts, unauthorized/widened fields, volatile/atomic
  loads and ambiguous eligible registrations remain rejected. An unbounded
  alias cannot rescue strict rejection of the same pointer base.
- Pure constant `p[k]` accesses gain no new permission. Existing legacy folds
  and PR252's bounded/discovered folds keep their existing behavior.

Legacy offset-only metadata can authorize an exact field start only when an
aggregate-rooted GEP still describes the first element and its field width.
The shared field-size walker checks the innermost field, not a later pointer
cast's apparent type. Malformed present layouts never fall back to offsets.
Opaque byte chains with neither a layout nor recoverable aggregate type remain
unfolded. This intentionally narrows PR254 v2's unrestricted offset-only
fallback: a bare offset is not a width bound.

## Diagnostics and acceptance

Verbose diagnostics report `lightweight-element-zero-witness` on a selected
fallback. Rejection reasons include `witness-beyond-element-zero`,
`legacy-witness-missing-type`, `invalid-gep-witness`,
`layout-or-field-unauthorized`, `strict-pointer-alias` and
`ambiguous-lightweight-registration`. Existing may-const and branch audits stay
enabled. Successful optimization does not prove the deployment contract.

Tests cover typed/nested and flattened `slot % 5 / % 10` accesses, including
`inttoptr(base + field) + ((slot % N) << 7)`, LE/BE IR, L2/L3,
Baseline/Instrumented/PGOUse, live stores, legacy typed offsets and negative
authorization cases. A real native online-PGO test uses no allocation-size
callback or bounds registration, reaches 64 T1 executions, publishes T2,
deactivates, changes configuration and repeats.

Board acceptance still requires rebuilding the product runtime, running the
same business workload with the established worker/owner initialization,
checking final T2 IR and audit counts, and comparing outputs before and after
a synchronized configuration update. Host tests and BE object compilation are
not multicore SRE/SDK acceptance. Enabling previously dormant annotations can
expose incorrect business promises; additional folds must be audited rather
than assuming that only previously reported sites will change.

## C-source reproduction

`ejit_test/ejit_pointer_witness_source_repro.c` is compiler-only input, not an
SRE shell test. It deliberately declares, rather than supplies, the external
storage and sink. It has no `main` or board initialization entry. The three
entries cover direct access, taking `&g_1[cell].g_2[slot % N]` then reading,
and taking the same address through an always-inline reader. Each sink also
receives the real selected address, so removing the load must not redirect
the pointer to slot zero. Slot stride is 128 bytes and the marked byte is at
offset 31, matching the reported modulo/shift addressing shape.

The C-source validation feeds the actual frontend embedded bitcode to the
original PR252 field pass and the PR256 field pass with identical remaining
pipeline components. It does not replace function bodies or synthesize field
metadata. Set up two readable Cell objects; flags in cell0 are all one and
flags in cell1 all zero. Register only the normal `g_1` period pointer slot,
without pointer bounds or an allocation lookup for the positive case.

Acceptance matrix (three entries x five cases x two optimization levels x
three pipeline tiers x two slot counts x two byte orders = 360 cases):

| Case | Expected final marked load |
| --- | --- |
| cell0, unknown bounds, free_dim present | Removed, sink flag is constant 1 |
| cell1, unknown bounds | Retained |
| cell1, explicit complete bounds | Removed, sink flag is constant 0 |
| cell0, free_dim absent | Retained |
| cell0, pointer registration absent | Retained |

Both `%5` and `%10`, L2/L3 and Baseline/Instrumented/PGOUse configurations
were checked. PR252 retains the load in all 72 unknown-bounds positive
combinations; PR256 passes all 360 expected outcomes. The real slot argument
and its dependent selected address remain live in every case. These tier
configuration checks do not themselves run a live PGO profile; real online
64-entry PGO and renewal validation is separate, as described above.

The LE matrix uses the frontend pointer load unchanged. BE checks run on an
LE host only after explicit root-pointer materialization; the frontend GEP,
load and metadata remain intact. This is not a BE runtime or board execution.

In the positive final IR, the flag load disappears and the call becomes
`repro_sink(1, slot, selected)`. The `urem`/GEP may remain because `selected`
must still identify the real slot. A surviving `urem` alone is not evidence
that may_const folding failed. Product sites still require their real
metadata and rejection reason to establish the same diagnosis.
