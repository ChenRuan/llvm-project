# PR230 Board Smoke Test (spec5)

## Bound-pointer sharing (2026-11-27)

Bound-pointer entries now participate in representative sharing; they no longer
take the unconditional `bound-pointer identity uses independent fallback` route.
No additional option, Clang change, annotation or SDK callback is required.
Keep the existing `ejit_init_representative` initialization, active dimensions,
bound-pointer descriptors and authorized constant-field annotations. This
follow-up retains shared ABI v31 and adds no shared storage or wrapper steps.

Candidate classification and final compilation receive the same bound context:
different cell/TRP objects at different addresses may share if their optimized
code and effective bindings match. Six distinct bound objects with equal
authorized fields can therefore use one representative's 64 T1 samples and one
physical T2. Unannotated fields remain runtime reads through the actual pointer.
Unequal specialized constants still form separate groups; no member executes
another member's baked-in value merely because both pointers were annotated.

Descriptors remain borrowed until their final compiler read or cancellation.
Existing queued/final-member retries retain the same live lease. After an
explicit cancellation ends that lease, a new business request must provide a
fresh descriptor; maintenance must not resurrect the old raw address. Changes
to active bound objects still require the existing deactivate/borrow-drain/
update/reactivate protocol. The usual business-execution lifetime also applies.

On core 6, use the existing `ejit_reuse_diag_print_summary`,
`ejit_reuse_diag_print`, and `ejit_taskpool_print_stats`. Successful member reuse
reports `stage=FINAL_COMPARE outcome=SHARED_REUSE reason=IDENTITY_EQUAL`.
`PREFIX_IR_DIFF` / `NEW_GROUP` remains normal when specialized values differ.
Other eligibility checks and final exact comparison are unchanged.

## Deferred admission coalescing (2026-11-27)

The current shared protocol is **v31**, superseding v30 below. Rebuild the
runtime package, regenerate lipo output, relink every participating core and
cold-reset. This runtime-only change needs no new Clang flag or wrapper ABI;
previous compatible Clang/AOT objects and embedded bitcode can be reused.
The default remains 8192 live attempts. On the tested native AArch64 build,
each record is 224 bytes and shared taskpool state is 2,281,408 bytes, 144 KiB
larger than v30. Check the final SDK shared-section placement/size.

A classified request that cannot obtain a T1 sampling slot keeps its original
lease. Repeated calls with the same function, dimension versions, generation
and bound-pointer descriptors return AOT/pending without another queue entry,
classification or token. The worker resumes it when a slot is available;
business calls need not trigger the retry. The coarse per-function queue claim
is released while waiting so another identity's T2 can progress. Representative
waiters still use their group's profile, not their own private T1. Independent
fallback still samples 64 dispatches per admitted logical version.

On core 6, `ejit_taskpool_print_stats` now also prints (illustrative numbers):

```text
admission waiting=12 resumed=8 coalesced=2000
```

- `waiting`: current classified T1 requests waiting for a sampling slot.
- `resumed`: successful worker requeues from that waiting state.
- `coalesced`: duplicate producer requests suppressed before token allocation.

These are separate from `attempts ... resumed=...`, which counts final-member
T2 requeues. Both counter sets reset at owner initialization. A growing
`coalesced` count with stable token/queue counts is expected for a hot loop.
Attempt IDs are cumulative request identities, not a count of compiled bodies
and not a strict `functions x cells` limit. Different TRPs, lifecycle versions,
queue failures and legitimate retries can create additional attempts.

The original coalescing commit `de8d39e7f` did not enable bound-pointer sharing;
the follow-up above now connects descriptor-aware candidate preparation and
the existing exact final comparison. Coalescing is still request deduplication,
not proof that distinct logical versions can share physical code.

## Live request capacity update (2026-11-26)

