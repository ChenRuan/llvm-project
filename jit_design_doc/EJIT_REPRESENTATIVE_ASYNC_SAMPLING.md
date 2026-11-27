# Representative T1: asynchronous sampling and retry validation

Date: 2026-11-26. Branch: `codex/pr230-on-pr274`.
Parent: `fdba07ec7afcb1bffd153522d0a9d1f1427558a6`.

## Scope

The parent queues cold peer admission asynchronously but still waits for an
owner reply on each actual representative T1 grant. This update removes that
remaining business-to-worker handshake. It does not disable representative
sharing, lower the 64-call quota, or substitute switch-case merging.

## Runtime contract

1. The published slot owns the exact attempt/generation and cumulative quota.
   A business lookup tries observation exclusion once. Contention returns AOT
   for this call, with no sample charged and no execution token retained.
2. A granted T1 call increments the quota once and pins its execution token
   before releasing observation exclusion. The default limit remains 64.
   Closed-quota calls use AOT until T2 publication; they do not extend T1.
3. Worker maintenance reads the committed high-water mark and reconciles the
   matching group session. No sampling acknowledgement mailbox is needed.
   The final profile is captured only after all actual T1 executions leave.
4. The worker owns queue-full and T2 compilation retries without requiring
   another business call. Monitoring continues through frozen-bundle and
   linked-code states until successful shared-cache publication. Maintenance
   scans the retained bindings once, skipping completed entries; it does not
   perform a nested search over every retained group and binding each tick.
5. Retries retain representative identity. A retry after physical linking may
   reuse the same exact code identity; it must not create another physical
   object or silently become an independent member.
6. Idle cancellation checks the count again under slot exclusion, preventing
   a stale timeout from cancelling newly progressing sampling. Existing
   generation, deactivation, shutdown and reader-drain checks remain active.
7. A contended cache scan, including an apparent miss during publication, is
   not evidence that a version needs compiling. Representative callers return
   AOT for that call instead of admitting a duplicate candidate.

Independent bound-pointer PGO retains its existing behavior and does not send
representative acknowledgements. Worker throttling and idle yields remain.
The explicit legacy mailbox APIs remain available; production representative
sampling does not use them. There is no new public C API or lipo root.

## Regression evidence

The paused-worker test fails on the parent: a subsequent representative T1
grant cannot complete normally without the worker acknowledgement. This is a
reproduced blocking path, not proof of the entire product watchdog root cause.

The final patch was tested on native AArch64 Linux, with fresh EJIT sources,
real ORC/JITLink generated-code execution, shared code pointers, NO_RECLAIM,
4K batched publication, branch audit and value profiling enabled. Non-EJIT
LLVM dependencies were reused. Builds were serialized and limited to `-j8`.

| Check | Result |
| --- | --- |
| Runtime/pool/PGO/identity/folding regression executable | 392/392, five fresh processes |
| New asynchronous and retry cases, randomized order | 6/6, twenty fresh seeds |
| Real worker paused throughout remaining T1 grants | Completes without owner replies |
| Quota and last execution | Exactly 64; no bundle before last token release |
| Queue full; failure before capture, after bundle, after link | Worker retries without new business calls |
| Final representative and member pointers | Same T2; exactly one physical object |
| Observation/attempt-lock contention; occupied mailboxes | No synchronous representative wait or quota leak |
| Concurrent grants; stale timeout; contended publication | Exact quota; no stale cancellation or duplicate admission |
| AArch64 BE formal-config objects | CompileDriver and SharedTaskPool compile; MSB ELF verified |

The full suite also covers independent bound-pointer PGO, unequal identities,
renewal, deactivation, repeated lifecycle churn and existing worker-throttle
heartbeat checks. Two pre-existing disabled tests were not executed. During
repetition, an extra reuse exposed a contended-lookup false miss; a deterministic
regression was added and the exact reuse-count assertion was retained. A test
setup race was also fixed: retry nonblocking admission until the real worker
actually reaches its pause gate, before asserting paused-worker behavior.

New focused tests:

```text
EJitCrossCoreRuntime.RepresentativeSamplingDoesNotWaitForWorkerAndRetriesWithoutProducer
EJitCrossCoreRuntime.RepresentativeRetriesAfterBundleWithoutProducer
EJitCrossCoreRuntime.RepresentativeRetriesAfterLinkWithoutProducer
AsyncRepresentativeDispatchTest.BusyObservationAndAttemptLocksNeverMakeBusinessWait
AsyncRepresentativeDispatchTest.TimeoutCannotCancelNewSamplingProgress
AsyncRepresentativeDispatchTest.PublicationContentionIsTerminalInsteadOfNewCompilation
```

