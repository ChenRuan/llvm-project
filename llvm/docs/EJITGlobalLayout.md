# EJIT Global Function Layout

Issue #245, spec v0.4 (2026-10-13), baseline
`e09df842062735ba9f6279ae7d1c5b43fd723bec`.
The former v0.3 pending-scope/dimension-major checkpoint is superseded.
Production implementation and native tests are present. SDK final linking,
cold core6/core16 board acceptance and performance acceptance remain pending.

## Frozen Contracts

The coordinator-approved invariant covers EVERY declared final Baseline body
(PGO off), or final PGOUse/T2 body (PGO on), of one selected complete generation.
Temporary T1 and prior/retired generations are not moved or included in that
generation's order; their real execution lifetime and memory costs remain protected.
No late, cold, inactive or insufficiently sampled final member is optional.

The fully expanded manifest array position is authoritative global rank, across
ALL preparation batches and sampling waves. Dimension identities do not re-sort
the caller's list. V1 takes an explicit list, not an implicit expansion template.
If a generator offers a function template, it must display/freeze its complete
cell/TRP/version expansion before calling initialization.

V1 provides sequential safe publication after ALL bodies have linked, passed
actual address/range validation and sealed. It does not promise all-entry atomic
visibility. Mixed still-valid old/new dispatch is permitted during cutover.
Partial failure retains published resources, reports published/unpublished ranks
and remains FAILED, never COMPLETE. Recovery is a new explicit generation after
the existing lifecycle permits shutdown; no invalid identity is silently retried.

## Manifest and Identity

V1 requires the batched NO_RECLAIM shared SRE code pool.
`Config::globalLayout` / `ejit_init_global_layout` freezes an explicit complete
initialization list. The additive C API leaves `ejit_config_t` unchanged.
Names resolve read-only through `EJitFuncRegistry`; every registered entry must
be represented. Each declared specialization has its exact registered dimension
shape, instance versions and bound argument/address/extent descriptors.
`sharedOwnerElected` validates loader metadata, bound argument indices and full
registry coverage. `configureGlobalLayout` rejects duplicates, unsupported
shapes, invalid descriptors, dispatch-key collisions and bucket capacity overflow
as one initialization failure, with no partial installation.

Completeness of instance expansion is caller-declared: V1 cannot infer which
inactive cell/TRP/version combinations a product intends to require. The supplied
list must be that complete expansion; every supplied member remains mandatory.
Different concurrent bindings or versions with the same legacy dispatch key are
not representable and fail explicitly rather than sharing the wrong body.
Registered dimension formal order is kept for legacy dispatch compatibility;
it is not layout rank. Manifest/source-bitcode/configuration/budget/bound identity
participates in the shared peer fingerprint. Request equality is structural,
including owner generation, dimension/version pairs and complete bound descriptors.
Stable T2 lookup does not query or sort the manifest.

## Preparation Boundary

`EJitOrcEngine::prepareBitcodeModule` parses and isolates the entry, resolves
registered imports, consumes borrowed bound views, applies the SAME extracted
`optimizeModule` routine as the immediate path, then calls the configured
`IRCompileLayer::getCompiler`. The artifact owns a relocatable `MemoryBuffer`,
entry/tier/cache identity and import symbols, not already-relocated machine code.
No JITDylib object submission, executable allocation or callable lookup occurs
until `linkPreparedObject`. `EJitCompileDriver::prepareNow` is wired to the real
shared worker; it is not a test-only planner.

Reachable mutable/address-significant data requires canonical registered backing
through existing period/static/user symbol registries. It becomes an external
import of the original storage, never a copied mutable initializer. Only proven
address-insignificant immutable data can remain private. Unused local globals
are pruned after other roots are isolated, preventing unrelated data from being
replicated into every object. Unsupported backing fails conservatively.

## Normal PGO and True Returns

T1 still compiles/links immediately in the far pool and executes real samples
under the unchanged admission limit. A sampling lease is acquired BEFORE
incrementing the dispatch hit count; the normal wrapper leave releases it only
after the real call returns. Saturation closes new T1 dispatch; waiting calls use
legal AOT/old fallback. `samplingDrained` requires threshold hits AND true returns
AND zero executing leases. Worker polling remains nonblocking while draining.

The complete counters/helper inventory, verified ICP/memop and scalar/audit
metadata are frozen through existing snapshot protocols. Sibling VP snapshots
remain owned and charged until their own identity is prepared. Helper profile
names include full specialization identity to prevent cross-root conflation.
Missing/inconsistent profiles and failed snapshots cannot fall back to an
edge-only pretend success in global mode.

