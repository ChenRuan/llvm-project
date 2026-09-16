# EJIT version code reuse implementation map

This table tracks implementation evidence for PR230. `independently-verified` is
reserved for coordinator review. The experimental feature remains default off.

| Requirement | Production file / symbol | State | Exact test or artifact | Remaining risk / next gate |
| --- | --- | --- | --- | --- |
| Request-attempt identity and three completion events | `EJitSharedTaskPool::{beginRequestAttempt,cancelRequestAttempt,runCompile,cachePublish}` | implemented; coordinator independently verified milestone | `SharedTaskPoolTest` 173/173 at `30157004912f`; `/home/ruanchen/ejit-dev/agents/pr230-web-review/COORDINATOR_REREVIEW_3015700.md` | Public runtime borrow-completion adapter is still absent. |
| Exact candidate grouping before PGO | `EJitOptimizer::runPipeline`, `EJitCandidateCapture`, `EJitCandidateDirectory` | classifier implemented after accepted `7d26b051a13d`; coordinator review pending; runtime scheduling remains off | exact-source `EJitPreparedCodeTest`: 20/20; real optimizer two-phase prefix/schema capture; preserved-cell 200/200 group and 300 split; binding completeness/kind, schema, forced-collision, group+byte exhaustion; preserved-dims OFF/ON x VP OFF/ON optimizer TU 4/4 | Prefix is serialized before instrumentation and completed only with schema extracted from that same module after IR instrumentation. Hash is bucket-only and full prefix/bindings/schema are compared; the prefix entry and every referenced external function/data binding are validated. No ORC emission, representative scheduling, waiter lifetime, or final-code eligibility gate is connected here. |
| One representative T1 per group | planned taskpool/group scheduler | planned | none | Non-representatives must remain AOT and consume no individual admission. |
| Unique group generation, request token, and sampling session | `EJitCompileDriver::Tier1ProfileIdentity`, `SpecializationContext::samplingSessionId`, `EJitProfileBundle`, `EJitVpSharedState::lastIssuedSessionId` | partial | VP-macro production TUs; scalar session IR test; 120 sequential sessions and 30 waves of four active sessions | Request token and session are separate fields and domains; sampling IDs remain monotonic across owner-driver reconstruction because the allocator is shared. Group generation/profile epoch await the group scheduler. |
| Edge/IC/memop/scalar session isolation | `EJitVpCollector::{ejitVpCreateSession,ejitVpBeginSession,ejitVpBindProfileData,ejitVpTakeSessionSnapshot,ejitVpEndSession}`, session-aware scalar instrumentation; representative-private edge arrays | implemented for bounded active representative sessions; coordinator follow-up pending | collector and original probes 28/28; deterministic cancellation/retirement probes 35/35; shared cancellation/lifecycle integration 173/173; focused scalar/schema 12/12; production VP TUs compile | The ABI-v7 gate uses an exclusive allocation/retirement transition and references for producers, cancellation, and close; terminal discard cannot be changed back to retry preservation. An exact shared request token closes a published T1 session, failed drains are serviced under pressure, owner cold paths erase cancelled partial accumulators, tokenless queued-T2 lifecycle/generation drops notify the owner using the exact generation/version identity, replacement T1s retire any predecessor, and owner release retires sessions before private profd state is cleared. Group-level representative re-election remains to connect. |
| Immutable complete ProfileBundle | `EJitProfileBundle`, `readProfileSchema`, `EJitCompileDriver::compileCold`, `captureTier1ProfileAttemptIdentity`, `applyT1DispatchObservation`, `SpecializationContext::profileBundle` | implemented for the existing single-key T1/T2 chain; repaired above `b91cf59e`; pending independent verification | profile/schema probe exit 0; merge/scalar focused tests 12/12; VP macro production TUs compile; exact-source `EJitObservedT1DispatchTest` 192/192 token (16 observed/bundle tests) and 18/18 observed in NO_RECLAIM (198-test suite: 7 pre-existing base failures + 1 pre-existing flaky); real granted-T1 -> real queued T2 request -> bundle join test | `actualDispatchCount`/`dispatchLimit`/`quotaEnd`/`dispatchQuality` come only from the frozen Tier-1 dispatch observation carried by the Tier-2 request (never the configured threshold or the Tier-2 compile time); `freezeCompletedAt` stays the snapshot-completion instant. The engine-independent driver join is executed end-to-end with real pool objects; the ORC-engine part of `compileCold` remains compile-verified only. In the token build a concurrent closed-quota retry may still enqueue before the frozen timestamp becomes visible and is then reported as unknown (0), never fabricated. |
| 64 real T1 dispatch boundary | `EJitSharedTaskPool::{resolveMatchedSlot,peerPrepareSlot,admitObservedT1Dispatch,admitObservedT1DispatchLocked,classifyHit,enqueueTier2FromLookup,cachePublish}`, slot `t1DispatchLimit/t1DispatchCount/t1QuotaEnd`, bucket `observationLock` | implemented for the per-function experimental request-attempt path (shared ABI v22; the lock word reuses bucket header padding so every offset and `sizeof` are unchanged); repaired above `b91cf59e`; pending independent verification | exact-source `EJitSharedTaskPoolTest` + `EJitObservedT1DispatchTest` 192/192 token and 198 total / 190 pass NO_RECLAIM (7 pre-existing base failures + the pre-existing flaky `ConcurrentPeersCapTier1AtConfiguredSampleCount`, 5/10 head vs 3/10 e245); observed filter 16/16 token and 18/18 NO_RECLAIM, `ConcurrentReplacementCannotInterleaveTheCommit` 30/30 in NO_RECLAIM; reviewer barrier probe K2 (8 threads x 32 calls) 10/10 quota closed at count=hits=64, `missOpenPost=0`, vs e245 1/10; e245 implementation objects fail exactly the two new regressions (seqlock stability, legacy wrong Tier-2); local taskpool layout suite 91/91; compile matrix 14/14; AArch64 BE objects rebuilt with the v22 layout static asserts | Quota is the configured Tier-2 threshold (64 by default); only a committed pointer return consumes one entry; the final allowed dispatch freezes count/quotaEnd once and later calls fall back. A cold non-owner peer preparation carries the exact validated publish coordinates, so the final real dispatch arranges its own Tier-2 request. NO_RECLAIM serializes identity re-check + admission CAS + quotaEnd freeze with every publish/cancel/reset through the separate leaf `observationLock` (v22) instead of the bucket writer lock, so a granted dispatch no longer sets `writeFlag`/bumps `publishSeq` and cannot invalidate a concurrent load-only lookup (R1R-1); the clock is called under that exclusion, so it must stay a non-blocking timestamp source. A failed legacy cold-peer preparation no longer propagates the deferred arm with zeroed bucket0/slot0 coordinates (R2R-01). The replacement regression drains the FIFO ring with a bounded poll loop until its own attempt publishes (R2R-02). Legacy `hitCount` stays an identity-hit/hotness counter; legacy/tokenless mode reports Unavailable. Representative/group ownership, waiter lifetime and the borrow-completion adapter remain open. |
| Frozen bundle consumed by different members | planned group lookup plus `SpecializationContext` | planned | none | Every consumer must validate schema and use the same immutable bundle. |
| Final IR exact compare and one physical code object | `EJitPreparedCode` / `EJitPreparedCodeEmitter` | implemented as backend fixture only | `EJitPreparedCodeTest.VersionFamilySharesRepresentativeProfileAndPhysicalCode` | Not connected to taskpool request/group lifecycle. |
| Representative cancel/re-elect, waiter cancel, same-key retry, exhaustion, queue-full | planned group lifecycle + existing attempt layer | planned | none | Must settle only exact token/session/admission owners. |
| Host / AArch64 BE / board evidence | affected host TUs and ABI probes | partial | host and AArch64 BE header evidence in `SOL_MILESTONE_STATUS.md` | Full BE TU lacks target libc/sysroot; no board evidence. |