Server evidence is retained under
`ejit-local-support/pr230-on-pr274-20261125/`:
`async-t1-red.log`, `async-t1-contention-red.log`,
`async-t1-release-final-{1..5}.log/xml`,
`async-t1-release-final-races-{1..20}.log`, and `async-t1-be-final.log`.

## Deployment boundary

Protocol v29 rejects mixed peers. Rebuild and relink one consistent runtime
image, then cold-reset all participating cores. Keep the existing
`ejit_init_representative` configuration, audit ON, and the documented
[6 -> 16 -> 6 smoke test](EJIT_CODE_REUSE_BOARD.md).

BE object compilation is not final SDK linking. These host tests do not prove
product memory mappings, SDK scheduling latency or watchdog recovery. Product
board acceptance and a before/after watchdog trace remain outstanding.

The subsequent live-request-capacity update requires v30 instead; see the
current deployment instructions in `EJIT_CODE_REUSE_BOARD.md`.

## Follow-up: publication with continuous candidate traffic

Parent `ff8a57d1f00a41a67c39c38521ad4ac4cfe3e50a` still required the entire
compile queue to become empty before automatically publishing linked T2 code.
A new regression reproduces the resulting starvation through real pool paths:
an independent T1 in representative mode consumes all 64 grants, T2 links,
and another core continuously queues candidates while the PGO admission slot
remains occupied. T2 never publishes and the next sampling admission cannot
proceed. A later producer lull can make this look like unusually slow warmup.
This establishes a runtime defect; it does not prove every delay in a product
log has this single cause. The test uses mock generated-code readiness/flush
callbacks; the broader suites also execute real ORC/JITLink code.

The owner now snapshots the enqueue position when the first T2 in the batch
links. Work already queued runs first, with ordinary per-item throttling.
Once the consumer reaches that fixed boundary, publication runs before newer
requests. Additional linked T2s do not move the boundary. The bounded queue
distance permits a wrap-aware uint32 position comparison. The snapshot is
owner-private; shared ABI v29 and all public entry points remain unchanged.
Code remains RW/NX and invisible to dispatch until ordinary sealing and cache
publication succeed. Existing explicit retry on batch-flush failure is retained.

Regression: `SharedTaskPoolTest.BatchPgoPublishesClosedQuotaDespiteContinuousPeerCandidates`.
It fails on the parent (zero flushes, one pending linked T2, no completed PGO)
and passes with the fix while the queue remains nonempty. It checks the frozen
64-call boundary and that PGO admission had actually deferred another request.

Validation: 393/393 tests in five fresh processes; four focused publication,
queued-T1 ordering, worker-throttle and flush-failure cases repeated 100 times
with shuffled order; CompileDriver and SharedTaskPool BE objects compiled.
Two pre-existing disabled tests remain unexecuted. Evidence under the same
server directory: `publish-starvation-red.log`,
`publish-starvation-final-{1..5}.log/xml`, `publish-starvation-stress.log`, and
`publish-starvation-be.log`. No product-board validation was performed here.

## Follow-up: retained request capacity and full-table progress

On parent `7b83c089b1ee7733a8f09ff6a6996c58b70a359e`, a queue/classification
test retaining 200 x 6 x 6 member identities failed at identity 256: the
257th member could not allocate a lease. Waiting members consume live slots
even though they have no private T1. A separate progress problem was that
owner maintenance allocated a second request before waking a retained member.

The default is now 8192 live slots, configurable in CMake (1..65536). A rotating
free-slot cursor avoids repeatedly scanning the occupied prefix. Full-table
and busy-lock misses do not burn attempt tokens. Owner final-member work
rearms its original borrow with queue/publication ownership, not another
allocation or sampling session. Ring rejection rolls back just the rearm,
preserving the source lease. Identity/generation/version/cancellation checks
remain in force. No-bundle re-election still goes through ordinary admission;
it must not be confused with final T2 resumption. The old per-miss INFO line
is replaced by snapshot counters in `ejit_taskpool_print_stats`.

Shared ABI v30 includes pressure counters. Cancellation snapshots use private
dynamic storage rather than an array proportional to capacity on the task
stack. Native AArch64 measured 208 bytes per attempt and 2,133,952 bytes for
the complete shared state in the test configuration. The request-array growth
is 1,650,688 bytes; it does not allocate more executable code pools.