Global-layout VP uses bounded site probing (same existing capacity), not
direct-map sibling displacement. Full tables, out-of-range cores or preempting
same-core writers latch a failed round instead of silently losing data or
spinning under preemption. Existing nonglobal bounded approximation remains
unchanged. The lossless label concerns site ownership, not unbounded retention
of arbitrary value distributions; existing K-way heavy hitters still apply.

Only AFTER an owned PGOUse artifact exists does `releaseSamplingAdmission` free
the drained member's slot. This is distinct from `finishPgoFunction` publication;
it neither claims T2 publication nor fabricates samples. Ready markers prevent
resampling while final layout waits. T1 counters/side metadata can then be released;
the emitted object no longer borrows them. Admission remains funcIndex-based as
in the existing pool, so instances of one entry advance sequentially.

## Ordered Linking and Cutover

`serviceGlobalLayout` gates linking on required == ready, rechecks identities,
and submits every object in explicit rank order. Fresh serial-tagged JITDylibs
retain older generations. Existing JITLink, lazy imports, graph helpers/GOT,
alignment and code-pool routing determine actual size, not fixed per-function slots.
Actual entry addresses must strictly increase; each finalized owning executable
range must contain its body and be Near. Dynamic allocator address reversal or
unexpected routing fails, never produces a sorted-pointer success claim.

The existing batch seal/flush and code-readiness/range callbacks run before the
first cache publication. Shared lookup retains existing per-core preparation
guards. An intermediate `flushCodeBatch` request being serviced is NOT global
COMPLETE and cannot link/seal a collected subset. State transitions use guarded
CAS so concurrent lifecycle invalidation cannot be overwritten by COMPLETE.

Already-shared/aliased identities are not exempt from order checks. V1 rejects
legacy-key collisions, emits independent closed graphs only with canonical data
backing and requires strictly increasing body addresses. It does not claim
integration with unmerged #230/#231 common-profile/shared-body mechanisms.

## Budgets and Shutdown

Defaults: 1024 members (also queue/bucket capacity constrained), 64 MiB objects
and import metadata, 32 MiB frozen profile/side metadata, 16 MiB each Near/Far
retained reservation, and 1,048,576 collection worker polls including drain wait.
The pending sibling VP record cap uses a quarter of the profile byte budget.
Poll budgets are not wall-clock or hard-real-time compiler deadlines.

Persistent shared Near/Far reservation ledgers charge complete pools, raw alignment
slack, failed holes, helpers/GOT/data and all retired NO_RECLAIM generations.
Untracked legacy owner history cannot switch to layout ownership, and retained
layout allocations cannot switch to legacy ownership without a cold reset; this
prevents reuse of old pools whose retirement offsets cannot be proven.
Fixed-region offsets survive compatible compiler-owner changes; old legal addresses are not
overwritten. Exhaustion is explicit fallback, not reserve enlargement. Object
buffers are released on failure or after finalized validation; linked/partially
published code remains charged and retained. A failed generation closes/drains
remaining T1 rounds before releasing their sampling ownership.

Shutdown stops/joins the worker and uses a bounded wait for real T1 leaves before
slot reuse or compiler destruction. The C API defers shutdown while live leases
remain, retaining its runtime for a later retry. Direct C++ destruction with an
unreturned T1 retains one compiler and leaves the shared domain Stopping: this
conservative terminal retention is preferable to UAF, not a successful shutdown.
Platform obligations for stable NO_RECLAIM code/registered data remain unchanged.
Shared ABI is 19 and VP ABI is 3: rebuild every participating image together.

## Verification Boundaries

Actual-source state tests cover missing members, explicit cell/TRP rank, invalid
manifests/bindings, object budget, preparation/link/order/seal failures, stale
versions, partial cutover and retained reservations. They do not alone prove
real machine-code order. Actual compiler integrations cover A/H/Z, reverse and
permuted arrivals, intermediate flushes, helpers/registered mutable backing,
cross-4KiB and cross-2MiB graphs, live T1 shutdown, and real four-slot/20-target
PGO with exactly 160 preserved indirect-call observations before final linking.
Real cell/TRP/bound Baseline and PGO cases exercise the expanded identity path.

QA01-QA11 have native evidence, but this is not comprehensive product acceptance.
SDK/BE final DLIB linking and QA12 cold core6/core16 board execution are pending;
`ejit_test/ejit_global_layout_sre_test.c` is a source fixture, not a board result.
QA13 overhead/latency/I-cache/iTLB/business-cycle measurements remain pending.
No layout speedup, board readiness or issue closure is claimed. Raw commands,
flags, counts and retained failure logs are recorded in the task support RESULT.