Candidate-classifier verification uses the exact files in this worktree. Under
`/home/ruanchen/ejit-dev/build.lock`, `/tmp/compile-candidate.py` compiled
`EJitPreparedCode.cpp`, `EJitOptimizer.cpp`, and `EJitStructFieldPass.cpp`;
`/tmp/compile_candidate_matrix.py` compiled `EJitOptimizer.cpp` with preserved
dimensions OFF/ON crossed with value profiling OFF/ON (4/4); and the linked
`/tmp/pr230-candidate-tests` passed all 20 candidate, final-identity, and native
prepared-code tests. The candidate tests call the production optimizer, capture
the canonical prefix before instrumentation, then use the PGO schema extracted
from that same module after IR instrumentation. No candidate test invokes ORC
emission for a non-representative.

The configured `ninja -C build/ws6/host -j8 EJITTests` target is not evidence for
this milestone: that build directory names a different workstation source tree
and its link also fails on pre-existing missing SRE platform symbols. The
exact-source compile/link above avoids claiming that unrelated failure as either
a product regression or a pass. A first combined locked validation command was
also stopped by its 60-second outer timeout during the compile matrix; splitting
the same locked checks produced the passing results above.

Observed Tier-1 dispatch metadata verification (2026-09-11) uses the exact files
in this worktree. Under `/home/ruanchen/ejit-dev/build.lock`,
`/tmp/pr230-obs/build-shared-tests.sh` compiled the current
`EJitSharedTaskPool.cpp`, `EJitSharedPlatform.cpp`, `EJitLogger.cpp`,
`EJitProfileMerge.cpp`, `EJitSharedTaskPoolTest.cpp` and the new
`EJitObservedT1DispatchTest.cpp` with the exact `EJITSharedTaskPoolTests` target
flags and linked `/tmp/pr230-obs/out/tests`: 185/185 PASS (176 existing + 9 new).
The same script with `-DEJIT_SRE_TASKPOOL_NO_RECLAIM` produced
`/tmp/pr230-obs/out-noreclaim/tests`: all 10 new observed tests PASS; the 7
remaining failures in the pre-existing suite are byte-identical to the HEAD
baseline binary `/tmp/pr230-obs/out-base-noreclaim/tests` built from `git show`
HEAD sources (one further pre-existing threaded test is flaky in both).
`/tmp/pr230-obs/build-taskpool-tests.sh` linked the local (non-shared) taskpool
suite with the v21 request layout: 91/91 PASS. `/tmp/pr230-obs/matrix.py`
compiled 11/11 cells: driver local/shared x VP OFF/ON, production pool
(with/without NO_RECLAIM), profile merge VP OFF/ON, local taskpool VP OFF/ON and
the updated request-layout test TU.

