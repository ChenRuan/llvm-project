# EJIT Bitcode Repository and SCC Manifest

**Status:** experimental  
**Version:** 1  
**Applies to:** `-fejit-cross-jit-helpers`

## 1. Problem

The original cross-TU helper mode emits one self-contained bitcode module for
every `ejit_entry`. If several entries reach the same large helper closure, the
final image stores that closure once per entry:

```text
entry_a payload = entry_a + helper_b + helper_c
entry_d payload = entry_d + helper_b + helper_c
```

Forcing all helpers to inline does not solve the storage problem. It duplicates
the helper instructions at each call site and can make both bitcode and
generated code larger.

The repository format separates template storage from specialization:

* the linked image stores every reachable helper definition once;
* each runtime compilation gets an independent transient module;
* IPSCCP may therefore produce different helper versions for different entry
  dimension values without duplicating the original bitcode payload.

## 2. Linked-image format

`ld.lld --ejit-cross-jit-helpers` merges the `.ejit_cross` inputs, keeps the
union of all entry closures, and serializes that module once as:

```text
@__ejit_bitcode_repository
```

Every `EJIT_REG_BITCODE` record still has the existing ABI:

```text
{ type, entryName, reserved, data, size }
```

The records for all entries point to the same repository `data` and `size`.
There is no runtime registry ABI change and legacy per-entry payloads remain
valid.

The repository module contains three named metadata records:

```llvm
!ejit.repository.version = !{!0}
!ejit.repository.sccs = !{!1, !2, ...}
!ejit.repository.entries = !{!10, !11, ...}

!0 = !{i32 1}
!1 = !{i32 0, !"helper_b", !"helper_c"}
!10 = !{!"entry_a", i32 0, i32 3}
```

`ejit.repository.sccs` records the function call-graph SCCs. An SCC is the
smallest unit that must remain together for recursion. An entry record contains
the sorted SCC IDs in its transitive closure.

Global initializers, aliases, ifunc resolvers, constant expressions and
function-pointer tables participate in reference collection. Runtime-supplied
indirect targets cannot be inferred and remain external runtime dependencies.

## 3. Link-time algorithm

1. Merge selected `.ejit_cross` modules.
2. Find every `ejit_entry`.
3. Compute the conservative union closure and remove unrelated definitions.
4. Apply `AlwaysInlinerPass`; ordinary helpers remain as functions.
5. Build direct function-reference edges, including references reached through
   kept global initializers.
6. Run Tarjan SCC decomposition.
7. Compute each entry's transitive closure and encode its sorted SCC IDs.
8. Externalize mutable process globals exactly as the legacy extractor does.
9. Serialize the repository once.
10. Emit one normal bitcode registry record per entry, all referring to that
    repository payload.

The normal `-fejit-cross-inline` policy is unchanged and continues to emit
independent per-entry payloads.

## 4. Runtime selection

`EJitOrcEngine::loadBitcodeModule` parses the registered payload and calls
`prepareRepositoryForEntry` before symbol collection or optimization.

For a repository module:

1. Validate the format version and the requested entry record.
2. Validate all referenced SCC IDs and unique function membership.
3. Keep the requested entry externally visible.
4. Internalize every other definition, including other entries and helpers.
5. Run `GlobalDCE`; the requested entry becomes the sole root and reconstructs
   its function/global closure.
6. Verify every surviving function belongs to an SCC listed by the entry
   manifest.
7. Run the unchanged EJIT specialization pipeline:
   period-index substitution, may-const folding, IPSCCP and the selected
   function simplification pipeline.

Consequently, `entry_a(x=1)` and `entry_a(x=2)` parse the same repository
template but optimize independent transient helper copies. Symbols cannot mix
because every specialization retains the existing isolated JITDylib.

Legacy modules without `!ejit.repository.version` bypass repository selection
unchanged.

## 5. Size and cost model

Let `E` be the entry count, `H` the shared helper bitcode size and `A_i` each
entry's unique bitcode:

```text
per-entry closure:  sum(A_i) + E * H
repository:         sum(A_i) + H + manifest
```

The saving grows with the number of entries sharing a large closure. The
trade-off is worker-side parsing and `GlobalDCE` over the union repository for
each new specialization. Compilation is asynchronous, so this design
deliberately trades worker CPU and temporary memory for linked-image size.

The first implementation stores one repository. If parsing a very large union
becomes expensive, the compatible next step is one repository payload per
connected group of SCCs, while preserving the same metadata format.

## 6. Correctness invariants

* Repository metadata is versioned and malformed manifests fail compilation.
* The manifest is not trusted as a deletion instruction: `GlobalDCE` derives
  liveness from the selected entry; the manifest validates the result.
* Mutable globals remain owned by the AOT image.
* Helper definitions become local before IPSCCP, so constants can propagate
  through all statically known call sites.
* The selected entry remains externally visible for ORC lookup.
* Existing registry ABI, funcIndex assignment, cache keys, taskpool state and
  code-pool publication are unchanged.

## 7. Limitations

* Different specialization values may still require different helper machine
  code. This design deduplicates bitcode templates, not inherently distinct
  generated code.
* The whole repository is parsed for each cache miss. SCC-sharded physical
  payloads are a future optimization.
* Truly dynamic indirect callees cannot be added to a static closure unless a
  module-owned function table exposes their candidates.
* Repository mode currently follows the JIT-local-helper option. It is
  experimental until linked-image size and worker compilation latency are
  measured on the production SRE image.

## 8. Required verification

* Link two entries sharing a nested or recursive helper closure.
* Confirm one `__ejit_bitcode_repository` symbol and one helper definition in
  saved repository IR.
* Confirm both registry entries use the shared payload.
* Select each entry independently and confirm the unrelated closure is removed.
* Confirm different dimension values specialize the same helper template
  independently.
* Compare `.ejit_bitcode`/ELF size and worker compile time against per-entry
  JIT-helper payloads on representative production inputs.

## 9. Prototype result

The initial AArch64 test links two entries that share an ordinary nested helper
chain. Both versions use the same input objects:

```text
legacy per-entry payloads: 4732 bytes total
repository payload:        2996 bytes
reduction:                  36.7%
```

The final ELF contains one `__ejit_bitcode_repository` symbol. Its saved IR
contains both entries, one copy of each helper, and all three repository
metadata records. Runtime tests select each entry independently, remove the
other closure, retain a recursive helper SCC, reject an unknown SCC, and
specialize the same helper template to two different constant results (`10`
and `20`).

This small result demonstrates structural deduplication, not a production size
forecast. Production benefit depends on how much helper closure is shared
between entries. Worker compile latency still includes parsing the whole
repository and must be measured on the SRE workload.
