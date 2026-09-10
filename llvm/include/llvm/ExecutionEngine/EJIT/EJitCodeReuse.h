//===-- EJitCodeReuse.h - PR230 shared-version code reuse core -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Cold-path bookkeeping for EJIT_VERSION_CODE_REUSE_SPEC.md (PR230):
//
//   * logical identity vs physical identity (§5.2, §8.1): a LogicalKey names
//     one (entry, cell, trp, versions) cache entry; a CodeKey names one
//     physical emission. Many logical entries may point at one CodeRecord.
//   * two comparison points (§5.1): a pre-PGO prefix comparison that only picks
//     the shared profile, and a final comparison over the complete optimized
//     emission IR that is the only thing allowed to authorize code sharing.
//   * a digest is an index and never proof (§5.3): every candidate hit is
//     confirmed by exact byte comparison of the canonical emission plus the
//     semantic binding environment.
//   * request attempts and three separate completion events (§5.8): a stale
//     callback from a cancelled attempt must not settle a new attempt that
//     reuses the same LogicalKey / version / group / profile epoch.
//   * group-level PGO state (§5.5-§5.7): one representative sampler per group,
//     nonrepresentatives wait instead of consuming admission, the 64 quota
//     counts real T1 dispatches, and the frozen ProfileBundle is immutable.
//   * bounded state (§6.5): every table and retry loop has a budget and falls
//     back to AOT instead of growing without limit.
//
// None of this runs on the stable wrapper path: it is compilation/cancellation
// cold-path state only, and this file adds no query to the published hit path.
//
// This header is deliberately free of ORC/CodeGen dependencies so the state
// machine can be unit tested without an ExecutionSession.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITCODEREUSE_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITCODEREUSE_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <string>
#include <vector>

namespace llvm {

class Module;

namespace ejit {

//===----------------------------------------------------------------------===//
// Identity
//===----------------------------------------------------------------------===//

/// One specialization dimension of a logical version: the period name and the
/// instance the caller asked to compile. Kept as data even though the shared
/// mode no longer turns it into a constant, because it is still the cache and
/// lifecycle identity (§3.1).
struct ReuseDim {
  std::string periodName;
  uint32_t instance = 0;

  bool operator==(const ReuseDim &Other) const {
    return periodName == Other.periodName && instance == Other.instance;
  }
};

/// Logical identity of one cache entry (§5.2). Two logical entries with the
/// same key are the same version of the same function; the physical code they
/// map to is tracked separately.
struct LogicalKey {
  std::string entry;
  /// Source/bitcode revision identity (never a path: two hosts must agree).
  std::string sourceId;
  SmallVector<ReuseDim, 4> dimensions;
  /// Per-dimension lifecycle version, aligned with `dimensions`.
  SmallVector<uint64_t, 4> lifecycleVersions;
  /// Runtime registry/generation the key was formed under. Prevents reuse
  /// across a shutdown or an incompatible re-link (§5.2).
  uint64_t runtimeGeneration = 0;

  bool operator==(const LogicalKey &Other) const;
  bool operator!=(const LogicalKey &Other) const { return !(*this == Other); }

  /// Stable text encoding for map keys and diagnostics. Deterministic across
  /// processes: no pointers, no addresses.
  std::string encode() const;
};

/// One externally referenced symbol and the address it resolved to. Addresses
/// are part of the identity: the same IR bound to different addresses must not
/// share code (§5.2, §5.3).
struct BindingEntry {
  std::string symbol;
  uint64_t address = 0;
  /// 0 = function, 1 = global data, 2 = other/unknown.
  uint8_t kind = 2;

  bool operator==(const BindingEntry &Other) const {
    return symbol == Other.symbol && address == Other.address &&
           kind == Other.kind;
  }
};

/// The effective symbol-binding environment of one compilation.
struct BindingEnvironment {
  SmallVector<BindingEntry, 8> entries;

  /// Sort by symbol so two enumerations of the same binding agree.
  void normalize();
  bool operator==(const BindingEnvironment &Other) const;
  bool operator!=(const BindingEnvironment &Other) const {
    return !(*this == Other);
  }
  std::string encode() const;
};

/// Validated PGO schema identity (§5.5). The digest covers the function/site
/// schema after the Gen/Use correspondence has been verified; a bare CFG hash
/// is not sufficient and must not be used here.
struct PgoSchemaKey {
  /// Gen/Use policy identifier (which instrumentation and use phases ran).
  std::string policy;
  /// Digest of the verified function/site schema (names, kinds, counts).
  std::string schemaDigest;
  uint32_t functionCount = 0;
  uint32_t indirectCallSites = 0;
  uint32_t memOpSites = 0;
  uint32_t scalarSites = 0;
  /// Value-profile kinds that were enabled but produced no usable data. These
  /// are recorded rather than silently dropped, and they are part of the key.
  SmallVector<std::string, 2> disabledKinds;