AArch64 big-endian target evidence:
`/tmp/pr230-obs/be/probe_be.cpp` (v21 request/slot layout static_asserts),
`EJitSharedTaskPool.cpp`, `EJitProfileMerge.cpp` and `EJitCompileDriver.cpp` all
compile to genuine `ELF 64-bit MSB, ARM aarch64` objects
(`/tmp/pr230-obs/be/*-be.o`). The BE sysroot is the host aarch64-linux-gnu (LE)
cross-gcc 15 header set plus a one-line `gnu/stubs-lp64_be.h` alias shim; there
is no real BE product sysroot, no BE link, and no board run. The BE pool object
has no `wmemchr` relocation (only `memcpy`/`memmove`/`memset` plus hosted
operator new/delete). Host tests do not substitute for SRE runtime behavior.

Reviewed-defect repair above `b91cf59e` (2026-09-11, pending independent
verification). Under `/home/ruanchen/ejit-dev/build.lock`,
`/tmp/pr230-obs-fix/build-shared-tests.sh` compiled the current
`EJitSharedTaskPool.cpp`, `EJitSharedPlatform.cpp`, `EJitLogger.cpp`,
`EJitProfileMerge.cpp`, `EJitSharedTaskPoolTest.cpp` and the extended
`EJitObservedT1DispatchTest.cpp` with the exact `EJITSharedTaskPoolTests` target
flags: `/tmp/pr230-obs-fix/out/tests` 191/191 PASS (token path, 16
observed/bundle tests) and `/tmp/pr230-obs-fix/out-noreclaim/tests` 189/196 with
only the 7 pre-existing NO_RECLAIM failures of the base binary (the known
threaded `ConcurrentPeersCapTier1AtConfiguredSampleCount` is flaky in both:
6/10 vs 5/10 over ten runs). `/tmp/pr230-obs-fix/build-taskpool-tests.sh`:
local taskpool suite 91/91. `/tmp/pr230-obs-fix/matrix.py`: 14/14 macro cells
(driver local/shared x VP OFF/ON, pool production / NO_RECLAIM /
code-pointers-OFF, profile merge VP OFF/ON, local taskpool VP OFF/ON, taskpool
layout TU, new test TU default / NO_RECLAIM / code-pointers-OFF). Pre-fix
discrimination: `/tmp/pr230-obs-fix/build-base-probe.sh` links the same new test
TU against the `git show b91cf59e` pool + merge implementation; that baseline
binary fails exactly the cold-peer Tier-2 claim tests (3/3) plus, in NO_RECLAIM,
the concurrent-replacement freeze test (observed `publishedInsideCommit == true`
and the predecessor timestamp stamped on the replacement slot) and the
closed-quota freeze test (request `quotaEnd == 0`) — all 5 pass on the repaired
build. AArch64 BE: `/tmp/pr230-obs-fix/be/*-be.o` rebuilt as `ELF 64-bit MSB,
ARM aarch64` with no `wmemchr`/`memchr` undefined symbol; host layout dump
`request=232 slot=240 state=470336 abi=21`, identical to `b91cf59e` (no shared
layout/ABI change in the repair). Honest gaps: the ORC-engine body of
`EJitCompileDriver::compileCold` is still compile-verified only (the
engine-independent granted-T1 -> queued-T2 -> bundle join is executed); no BE
link/board run; R1-3's peer-capture TOCTOU is closed by construction (snapshot
identity + fail-closed commit) but has no deterministic injection point, so it
is not separately reproduced.

