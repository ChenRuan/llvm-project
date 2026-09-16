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
| Immutable complete ProfileBundle | `EJitProfileBundle`, `readProfileSchema`, `EJitCompileDriver::compileCold`, `SpecializationContext::profileBundle` | implemented for the existing single-key T1/T2 chain; coordinator follow-up pending | profile/schema probe exit 0; merge/scalar focused tests 12/12; VP macro production TUs compile | `quotaEnd` is currently observed at Tier-2 compile rather than at the exact taskpool saturation transition; `actualDispatchCount` is still derived from the threshold; group metadata stays zero until grouping lands. |
| 64 real T1 dispatch boundary | `EJitSharedTaskPool::resolveMatchedSlot` / Tier-2 enqueue | partial (existing per-function path) | SharedTaskPool current-source regression 170/170 | Move quota ownership from function slot to representative session; distinguish quota end from freeze time. |
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
