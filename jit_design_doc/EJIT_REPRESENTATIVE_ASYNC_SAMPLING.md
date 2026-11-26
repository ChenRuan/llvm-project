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