This earlier capacity update introduced **v30**. Rebuild/relink all cores and cold-reset;
do not mix it with v29 or a runtime built with another capacity.
`EJIT_SRE_REQUEST_ATTEMPT_CAPACITY` defaults to **8192** in CMake and the header,
and is explicit in the AArch64 BE preset. This is the number of live request
leases (including waiting members), NOT a compile concurrency setting.
200 functions x 6 cells x 6 TRPs needs 7200 logical requests. One request record
is 208 bytes on the tested AArch64 configuration; the table grows by about
1.57 MiB versus 256 slots. Check the final SDK shared-section placement/size.

Retained members now reuse their own lease when a frozen profile becomes
available. Even a completely full table can finish those members without
allocating another request. Failed queue insertion keeps the original borrow
for retry; cancellation and stale identities still reject execution. The
representative still owns exactly 64 T1 samples. Worker throttling is unchanged.

On core 6 call the existing `ejit_taskpool_print_stats` (already retained by
lipo). Its additional line has this form, with illustrative numbers:

```text
attempts live=7200/8192 highwater=7200 resumed=36 reject_full=0 reject_busy=2 reject_stale=0 reject_token=0
```

`live`/`highwater` measure current/peak occupancy; `resumed` counts successful
member requeues. Rejection counters distinguish full storage, transient lock
contention, stale lifecycle and exhausted monotonic tokens. Busy misses stay on
AOT without waiting. There is no per-miss `capacity/token` INFO flood.
These counters reset at owner initialization. The C stats struct is unchanged.

Queue slots, Ready-cache slots, group count and code-pool bytes remain separate
limits. Raising request capacity does not promise 8192 cached versions or
unlimited sharing groups. Host pressure tests cover request lifecycles and
the existing real-JIT tests cover sampling/sharing; product acceptance still
requires board validation. Existing initialization and the demo below are unchanged.

## Async representative sampling update (2026-11-26)

This earlier sampling change introduced v29; the latest update above now
requires v31. Rebuild the runtime package and link a fresh image for all
participating cores; reset before testing. No new SDK hook, public entry point or demo source
change is required for this update. Keep `ejit_init_representative` and the
existing audit-enabled configuration.

The representative's Tier-1 grants no longer wait for worker mailbox replies.
The worker observes the shared 64-dispatch quota and owns Tier-2 retries even
after business calls stop. Contended lookups return AOT for that call without
consuming a sample or requesting a duplicate compile. Profile capture still
waits for the last granted Tier-1 execution to leave. See the
[async sampling validation record](EJIT_REPRESENTATIVE_ASYNC_SAMPLING.md).

The existing core 6 -> core 16 -> core 6 sequence and final PASS conditions
below are unchanged. Host regressions and BE object compilation do not certify
product watchdog behavior; final board acceptance remains necessary.

The follow-up bounded-publication fix also removes a queue-starvation path:
once a T2 links, automatic publication waits only for the queue prefix already
present at that moment. New candidate requests cannot extend that boundary.
The INFO log `PGO Tier-2 publish boundary reached: auto-publishing ...
queued_after=N` may legitimately show N greater than zero. Existing T1 sampling,
physical-identity checks, worker throttling and explicit-flush failure policy
are unchanged. This change adds no shared-protocol fields or new options.

Source: `examples/ejit_reuse_sre_test.c`, a single header-free C file.
Replace the previous file defining `test_ejit_period`; do not link both demos.
The companion `ejit_reuse_host_check.c` is a host-only mock, NOT a board source.
For optional mismatch reasons and paired bounded IR excerpts on core 6, see
[reuse diagnostics](EJIT_REUSE_DIAGNOSTICS.md). Capture is enabled by default;
call `ejit_reuse_diag_print_summary` for reason counts or
`ejit_reuse_diag_print` for individual differences on core 6 after testing.
The smoke source need not change.

## Default lightweight mode

The default now uses two functions (`reuse_0`, `reuse_1`), six cells each:
12 logical T2 entries, three representative groups/physical T2 objects, and
3 x 64 = 192 T1 dispatches. It still checks same-value sharing, unequal cell 5,
live loads/stores, borrow fencing and rejoining the original code after update.
Expected shared reuse after renewal is 11. This is a smoke test, not the old
20-function pressure test; it does not prove the same capacity coverage.