  bool operator==(const PgoSchemaKey &Other) const;
  bool operator!=(const PgoSchemaKey &Other) const { return !(*this == Other); }
  std::string encode() const;
  bool isUsable() const { return !schemaDigest.empty(); }
};

/// A canonicalized module image used for exact comparison.
///
/// `digest` indexes candidates; `bytes` is the comparison material. Equal
/// digests with different bytes are a hash collision and must fall back to
/// independent compilation, never to reuse.
struct CanonicalModule {
  std::string digest;
  std::string bytes;

  bool operator==(const CanonicalModule &Other) const {
    return bytes == Other.bytes;
  }
  bool empty() const { return bytes.empty(); }
};

/// Produce the canonical comparison image of \p M.
///
/// Removed: module identifier/path, source filename, debug info and locations,
/// and local SSA/block/argument display names (consistently renumbered by the
/// printer, so every reference follows). Retained: instruction order, types,
/// constants and their bit patterns, global and function names, metadata,
/// attributes, calling conventions, aliases, comdat, section, and target
/// triple/DataLayout.
CanonicalModule canonicalizeModule(const Module &M);

/// Pre-PGO candidate group identity (§5.1, §5.2). Only the *profile* decision
/// is made at this comparison point; a match does not authorize sharing code.
struct ProfileGroupKey {
  std::string sourceId;
  std::string entry;
  /// Compiler/pipeline policy that produced the prefix.
  std::string policy;
  /// Digest of the canonical common specialization prefix IR.
  std::string prefixDigest;
  PgoSchemaKey schema;
  BindingEnvironment bindings;

  bool operator==(const ProfileGroupKey &Other) const;
  bool operator!=(const ProfileGroupKey &Other) const {
    return !(*this == Other);
  }
  std::string encode() const;
};

/// Physical emission identity (§5.2). Deliberately excludes cell/TRP values and
/// their lifecycle versions: those live in the LogicalKey, and including them
/// here would defeat cross-instance reuse. Anything the IR actually kept — a
/// folded constant, a materialized address — is inside `finalIrDigest`, so it
/// still separates the emissions.
struct CodeKey {
  std::string sourceId;
  std::string entry;
  /// Compiler/pipeline/target/ABI policy.
  std::string compilerPolicy;
  BindingEnvironment bindings;
  std::string finalIrDigest;

  bool operator==(const CodeKey &Other) const;
  bool operator!=(const CodeKey &Other) const { return !(*this == Other); }
  std::string encode() const;
};

//===----------------------------------------------------------------------===//
// Physical code records
//===----------------------------------------------------------------------===//

/// One [address, address + size) range, in whatever space it belongs to.
struct ReuseRange {
  uint64_t address = 0;
  uint64_t size = 0;

  bool operator==(const ReuseRange &Other) const {
    return address == Other.address && size == Other.size;
  }
};

/// Publication state of one physical code object. The states are explicit so a
/// callback or re-entrant lookup can never observe a half-built record (§6.2).
enum class CodeState : uint8_t {
  Building = 0,
  LinkedPending = 1,
  Published = 2,
  Failed = 3,
};

const char *codeStateName(CodeState State);

/// One physical JIT emission, referenced by any number of logical versions.
struct CodeRecord {
  uint64_t codeId = 0;
  CodeKey key;
  /// The canonical final emission the key was computed over, retained for the
  /// exact comparison and for the dump/identity diagnostics (§7.1).
  CanonicalModule finalIr;
  uint64_t entryAddress = 0;
  std::vector<ReuseRange> execRanges;
  std::vector<ReuseRange> dataRanges;
  /// Placement domain actually used (unified near pool for final T2).
  std::string pool;
  CodeState state = CodeState::Building;
  /// How many logical versions currently reference this record.
  uint32_t logicalRefs = 0;

