# PR230 Host Closeout - 2026-09-16

Status: host implementation and the latest legacy concurrency follow-up are
ready to publish for review. The feature remains OFF by default and the PR
remains Draft. This is not product, board, or whole-specification acceptance.

## Implemented Host Path

The production runtime classifies candidates before representative sampling,
executes generated Tier-1 code, freezes the group's profile, and compares final
IR plus effective bindings before sharing physical Tier-2 code. Host coverage
includes 20 entries across six cells, unequal-member separation, profile/session
isolation, cancellation, re-election, source-borrow completion, queue rejection,
compile failure, stale callbacks, and token/NO_RECLAIM variants.

Bound-pointer requests still use conservative independent compilation. The
IR01 may_const board sample is a separate feature test, not PR230 acceptance.

## Failed-Preparation Lock Repair

The NO_RECLAIM legacy cold path used Tier-2 eligibility coordinates to select
the settlement lock. Failed peer preparation cleared those coordinates while
preserving the real cache slot, allowing a nonzero-bucket slot to be charged
under bucket zero's lock.

The fix carries independent legacy bucket/slot coordinates, validates bounds
and exact slot ownership, and then checks the captured publication identity
under the real bucket's observation lock. Failed preparation still grants no
Tier-1 execution and must not arm Tier-2. Token behavior and shared-state ABI
are unchanged. Five permanent tests cover successful/failed generic lookup
and failed fixed 0D/1D/2D lookup with the actual nonzero-bucket lock held.

An independent, read-only Luna xhigh review found no actionable issue in this
narrow production patch. It did not independently run tests, review the later
stress-classification change, or accept the entire feature.

## Concurrent Stress Classification

Before changing any acceptance condition, the original concurrent test was
instrumented with per-thread records printed after join. The original strict
assertions passed 977/1000 independent processes. The 23 failures contained
24 captured status-5 (`PgoAdmissionDeferred`) returns, all with the expected
AOT pointer, no read token, and exactly 64 total Tier-1 grants. Their original
logs, executable, source snapshot, and hashes are retained.

The bounded seqlock lookup can exhaust its retries while the function already
owns a PGO admission. Its slow path explicitly returns an AOT admission
deferral; it must not be reported as a new queued compilation or a T1 grant.
The test previously accepted only `AlreadyPending` as AOT.

The corrected test accepts only that exact documented deferred result shape,
not arbitrary non-hits. It additionally validates the Tier-1 pointer, absence
of fallback read tokens, deferred-miss accounting, and all 256 calls. The
exactly-64 grant and hit-count assertions, single queued T2, and single T2
compile remain. No production status or quota was weakened to pass this test.
These are mock-pointer pool tests, not 64 generated-code executions.

Fresh capture establishes the current failure mechanism; it does not recover
the unrecorded statuses of historical failures or erase earlier quota defects.

## Verification

| Check | Result |
| --- | --- |
| Focused original probes and lock/publication controls, before test-only closeout | 18/18 |
| Final-source shared pool, NO_RECLAIM, all target TUs rebuilt | 205/205 |
| Final-source shared pool, token, all target TUs rebuilt | 187/187 |
| Corrected concurrent NO_RECLAIM test, independent processes | 1000/1000 |
| Corrected concurrent token test, independent processes | 100/100 |
| Real representative runtime, NO_RECLAIM | 4/4 |
| Real representative runtime, VP + NO_RECLAIM | 4/4 |

Runtime results are the September16 fresh 29-TU builds. Their production/test
inputs and EJIT headers were rehashed against the closeout source; the later
edit affects only the standalone shared-pool test, not either runtime target.
Non-EJIT LLVM libraries were retained. Evidence is Windows x86-64 only.

Local evidence directories under the PR230 control root:
`reports/failed-prep-repair-20260916/` and `reports/closeout-20260916/`.
The latter retains strict-capture and corrected results separately.

## Remaining Gates

- Current-source AArch64 big-endian artifacts, ELF/dependency inspection,
  stable-wrapper comparison, cross-core permissions and actual board execution.
- The PR230 single-C six-cell/twenty-entry board scenario and product
  correctness/performance/resource acceptance from specification sections 9/11.
- Bound-pointer representative source borrowing/shared-code support; current
  fallback is independent compilation, not a claim of shared support.
- Broader whole-feature review and final commit organization before merge.

Publishing this implementation does not enable the feature or authorize merge.