The pressure fixture uses the real peer queue, classifier hook and attempt
lifecycle with mock compilation, not 7200 ORC compilations. It covers 7200
simultaneous retained identities, a completely full 8192-slot table, repeated
overflow misses, member resumption, duplicate wake, failed enqueue rollback,
stale/wrong identities, deactivation, compile failure and full shutdown cleanup.
Existing real-ORC tests separately verify the exact 64-call quota, frozen
profile, shared final function pointer, retries and repeated group re-election.
Request capacity is not group capacity, cache capacity or code-pool capacity.

Validation (audit ON, serialized builds, at most `-j8`):

| Check | Result |
| --- | --- |
| Final native runtime/pool/PGO/folding suite | 398/398, five fresh processes; two pre-existing disabled tests |
| Capacity, async observation and bounded-publication cases | 10/10, shuffled 50 repeats |
| Existing 128-generation re-election/borrow regression | 20 repeats after fixing the no-bundle routing distinction |
| BE Linux ABI/compiler model | SharedTaskPool, CompileDriver and Runtime compile; MSB AArch64 objects verified |
| CMake capacity validation | 0, 65537 and nonnumeric input rejected; 8192 accepted |
| lipo GC and merge roots | Real AArch64 object/archive test passes, including `ejit_taskpool_print_stats`; bound-pointer root test passes |

Evidence is in the same server support directory:
`attempt-capacity-red.log`, `attempt-capacity-release-{1..5}.log/xml`,
`attempt-capacity-release-stress.log`, `attempt-capacity-reelection.log`,
`attempt-capacity-be.log`, `attempt-capacity-config-*.log`,
`attempt-capacity-lipo.log`, and `attempt-capacity-lipo-real.log`.
An intermediate full-suite run caught the no-bundle re-election regression;
it was corrected before the five final clean runs. The BE model uses real GNU
target inputs but is not a final SRE SDK link or board validation.

## Follow-up: deferred candidate hot loops (2026-11-27)

On parent `f9840efda`, a bound-pointer candidate that selected independent T1
could fail sampling admission while another entry held the slot. Its attempt
was then retired, so the next identical business call enqueued classification
again. A red regression reproduced this without timing assumptions.

The worker now parks that attempt with its source borrow and exact descriptor
identity intact. A per-function chain under the attempt lock lets producer
calls coalesce only matching function/dimensions/versions/generation and
`{argIndex, rawPtr, size}` descriptors. Peer admission still uses one try-lock;
it never waits for a worker reply. No source memory is dereferenced by lookup,
queueing or retry. Owner-origin calls also coalesce, avoiding competing T1
attempts for a parked representative. Maintenance and final-member resumption
explicitly retain their existing lifecycle transitions.

Worker maintenance scans at most 64 attempt slots per step, resumes an eligible
request with its original token, and preserves the borrow on queue-full
rollback. It releases the coarse function queue claim while parked. Stale
versions, deactivation, cancellation, shutdown and failed compilation settle
their exact leases. No ordinary heap source or unrelated binding is silently
treated as equivalent. The worker also refreshes cancelled representatives
without another producer miss, including after their diagnostic tombstone
rotates out. Exactly 64 T1 dispatches and final precise sharing checks remain.

This is shared protocol v31 (record 224 bytes; state 2,281,408 bytes on the
tested AArch64 configuration). All cores must rebuild/relink and cold-reset.
Public C APIs, Clang metadata/wrappers, audit settings and worker throttling are
unchanged. The existing lipo-retained `ejit_taskpool_print_stats` reports current
admission waiters, worker resumptions and suppressed duplicates. Bound-pointer
candidate eligibility itself is unchanged; enabling sharing is separate work.

Regression coverage includes hot-loop peer and owner calls; distinct bound
addresses/sizes/parameter indices; six independently sampled cell identities;
queue insertion failure and retry; compilation failure and fresh retry;
deactivation/version change; 7200 retained identities and full-table member
resumption; cancellation/re-election without producer retries; and the real
20-entry shared-profile/T2 flow. Host coverage does not replace the product
watchdog, final SDK linkage or board acceptance.

Validation for this follow-up:

| Check | Result |
| --- | --- |
| Native complete runtime/pool/PGO/folding suite | 405/405 in five fresh processes; 406/406 after adding the final compile-failure retry test; two pre-existing disabled cases |
| Deferred admission and retained-capacity regressions | 13/13, shuffled 50 repeats |
| Real 128-generation cancellation/re-election regression | 20 repeats; initial re-election now requires no producer retry |
| BE Linux ABI/compiler model | All three affected implementation objects compile as MSB AArch64, audit/diagnostics ON, capacity 8192 |
| lipo roots | Bound-pointer root and real AArch64 archive GC tests pass, explicitly including `ejit_taskpool_print_stats` |