  /// Total executable bytes of this record, counted once (§7).
  uint64_t execBytes() const;
};

enum class ReuseDecision : uint8_t {
  /// Exact match on a published record: reuse its address (§6.2).
  ReusedPublished,
  /// Exact match on a record that is still RW/NX: join it as a waiter.
  ReusedPending,
  /// No exact match: this caller compiles its own emission.
  CompileIndependently,
  /// A budget was exhausted: fall back to AOT, do not keep compiling (§6.5).
  BudgetExhausted,
};

/// Result of a candidate lookup.
struct ReuseLookup {
  ReuseDecision decision = ReuseDecision::CompileIndependently;
  uint64_t codeId = 0;
  /// True when the digest matched but the exact comparison did not. A hash
  /// collision must be visible to diagnostics, not silently treated as a miss.
  bool digestCollision = false;
};

struct ReuseTableBudget {
  uint32_t maxCodeRecords = 256;
  uint32_t maxLogicalBindings = 4096;
  uint32_t maxFailedMatches = 128;
  /// Comparison material is bounded independently of the record count (§6.3).
  uint64_t maxCanonicalBytes = 4 * 1024 * 1024;
  uint64_t maxFailedMatchBytes = 256 * 1024;
};

struct ReuseTableStats {
  uint32_t codeRecords = 0;
  uint32_t logicalBindings = 0;
  uint32_t failedMatches = 0;
  uint64_t canonicalBytes = 0;
  uint64_t execBytesUnique = 0;
  uint32_t reusedVersions = 0;
  uint32_t digestCollisions = 0;
  uint32_t budgetRejections = 0;
};

/// Cold-path index from CodeKey to CodeRecord. Owner-private (§8.2): the
/// container is only ever touched by the compiling worker.
///
/// A digest hit never authorizes reuse on its own: `lookup` re-checks the exact
/// canonical bytes and the binding environment, and records a failed match when
/// the digest collided so the same pair is not retried forever (§5.4).
class CodeReuseTable {
public:
  explicit CodeReuseTable(ReuseTableBudget Budget = {}) : budget_(Budget) {}

  /// Register a fresh emission and return its code id. `OutOfBudget` is
  /// reported through the return value rather than by growing the table.
  struct AddResult {
    bool ok = false;
    uint64_t codeId = 0;
  };
  AddResult addCandidate(const CodeKey &Key, CanonicalModule FinalIr);

  /// Find an existing emission for \p Key. Only exact equality of the canonical
  /// bytes and the bindings returns Reused*; a digest-only match is reported as
  /// a collision and remembered as a failed pair.
  ReuseLookup lookup(const CodeKey &Key, StringRef CanonicalBytes);

  /// Move a record to LinkedPending/Published/Failed. Publishing records the
  /// real address and ranges exactly once; a second call is a no-op.
  bool markLinked(uint64_t CodeId, uint64_t EntryAddress,
                  ArrayRef<ReuseRange> ExecRanges,
                  ArrayRef<ReuseRange> DataRanges, StringRef Pool);
  bool markPublished(uint64_t CodeId);
  bool markFailed(uint64_t CodeId);

  const CodeRecord *getRecord(uint64_t CodeId) const;

  /// Associate one logical version with a physical record. Independent
  /// invalidation: removing a logical binding never touches the record or any
  /// other logical binding (§6.3).
  bool bindLogical(const LogicalKey &Logical, uint64_t CodeId);
  bool unbindLogical(const LogicalKey &Logical);
  /// Returns 0 when the logical version has no binding.
  uint64_t codeIdFor(const LogicalKey &Logical) const;

  /// Number of logical versions currently mapped to \p CodeId.
  uint32_t logicalRefs(uint64_t CodeId) const;

  const ReuseTableStats &stats() const { return stats_; }
  const ReuseTableBudget &budget() const { return budget_; }

private:
  void recomputeStats();
  void rebuildDigestIndex();
  void recordFailedMatch(const CodeKey &Key, StringRef CanonicalBytes);

