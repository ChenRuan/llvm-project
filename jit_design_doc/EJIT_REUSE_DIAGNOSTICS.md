# Why two requests did not share code

These diagnostics supplement PR230's exact identity checks. They do not relax
those checks, force a merge, or count a normal unequal-value split as a failure.
They run on classification/compilation paths, never per cache-hit execution.
Build with `EJIT_DIAG_ENABLE` and the existing representative-sharing options;
keep `EJIT_SRE_PGO_BRANCH_AUDIT=ON` for validation.

## Board usage

After `test_ejit_period` initializes the worker on core 6, and BEFORE starting
the workload on core 16, run on core 6:

```text
ejit_reuse_diag_config "reuse_0", 2
```

Then run the existing workload. On core 6, inspect retained records with:

```text
ejit_reuse_diag_print
```

The C APIs are `ejit_reuse_diag_config(const char *entry, uint32_t level)`,
`ejit_reuse_diag_print(void)` and `ejit_reuse_diag_reset(void)`. Use your shell's
normal string/argument syntax. These calls must run on the actual compile-owner
core after initialization; a peer call returns `EJIT_ERR_NOT_ACTIVE`, not an
apparently empty private registry. They do not route through a cross-core
mailbox. `EJIT_PENDING` means a diagnostic operation was busy: retry later.

Level 0 disables new capture, 1 records summaries (default, filter `*`), and 2
adds paired first-difference excerpts. The filter is an exact function name or
`*`, up to 95 printable ASCII bytes; an empty filter is equivalent to `*`.
Configuration clears the old capture window. Reset clears records but preserves
the filter/level. In-flight compilation can straddle config/reset. Turning on
level 2 after the mismatch happened cannot recover uncaptured historical IR;
enable it BEFORE a fresh run. A cached existing candidate is not reclassified
just to populate diagnostics.

## Reading the record

Illustrative shape (values depend on the module):

```text
[REUSE_DIAG] seq=1 entry=reuse_0 func=0 stage=CANDIDATE reason=PREFIX_IR_DIFF action=NEW_GROUP generation=1 attempt=7 group=2 group_gen=0 peer_group=1 peer_code=0 repeats=0 truncated=1 detail=...
[REUSE_DIAG] seq=1 dim=0 instance=5 version=1
[REUSE_DIAG] seq=1 first_diff_line=12 byte=420 left(peer)=...
[REUSE_DIAG] seq=1 right(request)=...
```

`entry`, `func`, dimensions/instance/version and attempt identify the request.
Candidate `group`/`peer_group` are directory candidate IDs; `group_gen=0` means
no runtime sampling generation is being claimed at this classification point.
For a new candidate, the diagnostic peer is the lowest-ID retained candidate
with the same entry and original bitcode digest, even across hash buckets. It
is a deterministic reference, NOT a claim that every other group was compared
in the log. The classifier still performs its unchanged exact-match search.
At FINAL, `group` and `group_gen` identify the actual live group and `peer_code`
is the group's retained physical code object.

- `NEW_GROUP`: normal candidate split. Different gain values can produce a
  `PREFIX_IR_DIFF`; this is not a broken compiler or an incorrect mayconst.
- `TRY_SEPARATE_CODE`: final identity differs from the group's physical code;
  independent emission is attempted. This line is not a publication certificate.
- `ORDINARY_ROUTE` / `AOT_FALLBACK`: the sharing path was unavailable/rejected;
  inspect the reason and detail (`TARGET_MISMATCH`, `PREPARE_REJECTED`,
  `LINK_FAILED`, `NO_EMITTER`, etc.). This can require a runtime/configuration fix.

Differences distinguish source/policy/entry scope, binding symbol/address/kind,
profile schema fields, and canonical IR. Left is the retained peer identity;
right is the incoming request. IR line/byte offsets refer to CANONICAL IR, not
the C source line. A constant may already be folded into an expression: this
version does NOT promise source-level mayconst names or reconstruct original
frozen values. It reports IR differences honestly instead of labeling them
`MAYCONST_VALUE_DIFF` without provenance. For bindings, field/name and actual
compared values are available directly. Only the first difference is reported.

## Bounds and safety

There are 16 fixed-size owner-local retained records. Oldest records are evicted
when full. The same function/generation/dimensions/versions/group/peer/stage/reason/action is logged
once while retained; later events increment `repeats` and preserve the first
request identity. After eviction it can be logged again. `evicted`, `dropped`
(nonblocking contention) and `truncated` are explicit; an empty log is NOT proof
that all requests shared. At most 256 bytes per IR side and 192 bytes of detail
are retained per record; names/reasons also have fixed limits. Control characters
are replaced with spaces for one-line logs. Select one function to avoid unrelated
events evicting the interesting one. Addresses and IR may reveal application
details: enable detailed diagnostics only where such logs are appropriate.

Existing identity material is reused to calculate the difference. No extra full
module/IR copy is retained, no worker registry pointer escapes, and no shared
taskpool ABI is changed. Shell snapshot/capture uses a single try-lock: busy
diagnostics can be lost instead of waiting for that lock, and comparison results
never change a sharing decision. Printing occurs after releasing the lock;
synchronous board logging and extra comparisons still cost time and can perturb
scheduling, so avoid broad detailed capture in latency-sensitive production.
Existing general EJIT logs are not silenced by this filter. This is a bounded first-diff
debugger, not an unlimited full-IR archive or a complete mayconst provenance map.