No extra flags are needed. For the original 20-function/120-entry workload,
compile this source with `-DREUSE_STRESS=1`. Whole IR/ASM capture and printing
are now off in BOTH modes; add `-DREUSE_DUMP_IR=1` when needed. Bounded reuse
difference diagnostics remain available independently via
`ejit_reuse_diag_print` on core 6. Update the demo object as well as the runtime
package, then link a fresh board image. Shell arguments do not select the mode.

The PGO quota remains 64; worker throttling and sample
lookup/execute/release/yield discipline are unchanged. Reduced workload and
omitting full dumps should shorten the run, but no board timing is promised.
SRE dump-loop throttling applies to bulk dump lines, not every ordinary log.
The new lipo GC roots must be used during `gc-merge`, before `merge.ld`;
verify `ejit_reuse_diag_print_summary` and
`ejit_reuse_diag_print/config/reset` exist in the final runtime object.

## Build And Startup

- Use the EJIT Clang and runtime from the current PR230 spec5 branch, including
  the worker fairness fix `5b8288a97`. Stock Clang is rejected. Replace the
  board example with the current source too; a runtime-only rebuild retains
  the old slow first-sweep scheduling or larger workload in the application.
- Use the existing spec5 AOT annotation/bitcode/static-registration build flow.
  Keep generated wrappers and registration constructors in the linked image.
- Runtime: async shared taskpool, fixed worker core 6, shared code pointers,
  normal online PGO, `EJIT_STATS_ENABLE`, and diagnostic/dump support enabled.
  Keep the representative quota at its current 64-dispatch setting.
- Keep `EJIT_SRE_PGO_BRANCH_AUDIT` enabled. Online PGO plus audit diagnostics
  is supported; it is not audit-only mode. Rebuild the runtime with the PR230
  audit-admission fix if initialization rejects this valid combination with
  `representative sharing requires Async + normal online PGO`. No example or
  public config layout change is needed for this fix.
- This example opts in through `ejit_init_representative`, not `ejit_init_pgo`.
  It does not change the product's default-off policy. Do not initialize EJIT
  elsewhere before running this example.
- Use the same 64-bit image/ABI on cores 6 and 16. Map `.mc_shared` coherently at
  the same VA on both cores. All configuration rows and test result state in
  that section must be shared, not per-core copies. Adjust `REUSE_SHARED` only
  to the product's equivalent shared section.
- Both participating cores run `call_init_array_functions()` once before their
  local EJIT init. Define `REUSE_RUN_INIT_ARRAY=0` only when platform startup
  already ran constructors on BOTH cores. Repeated shell calls and the print
  command never replay constructors or reinitialize EJIT.
- Run after board reset in an otherwise idle test image, from schedulable shell
  tasks. There must be no other callers or configuration writers. Provision
  cache/code/data capacity for 12 live logical entries, 3 representative T1s,
  3 physical T2 objects and their retained profiles (stress: 120/21/21). Old T1 allocation need
  not be reclaimed. This is larger than the previous six-version IR sample.

## Shell Sequence

```text
core[6]-> test_ejit_period
          [REUSE230] WORKER_READY
core[6]-> core 16
core[16]-> test_ejit_period
           [REUSE230] SAMPLED entry=0 cell=0 calls=65
           ... (3 representative identities; stress mode: 21)
           [REUSE230] PHASE_INITIAL checked ...
           [REUSE230] PHASE_UPDATE checked ...
           [REUSE230] OWNER_DONE ...
core[16]-> core 6
core[6]-> test_ejit_reuse_print
          [REUSE230] PASS ...
```

All four optional shell arguments are ignored. There is no Baseline/PGO mode
argument: this is always representative online PGO. A timeout or failure needs
a reset before another run; printing partial state is safe after failure.

## Full Stress Mode Details (`REUSE_STRESS=1`)