  ReuseTableBudget budget_;
  uint64_t nextCodeId_ = 1;
  std::vector<CodeRecord> records_;
  /// digest -> indices into records_. A vector (not a set) because a digest
  /// collision is expected to be observed and reported, not assumed away.
  std::vector<std::pair<std::string, std::vector<uint32_t>>> digestIndex_;
  std::vector<std::pair<std::string, uint64_t>> logicalBindings_;
  std::vector<std::pair<std::string, std::string>> failedMatches_;
  uint64_t failedMatchBytes_ = 0;
  ReuseTableStats stats_;
};

//===----------------------------------------------------------------------===//
// Request attempts and the three completion events (§5.8)
//===----------------------------------------------------------------------===//

/// Identifies one request attempt. Encodes the owner generation and a sequence
/// number; it is never derived from a reusable object address.
using RequestToken = uint64_t;

constexpr RequestToken kInvalidRequestToken = 0;

enum class AttemptState : uint8_t {
  /// Created; reads and publication are still allowed.
  Active = 0,
  /// Cancellation requested; no new reads may be scheduled, but the borrow is
  /// not yet confirmed ended.
  Cancelling = 1,
  /// Compile borrow confirmed ended and the attempt is finished (published,
  /// failed or cancelled).
  Finished = 2,
};

struct AttemptRecord {
  RequestToken token = kInvalidRequestToken;
  std::string logicalKey;
  AttemptState state = AttemptState::Active;
  /// The three events are tracked separately and each settles exactly once.
  /// Set once cancellation has been requested. A cancelled attempt can never
  /// be reported as published, whatever its later event order is.
  bool cancelRequested = false;
  bool samplingSettled = false;
  bool samplingAborted = false;
  bool borrowEnded = false;
  bool published = false;
  /// Resources this attempt actually holds. Used so a stale callback releases
  /// only its own resources.
  bool holdsAdmission = false;
  bool holdsBorrow = false;
  bool holdsWaiter = false;
  uint64_t publishedCodeId = 0;
};

struct RequestRegistryBudget {
  uint32_t maxLiveAttempts = 256;
  uint32_t maxFinishedHistory = 512;
};

/// Outcome of one event delivery.
struct EventResult {
  /// False when the event was rejected: unknown/stale token, wrong attempt
  /// state, or an already-delivered event (idempotent no-op).
  bool accepted = false;
  bool duplicate = false;
  /// True when the caller must release the sampling admission slot exactly once
  /// for this delivery.
  bool releaseAdmission = false;
  /// True when this delivery confirmed the compile borrow ended.
  bool borrowConfirmed = false;
  /// True when this delivery settled the logical publication.
  bool logicalSettled = false;
  /// True when the attempt is now finished and may be reaped.
  bool attemptFinished = false;
};

/// Owner-private registry of live request attempts.
///
/// A new attempt always receives a token that cannot be confused with a
/// callback from an older attempt, even when the LogicalKey, version, group and
/// profile epoch are identical. Old callbacks still settle their own resources
/// (§5.8); they simply cannot touch the new attempt.
class ReuseRequestRegistry {
public:
  ReuseRequestRegistry(uint64_t OwnerGeneration = 1,
                       RequestRegistryBudget Budget = {})
      : ownerGeneration_(OwnerGeneration), budget_(Budget) {}

  /// Start a new attempt for \p Logical and supersede any previous live attempt
  /// with the same logical key. The previous attempt stays alive until its own
  /// events arrive; it is merely no longer current.
  RequestToken begin(const LogicalKey &Logical);

  /// Request cancellation: Active -> Cancelling. Publication is closed and no
  /// new reads may be scheduled; the attempt is not finished until its borrow
  /// ends (§5.8).
  bool cancel(RequestToken Token);

  /// SamplingFinished: the representative's bundle was frozen or the sampling
  /// was aborted. Delivered at most once per attempt; releases the admission
  /// this attempt holds, and only this attempt's.
  EventResult finishSampling(RequestToken Token, bool Aborted);

  /// CompileBorrowEnded: the compiler no longer reads the borrowed object.
  /// Does not mean the business object may be destroyed (§3.5).
  EventResult endCompileBorrow(RequestToken Token);

  /// LogicalPublished: the logical version's mapping became Ready after the
  /// physical code was executable and re-validated. Does not release the group
  /// admission a second time.
  EventResult publishLogical(RequestToken Token, uint64_t CodeId);

  /// Record that this attempt acquired/released auxiliary resources. Used by
  /// the group layer so a stale callback releases exactly what it took.
  void setHoldsAdmission(RequestToken Token, bool Holds);
  void setHoldsWaiter(RequestToken Token, bool Holds);

  AttemptState state(RequestToken Token) const;
  const AttemptRecord *record(RequestToken Token) const;
  /// A token is current when it is the latest attempt for its logical key.
  bool isCurrent(RequestToken Token) const;
  RequestToken currentFor(const LogicalKey &Logical) const;
  uint32_t liveAttempts() const { return liveCount_; }

private:
  AttemptRecord *find(RequestToken Token);
  const AttemptRecord *find(RequestToken Token) const;
  void reapFinished();