Evidence in the same server support directory: `admission-coalesce-red.log`,
`admission-coalesce-final-{1..5}.log/xml`, `admission-coalesce-release.log/xml`,
`admission-coalesce-stress.log`, `admission-coalesce-reelection.log`,
`admission-coalesce-be.log`, and `admission-coalesce-lipo-{bound,gc}.log`.
Intermediate full-suite failures exposed owner-origin duplicate attempts and
missing worker-driven cancelled-representative refresh; both were fixed before
the final runs. The BE build is not a final SRE SDK link or board test.

## Follow-up: bound-pointer candidate eligibility (2026-11-27)

By product request, remove the blanket independent route for entries with bound
pointer formals/descriptors. Candidate and final compilation now use one context
builder, preserving range validation and exact formal matching. Runtime views
feed the existing preserved-dimension/authorized-load pipeline. Raw descriptor
addresses are not added as an unconditional group discriminator: differing
addresses may produce identical code, while embedded constants, observable
addresses and effective symbol bindings still participate in exact identity.
Unannotated reads remain dynamic. No equality is fabricated and no differing
final IR is force-shared. Existing wrapper and annotation contracts are unchanged.

Retained member T2 work uses its original descriptor-bearing lease. Fresh
owner-origin wakeups also carry the supplied descriptors. Group retirement
ends old bound borrows and removes their automatic reclassification triggers;
the next business request supplies a fresh descriptor instead of maintenance
reopening a completed borrow. A failed/missing retained bound lease similarly
does not fall through to descriptor-less or stale-pointer compilation. Normal
T2 queue/compiler retries retain live leases and require no producer retry.

Shared ABI remains v31; there is no new public API, flag, SDK hook, lipo root,
shared-memory allocation, or business hit-path work. Rebuild the runtime and
relink the package/image; compatible AOT/Clang output can be reused.

Real ORC/PGO cross-core tests cover six different object addresses sharing one
T2, unequal authorized fields forming two groups, unannotated differing values
remaining dynamic under shared code, configuration rebind, cancellation with
fresh descriptor re-entry, and strict 64-sample retries before/after bundle
freeze. A pool regression separately checks both bound descriptors survive an
owner-origin T2 wakeup. The old runtime fails the new bound-group acceptance
test; the previous fallback-only test is updated to require a real group.

Invalid descriptor/formal combinations are rejected before candidate admission,
release their request borrow, and record `CompileFailure` instead of an empty
terminal reason. A regression checks rejection and subsequent valid re-entry.

Validation (serialized builds, at most `-j8`, audit/diagnostics retained):

| Check | Result |
| --- | --- |
| Original runtime with bound-group acceptance test | Fails as expected: no representative group |
| Complete native runtime/pool/PGO/folding suite | 414/414 on the final patch; two pre-existing disabled cases |
| Earlier full-suite repetition | 413/413 in five fresh processes, before the invalid-formal diagnostic regression/fix |
| Final bound-pointer and owner-wakeup regressions | 9/9, shuffled 50 repeats |
| BE Linux ABI/compiler model | Both changed implementation objects compile as MSB AArch64; no undefined `wmemchr` |
| lipo roots | Bound-pointer roots and real AArch64 archive GC checks pass, including `ejit_taskpool_print_stats` |

Evidence in the same server support directory: `bound-sharing-red.log`,
`bound-sharing-release-{1..5}.log/xml`, `bound-sharing-final2.log/xml`,
`bound-sharing-final2-stress.log`, `bound-sharing-be.log`,
`bound-sharing-be-undefined.txt`, and `bound-sharing-lipo-{bound,gc}.log`.
An intermediate invalid-formal test exposed the missing terminal failure reason;
that diagnostic was corrected before the final full and repeated focused runs.
These are server regressions and BE object checks, not a final SRE SDK link or
product-board acceptance result.

## Follow-up: representative PGO first, members after T2 publication (2026-11-27)

The old scheduling order allowed candidate traffic to sit ahead of a closed
representative's T2, and automatic batch publication waited for that queue
prefix. Members could start after profile freeze, before representative code
publication. Their final-read deadline also counted legitimate queue residence;
the waiter scan could select the representative itself. The tiny-deadline real
ORC regression fails on the parent runtime: the representative publishes but
its member never completes.

The owner now has a bounded, private priority queue for admitted T1 and closed
representative T2 requests. It holds at most the existing 16 sampling admissions;
it does not duplicate member IR, launch another worker, or change shared ABI.
Closed representative T2 precedes admitted T1, which precedes the ordinary
candidate/member queue. Queue rollback preserves the original attempt and source
borrow. A full candidate ring does not prevent admitted PGO work from progressing.
Normal worker throttling remains between requests and publication steps.