Twenty entries (`reuse_0` through `reuse_19`) each use six cells, with TRP 1 as
a second live dimension. Each entry has its own period array. Only `gain` is
may_const; ordinary loads/stores still use the actual cell and TRP arguments.
Every executed call checks the return value and all cells' live fields, so an
accidentally frozen write address cannot pass just because the gain is equal.

1. Initially five cells of `reuse_0` have gain 3; cell 5 has gain 17. All six
   cells match within each other entry. Expect 21 candidate groups, 21 real
   representative sessions, 21 x 64 = 1344 T1 dispatches, 21 physical T2 objects,
   and 120 logical T2 entries. The unequal cell must NOT share the equal group's
   pointer, and its final pointer must differ from its observed T1 pointer.
2. The owner first drives each of the 21 known representative identities to
   65 successful calls (64 T1 samples followed by a T2 call), one identity at
   a time. Each call performs a fresh lookup, executes the grant immediately,
   releases its read token and yields between attempts. Pending calls do not
   count. It then polls ALL 120 identities to verify final publication/reuse.
   This avoids leaving a newly published T1 idle while worker scheduling and
   logging process the rest of a 120-identity first sweep.
   Admission deferral is retried; actual queue/compile/publish failures are
   rejected separately. The same generated wrappers are exercised after
   convergence. This diagnostic smoke is neither a hot-path benchmark nor a
   concurrent-admission stress test. `SAMPLED` is progress, not final acceptance.
3. The owner stops business calls, deactivates cell 5 and waits for the exact
   compiler-source borrow fence. Only `EJIT_OK` permits changing gain 17 to 3.
   A pending/error/timeout never authorizes mutation. The owner reactivates the
   cell and continuously drives all identities again. All 20 renewed cell-5
   entries must reuse their original equal-group T2 pointers; other cells must
   retain those pointers. No new representative samples or physical T2 objects
   are expected. The retained old unequal T2 is still counted, so physical
   objects remain 21, not 20; shared reuse normally totals 119 after renewal.
4. Final acceptance runs on core 6, where the compiler's group registry lives.
   It checks the real physical-code and sampling counters plus drained work.
   `OWNER_DONE` on core 16 alone is NOT acceptance. The printed cache should
   show `tier2=120`, no collecting T1, and all 120 final entries
   `post_publish_seen=yes`. No re-election, schema rejection, independent
   fallback object, cancelled waiter, compile or publish failure is expected.

When `REUSE_DUMP_IR=1`, the dump captures only `reuse_0`. Inspect the final T2 entry and
module: the gain load should disappear, but the live load/store and real
cell/TRP indexing must remain. The recorded module is the latest capture, not
an archive of all 120 versions; use the cache lines and saved before/after
pointer matrices to compare the whole run.

## Verification Boundary

The runtime no-progress timeout is expressed in `ejit_taskpool_trace_now()`
units: nanoseconds on the host, hardware cycles on SRE. The default 5,000,000,000
must NOT be assumed to mean five seconds on the board. This smoke does not
change/disable the timeout or increase the sample quota; product timeout tuning
still requires the platform clock frequency and the business call cadence.

`ejit_reuse_host_check.c` checks arithmetic, startup order, admission retry,
read-token balance, all selected identities (12 or 120), mutation fences, failure paths, and
read-only printing with a mock. It also builds against the real public header
to check the header-free declarations. A deterministic slow-request clock
shows the old first sweep expiring profiles before their first sample, while
focused sampling completes with no expiration and exactly 192 T1 dispatches
(stress mode: 1344).
These are simulated time units, not a measured SRE cycle frequency. It is not
an AOT/JIT test.

The real runtime regression
`RoundRobinTwentyEntriesRejoinSharedTier2AfterBorrowFence` independently runs
the separate continuous round-robin scheduling and renewal pattern through ORC, online PGO,
the production shared cache and actual generated host code. The native host
tests do not certify AArch64 BE code generation, SRE mappings/cache maintenance,
or board execution. Those require the three board commands above and their
resulting logs. This example does not cover `bound_ptr`, PR231 or every product
may_const site.