  uint64_t ownerGeneration_;
  RequestRegistryBudget budget_;
  uint64_t nextSequence_ = 1;
  std::vector<AttemptRecord> attempts_;
  std::vector<std::pair<std::string, RequestToken>> currentByLogical_;
  uint32_t liveCount_ = 0;
};

//===----------------------------------------------------------------------===//
// Sampling sessions and the frozen profile bundle (§5.6, §5.7)
//===----------------------------------------------------------------------===//

/// Identity of one representative sampling round. Every raw sample, reset and
/// late write is attributed to one of these; a global "current group" or a core
/// id is never a substitute (§5.6).
struct SamplingSessionId {
  uint64_t groupId = 0;
  uint64_t profileEpoch = 0;
  uint64_t session = 0;

  bool operator==(const SamplingSessionId &Other) const {
    return groupId == Other.groupId && profileEpoch == Other.profileEpoch &&
           session == Other.session;
  }
  bool operator!=(const SamplingSessionId &Other) const {
    return !(*this == Other);
  }
  std::string encode() const;
};

/// Sampling quality of one frozen bundle. The 64-dispatch quota counts real T1
/// dispatches and the snapshot is explicitly approximate (§5.7): these fields
/// are what keeps the diagnostics from claiming 64 completed calls.
struct SamplingQuality {
  /// Real T1 dispatches observed when the quota closed.
  uint64_t dispatchCount = 0;
  /// The quota the session closed on.
  uint64_t dispatchQuota = 64;
  /// Tick when new dispatch stopped, and when the bundle was frozen.
  uint64_t quotaEndTick = 0;
  uint64_t freezeCompletedTick = 0;
  /// Shards that were missing at snapshot time (unknown, never fabricated).
  uint32_t missingShards = 0;
  /// True when the snapshot is bounded/approximate rather than an exact cut.
  bool approximate = true;
  /// True when the counting boundary could not be established.
  bool countBoundaryUnknown = false;

  std::string encode() const;
};

/// Immutable, complete profile bundle handed to every member's T2 (§5.6).
/// It carries the scalar side table and the validated schema, not just the
/// indexed profile payload: copying ctx.profileData alone would lose them.
struct ProfileBundle {
  std::string indexedProfile;
  /// Scalar/loop-bound side table for ctx.scalarValueSites. Kept as encoded
  /// site records so this header stays independent of the value-profile types.
  std::vector<std::string> scalarSites;
  PgoSchemaKey schema;
  /// Digest of the verified indirect-call target mapping.
  std::string targetMappingDigest;
  SamplingSessionId session;
  SamplingQuality quality;
  /// Representative logical identity that produced the samples.
  LogicalKey representative;
  bool frozen = false;

  std::string encode() const;
};

//===----------------------------------------------------------------------===//
// Group table (§5.4-§5.7)
//===----------------------------------------------------------------------===//

enum class MemberRole : uint8_t {
  /// This member runs the group's single T1 sampling task.
  Representative,
  /// This member runs AOT until the group bundle is frozen.
  Waiter,
  /// No budget: the member must use AOT and must not be retried in a loop.
  Rejected,
};

enum class GroupState : uint8_t {
  /// Created, no representative admitted yet.
  Idle = 0,
  /// One representative is sampling.
  Collecting = 1,
  /// The immutable bundle exists; members may proceed to T2.
  BundleFrozen = 2,
  /// Sampling failed; the group is finished without a bundle.
  SamplingAborted = 3,
};

const char *groupStateName(GroupState State);

struct GroupBudget {
  /// Active representative sampling tasks across all groups (the existing
  /// concurrency cap, now counted in groups rather than cells, §5.5).
  uint32_t maxActiveRepresentatives = 4;
  uint32_t maxGroups = 64;
  uint32_t maxWaitersPerGroup = 64;
  /// How many times a group may replace a failed/cold representative.
  uint32_t maxRepresentativeReselects = 2;
  uint64_t dispatchQuota = 64;
  uint64_t maxBundleBytes = 512 * 1024;
};

struct GroupStats {
  uint32_t groups = 0;
  uint32_t activeRepresentatives = 0;
  uint32_t frozenBundles = 0;
  uint32_t abortedGroups = 0;
  uint32_t waiters = 0;
  uint32_t reselects = 0;
  uint32_t rejectedMembers = 0;
  uint32_t lateSamplesRejected = 0;
  uint32_t admissionReleases = 0;
  uint64_t bundleBytes = 0;
};

struct GroupMemberResult {
  MemberRole role = MemberRole::Rejected;
  uint64_t groupId = 0;
  SamplingSessionId session;
  /// True when this call acquired the group's single admission slot, i.e. the
  /// caller must settle it exactly once via finishSampling/abortSampling.
  bool acquiredAdmission = false;
};

/// Owner-private group table. One representative per group samples; every other
/// member waits and is advanced by the frozen bundle rather than by a
/// per-member T1 completion event (§5.5).
class ReuseGroupTable {
public:
  explicit ReuseGroupTable(GroupBudget Budget = {}) : budget_(Budget) {}