Representative T2 publication no longer waits for unrelated queued candidates.
Only successful publication opens that group's member T2 work: a frozen profile
or a linked RW/NX object is insufficient. Publication failure retains the object
and retries after eight worker turns, without compiling it again, blocking the
business core, or starving unrelated queue work. Explicit publication failure
also retains automatic retry. Legacy PGO with representative sharing disabled
keeps its finite-prefix publication behavior.

The representative still samples exactly 64 executed grants; the last read-token
leave is required before profile consumption. Closed T1 calls use AOT while T2
is pending. Members keep using AOT until their exact final identity is validated
and published. Different final IR/bindings still do not share physical code.

Member queue residence is no longer interpreted as an expired source read.
Valid borrows remain until completion, bounded transform failure, cancellation,
deactivation/version change, or shutdown. This does not authorize the product to
mutate borrowed configuration: its existing deactivate/borrow-fence protocol
remains mandatory. Cold representative sampling still has the no-progress timeout
and bounded re-election policy. Waiter maintenance scans at most 64 bindings per
turn and never schedules the representative as its own member. Active T1 cache
entries are not eviction victims of unrelated member publications.

Default candidate groups, representative groups and physical-code identity slots
are aligned at 4096 (formerly 128). Existing identity/module/profile byte budgets
and per-group member bounds remain enforced. These are lazy upper bounds, not
4096 eagerly allocated IR modules. Count limits and byte budgets are distinct;
large product modules can still reach a byte budget and must be diagnosed.

The new real ORC regression drives 130 distinct entries across 6 cells and 6 TRPs
with separate bound descriptors, actual inline-cache registration, and a slow
compiler callback. There are 4680 logical identities. One unequal value requires
a separate group: expected totals are 131 T1 sessions, 8384 real T1 dispatches,
131 physical T2 objects, and 4680 logical T2 publications. It injects a T2 failure
after profile freeze, checks all returned values and live writes, verifies equal
members share pointers and the unequal member does not, drains attempts/admission,
and verifies a warm sweep creates no new attempts. Native elapsed time is not a
product compilation-time prediction.

No new Clang option, product binding macro, public C entry point or lipo root is
required. Rebuild the runtime and relink the image on all cores, then cold-reset.
Shared protocol remains v31; compatible existing AOT/Clang output is unchanged.

Validation for the PGO-first change (audit/diagnostics ON, serialized builds,
maximum `-j8`):

| Check | Result |
| --- | --- |
| Final complete native suite | 421/421; two existing disabled cases, none added |
| Preceding complete-suite repetition | 421/421 in three fresh processes, before the final publication-failure throttle adjustment |
| Queue, timeout, cancellation, nonblocking dispatch and publication regressions | 17/17, shuffled 50 rounds on the final runtime |
| Real 130 x 6 x 6 pressure in each complete run | 4680 final identities; 131 sessions; exactly 8384 T1 dispatches; 131 shared physical objects; no remaining attempts/admissions |
| Full-bucket contention | 20 colliding versions finish without evicting the unsampled representative T1 |
| BE Linux ABI/compiler model | Seven affected/consuming objects compile as MSB AArch64; no undefined `wmemchr` |
| lipo roots | Bound-pointer root check and real AArch64 archive GC check pass, including `ejit_taskpool_print_stats` |

Evidence is in `/home/chen/chen/workspace/ejit-local-support/pr230-on-pr274-20261125`:
`pgo-priority-red.log`, `pgo-priority-pressure*.log/xml`,
`pgo-priority-final-{1..3}.log/xml`, `pgo-priority-release.log/xml`,
`pgo-priority-repeat.log`, `pgo-priority-be.log`,
`pgo-priority-be-objects.txt`, and `pgo-priority-lipo{,-real}.log`.
Pressure uses deterministic shuffled caller order on every sweep. It does not
skip unequal-configuration checks or substitute mocked compilation.

The experimental whole-suite shuffle (`pgo-priority-release-1.log`) is NOT a
passing result: existing singleton fixtures reset the global function registry
while other fixtures retain constructor-time indices, producing missing-bitcode
failures. That run was stopped. The complete-suite results above use the existing
test order in fresh processes; the isolated 17-case scheduling subset is shuffled.
The native/BE/lipo checks do not certify final SRE SDK linkage, real multicore
permissions/cache maintenance, product watchdog behavior or product acceptance.