Narrow cross-check repair above `e245b6404c4e` (2026-09-11, pending independent
verification). The coordinator cross-check confirmed R1R-1: the e245 NO_RECLAIM
admission commit took the bucket writer lock on every granted observed dispatch,
so `writeFlag`/`publishSeq` invalidated concurrent load-only seqlock readers.
Under `/home/ruanchen/ejit-dev/build.lock`,
`/tmp/pr230-obs-fix2/build-shared-tests.sh` compiled the current
`EJitSharedTaskPool.cpp`, `EJitSharedPlatform.cpp`, `EJitLogger.cpp`,
`EJitProfileMerge.cpp`, `EJitSharedTaskPoolTest.cpp` and the extended
`EJitObservedT1DispatchTest.cpp` with the exact `EJITSharedTaskPoolTests` target
flags: `/tmp/pr230-obs-fix2/tok/tests` 192/192 PASS (16 observed/bundle: 12 pool + 4 bundle; the file defines 14 pool tests, two of them NO_RECLAIM-only) and
`/tmp/pr230-obs-fix2/nrc/tests` 198 total / 190 PASS, whose 8 failures are the
7 pre-existing base failures plus the pre-existing flaky
`ConcurrentPeersCapTier1AtConfiguredSampleCount` (5/10 head vs 3/10 e245 over
ten single-test runs, same signature as the base binary). The observed filter is
16/16 token and 18/18 NO_RECLAIM; the previously flaky
`ConcurrentReplacementCannotInterleaveTheCommit` is 30/30 in NO_RECLAIM after the
bounded FIFO drain (R2R-02). `/tmp/pr230-obs-fix2/probe-head/tests` (the
reviewer's barrier probe linked against the fresh NO_RECLAIM objects) closes the
threshold-64 quota in 10/10 barrier 8-thread x 32-call runs with
count=hits=64, `quotaEnd=1000` and a post-call `missOpenPost=0` (no miss while
the quota was still open); the same probe linked against the e245 objects closes
only 1/10 and reaches count 48-61 in the rest (R1R-1 reproduced). Pre-fix
discrimination with the current test TU: the e245 implementation objects fail
exactly the two new regressions (seqlock stability, legacy wrong-Tier-2 claim)
and the b91 objects additionally fail the three R2-PR230-01 cold-peer tests.
Layout `/tmp/pr230-obs-fix2/layout/sizes`: `request=232 slot=240 bucket=3904
state=470336 abi=22 obsLockOff=12 slotsOff=16` (no size/offset change).
AArch64 BE `/tmp/pr230-obs-fix2/be/*-be.o`: `ELF 64-bit MSB, ARM aarch64` with
the v22 layout static asserts and no `memchr`/`wmemchr` undefined symbol.
`/tmp/pr230-obs-fix2/taskpool/tests`: local taskpool suite 91/91;
`/tmp/pr230-obs-fix2/matrix.py`: 14/14 macro cells. Code-pointers-OFF variants
compile (matrix) and run the observed filter with the four pre-existing
cold-peer tests failing exactly as with the e245 nocp objects and the new R2R-01
test skipping itself. Honest gaps: host x86_64 only, no configured CMake suite
(ws6 names another source tree), no BE link/board run, and the ORC-engine body
of `compileCold` remains compile-verified only.