  /// Join \p Logical to the group identified by \p GroupKey (the encoded
  /// ProfileGroupKey). A representative is admitted only when an admission slot
  /// is free and the group has no live representative; otherwise the member
  /// becomes a waiter, or is rejected when a waiter/group budget is exhausted.
  GroupMemberResult join(StringRef GroupKey, const LogicalKey &Logical,
                         RequestToken Token);

  /// Count one real T1 dispatch for \p Session. AOT fallback never reaches this
  /// function (§5.5). Returns false when the session is unknown or has already
  /// closed its quota. When the quota closes, `outQuotaClosed` is set and no
  /// further dispatch is admitted for that session.
  bool noteRepresentativeDispatch(const SamplingSessionId &Session,
                                  bool *OutQuotaClosed);

  /// Deliver a raw sample for \p Session. Rejected unless the session is the
  /// group's current collecting session: stale samplers from an older epoch or
  /// another group can never contaminate the new bundle (§5.6).
  bool acceptSample(const SamplingSessionId &Session, uint32_t Count);

  /// Freeze the immutable bundle for \p Session. Exactly once per session; the
  /// caller is told to release the admission this session holds.
  struct FreezeResult {
    bool accepted = false;
    bool duplicate = false;
    bool releaseAdmission = false;
  };
  FreezeResult freezeBundle(const SamplingSessionId &Session,
                            ProfileBundle Bundle);

  /// Abort sampling for \p Session (timeout, queue-full, failure). Settles the
  /// admission exactly once. A new representative may be reselected afterwards
  /// while the retry budget lasts.
  FreezeResult abortSampling(const SamplingSessionId &Session);

  /// Replace the representative of \p GroupId after failure/cold timeout. The
  /// new representative gets a new epoch and session; old partial counters are
  /// never mixed in. False when the retry budget is exhausted.
  bool reselectRepresentative(uint64_t GroupId, const LogicalKey &NewMember,
                              RequestToken Token, GroupMemberResult *Out);

  /// The frozen bundle for \p GroupId, or nullptr. Every member — the original
  /// representative, waiters and late joiners — reads this same object (§5.6).
  const ProfileBundle *bundle(uint64_t GroupId) const;

  GroupState state(uint64_t GroupId) const;
  const GroupStats &stats() const { return stats_; }
  const GroupBudget &budget() const { return budget_; }
  /// Number of waiters currently registered for \p GroupId.
  uint32_t waiterCount(uint64_t GroupId) const;
  /// Whether the group currently holds an active admission slot.
  bool holdsAdmission(uint64_t GroupId) const;

private:
  struct Group {
    uint64_t id = 0;
    std::string key;
    GroupState state = GroupState::Idle;
    /// Admission is held while a representative is sampling.
    bool admissionHeld = false;
    uint64_t epoch = 0;
    uint64_t session = 0;
    uint32_t waiters = 0;
    uint32_t reselects = 0;
    uint64_t dispatches = 0;
    bool quotaClosed = false;
    RequestToken representativeToken = kInvalidRequestToken;
    std::string representativeKey;
    ProfileBundle bundle;
    uint64_t bundleBytes = 0;
  };

  Group *find(uint64_t GroupId);
  const Group *find(uint64_t GroupId) const;
  Group *findBySession(const SamplingSessionId &Session);
  uint64_t registerGroup(StringRef GroupKey, GroupMemberResult &Out);

  GroupBudget budget_;
  uint64_t nextGroupId_ = 1;
  uint64_t nextSession_ = 1;
  std::vector<Group> groups_;
  GroupStats stats_;
};

} // namespace ejit
} // namespace llvm

#endif
