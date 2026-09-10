//===-- EJitCodeReuse.cpp - PR230 shared-version code reuse core ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitCodeReuse.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <algorithm>

using namespace llvm;
using namespace llvm::ejit;

//===----------------------------------------------------------------------===//
// Identity helpers
//===----------------------------------------------------------------------===//

/// Append a length-prefixed field. Length prefixing is what keeps two different
/// field sequences from encoding to the same text (e.g. {"ab","c"} vs
/// {"a","bc"}), which matters because these encodings are map keys and digests.
static void appendField(std::string &Out, StringRef Value) {
  Out += std::to_string(Value.size());
  Out += ':';
  Out.append(Value.data(), Value.size());
  Out += ';';
}

static void appendField(std::string &Out, uint64_t Value) {
  appendField(Out, std::to_string(Value));
}

static std::string digestOf(StringRef Bytes) {
  SHA256 Hasher;
  Hasher.update(Bytes);
  auto Result = Hasher.final();
  return toHex(ArrayRef<uint8_t>(Result.data(), Result.size()));
}

bool LogicalKey::operator==(const LogicalKey &Other) const {
  if (entry != Other.entry || sourceId != Other.sourceId ||
      runtimeGeneration != Other.runtimeGeneration ||
      dimensions.size() != Other.dimensions.size() ||
      lifecycleVersions.size() != Other.lifecycleVersions.size())
    return false;
  // Explicit loop: a generic contiguous search over uint32-sized elements is
  // what pulled wmemchr into the SRE link before (see the SRE memory notes).
  for (size_t I = 0; I < dimensions.size(); ++I)
    if (!(dimensions[I] == Other.dimensions[I]))
      return false;
  for (size_t I = 0; I < lifecycleVersions.size(); ++I)
    if (lifecycleVersions[I] != Other.lifecycleVersions[I])
      return false;
  return true;
}

std::string LogicalKey::encode() const {
  std::string Out;
  appendField(Out, entry);
  appendField(Out, sourceId);
  appendField(Out, static_cast<uint64_t>(dimensions.size()));
  for (const ReuseDim &Dim : dimensions) {
    appendField(Out, Dim.periodName);
    appendField(Out, Dim.instance);
  }
  appendField(Out, static_cast<uint64_t>(lifecycleVersions.size()));
  for (uint64_t Version : lifecycleVersions)
    appendField(Out, Version);
  appendField(Out, runtimeGeneration);
  return Out;
}

static bool bindingLess(const BindingEntry &L, const BindingEntry &R) {
  if (L.symbol != R.symbol)
    return L.symbol < R.symbol;
  if (L.kind != R.kind)
    return L.kind < R.kind;
  return L.address < R.address;
}

void BindingEnvironment::normalize() {
  llvm::sort(entries, bindingLess);
}

bool BindingEnvironment::operator==(const BindingEnvironment &Other) const {
  if (entries.size() != Other.entries.size())
    return false;
  for (size_t I = 0; I < entries.size(); ++I)
    if (!(entries[I] == Other.entries[I]))
      return false;
  return true;
}

std::string BindingEnvironment::encode() const {
  std::string Out;
  appendField(Out, static_cast<uint64_t>(entries.size()));
  for (const BindingEntry &Entry : entries) {
    appendField(Out, Entry.symbol);
    appendField(Out, Entry.address);
    appendField(Out, static_cast<uint64_t>(Entry.kind));
  }
  return Out;
}

bool PgoSchemaKey::operator==(const PgoSchemaKey &Other) const {
  if (policy != Other.policy || schemaDigest != Other.schemaDigest ||
      functionCount != Other.functionCount ||
      indirectCallSites != Other.indirectCallSites ||
      memOpSites != Other.memOpSites || scalarSites != Other.scalarSites ||
      disabledKinds.size() != Other.disabledKinds.size())
    return false;
  for (size_t I = 0; I < disabledKinds.size(); ++I)
    if (disabledKinds[I] != Other.disabledKinds[I])
      return false;
  return true;
}

std::string PgoSchemaKey::encode() const {
  std::string Out;
  appendField(Out, policy);
  appendField(Out, schemaDigest);
  appendField(Out, functionCount);
  appendField(Out, indirectCallSites);
  appendField(Out, memOpSites);
  appendField(Out, scalarSites);
  for (const std::string &Kind : disabledKinds)
    appendField(Out, Kind);
  return Out;
}

bool ProfileGroupKey::operator==(const ProfileGroupKey &Other) const {
  return sourceId == Other.sourceId && entry == Other.entry &&
         policy == Other.policy && prefixDigest == Other.prefixDigest &&
         schema == Other.schema && bindings == Other.bindings;
}

std::string ProfileGroupKey::encode() const {
  std::string Out;
  appendField(Out, sourceId);
  appendField(Out, entry);
  appendField(Out, policy);
  appendField(Out, prefixDigest);
  appendField(Out, schema.encode());
  appendField(Out, bindings.encode());
  return Out;
}

bool CodeKey::operator==(const CodeKey &Other) const {
  return sourceId == Other.sourceId && entry == Other.entry &&
         compilerPolicy == Other.compilerPolicy &&
         bindings == Other.bindings && finalIrDigest == Other.finalIrDigest;
}

std::string CodeKey::encode() const {
  std::string Out;
  appendField(Out, sourceId);
  appendField(Out, entry);
  appendField(Out, compilerPolicy);
  appendField(Out, bindings.encode());
  appendField(Out, finalIrDigest);
  return Out;
}

//===----------------------------------------------------------------------===//
// Canonical comparison image
//===----------------------------------------------------------------------===//

CanonicalModule llvm::ejit::canonicalizeModule(const Module &M) {
  CanonicalModule Result;
  std::unique_ptr<Module> Clone = CloneModule(M);

  // Module identity and debug info are not semantics.
  Clone->setModuleIdentifier("");
  Clone->setSourceFileName("");
  StripDebugInfo(*Clone);

  // Local display names are not semantics either, and two compilations of the
  // same code can number or name their locals differently. Clearing the names
  // makes the printer renumber every value consistently, so all references
  // follow; global/function names are symbol identity and are deliberately
  // kept.
  for (Function &F : *Clone) {
    if (F.isDeclaration())
      continue;
    for (Argument &Arg : F.args())
      Arg.setName("");
    for (BasicBlock &BB : F) {
      BB.setName("");
      for (Instruction &I : BB)
        I.setName("");
    }
  }

  std::string Text;
  raw_string_ostream OS(Text);
  Clone->print(OS, nullptr);
  OS.flush();
  Result.bytes = std::move(Text);
  Result.digest = digestOf(Result.bytes);
  return Result;
}

//===----------------------------------------------------------------------===//
// Code records
//===----------------------------------------------------------------------===//

const char *llvm::ejit::codeStateName(CodeState State) {
  switch (State) {
  case CodeState::Building:
    return "building";
  case CodeState::LinkedPending:
    return "linked-pending";
  case CodeState::Published:
    return "published";
  case CodeState::Failed:
    return "failed";
  }
  return "unknown";
}

uint64_t CodeRecord::execBytes() const {
  uint64_t Total = 0;
  for (const ReuseRange &Range : execRanges)
    Total += Range.size;
  return Total;
}

CodeReuseTable::AddResult
CodeReuseTable::addCandidate(const CodeKey &Key, CanonicalModule FinalIr) {
  if (records_.size() >= budget_.maxCodeRecords ||
      stats_.canonicalBytes + FinalIr.bytes.size() >
          budget_.maxCanonicalBytes) {
    ++stats_.budgetRejections;
    return {};
  }
  CodeRecord Record;
  Record.codeId = nextCodeId_++;
  Record.key = Key;
  Record.finalIr = std::move(FinalIr);
  records_.push_back(std::move(Record));
  rebuildDigestIndex();
  recomputeStats();
  return {true, records_.back().codeId};
}

ReuseLookup CodeReuseTable::lookup(const CodeKey &Key,
                                   StringRef CanonicalBytes) {
  ReuseLookup Result;
  // The digest is an index into the candidate buckets, never the proof.
  for (const auto &Bucket : digestIndex_) {
    if (Bucket.first != Key.finalIrDigest)
      continue;
    for (uint32_t Index : Bucket.second) {
      const CodeRecord &Record = records_[Index];
      // Same entry/source identity only (§5.2): never share across entries or
      // source revisions, however similar the IR looks.
      if (Record.key.sourceId != Key.sourceId ||
          Record.key.entry != Key.entry ||
          Record.key.compilerPolicy != Key.compilerPolicy)
        continue;
      if (Record.key.bindings != Key.bindings ||
          Record.finalIr.bytes != CanonicalBytes) {
        // Digest match without exact match. Record the failed pair so the same
        // candidate is not retried forever (§5.4), and never reuse it.
        Result.digestCollision = true;
        recordFailedMatch(Key, CanonicalBytes);
        ++stats_.digestCollisions;
        continue;
      }
      if (Record.state == CodeState::Published) {
        Result.decision = ReuseDecision::ReusedPublished;
        Result.codeId = Record.codeId;
        return Result;
      }
      if (Record.state == CodeState::LinkedPending ||
          Record.state == CodeState::Building) {
        Result.decision = ReuseDecision::ReusedPending;
        Result.codeId = Record.codeId;
        return Result;
      }
      // Failed records are not reusable; keep looking for another candidate.
    }
  }
  Result.decision = ReuseDecision::CompileIndependently;
  return Result;
}

void CodeReuseTable::recordFailedMatch(const CodeKey &Key,
                                       StringRef CanonicalBytes) {
  if (failedMatches_.size() >= budget_.maxFailedMatches)
    return;
  const std::string Encoded = Key.encode();
  for (const auto &Match : failedMatches_)
    if (Match.first == Encoded)
      return;
  if (failedMatchBytes_ + CanonicalBytes.size() > budget_.maxFailedMatchBytes)
    return;
  failedMatches_.emplace_back(Encoded, digestOf(CanonicalBytes));
  failedMatchBytes_ += CanonicalBytes.size();
  recomputeStats();
}

bool CodeReuseTable::markLinked(uint64_t CodeId, uint64_t EntryAddress,
                                ArrayRef<ReuseRange> ExecRanges,
                                ArrayRef<ReuseRange> DataRanges,
                                StringRef Pool) {
  for (CodeRecord &Record : records_) {
    if (Record.codeId != CodeId)
      continue;
    if (Record.state != CodeState::Building)
      return false;
    Record.entryAddress = EntryAddress;
    Record.execRanges.assign(ExecRanges.begin(), ExecRanges.end());
    Record.dataRanges.assign(DataRanges.begin(), DataRanges.end());
    Record.pool = Pool.str();
    Record.state = CodeState::LinkedPending;
    return true;
  }
  return false;
}

bool CodeReuseTable::markPublished(uint64_t CodeId) {
  for (CodeRecord &Record : records_) {
    if (Record.codeId != CodeId)
      continue;
    // Idempotent: a repeated publication callback is a no-op, not an error and
    // not a second allocation.
    if (Record.state == CodeState::Published)
      return true;
    if (Record.state != CodeState::LinkedPending)
      return false;
    Record.state = CodeState::Published;
    return true;
  }
  return false;
}

bool CodeReuseTable::markFailed(uint64_t CodeId) {
  for (CodeRecord &Record : records_) {
    if (Record.codeId != CodeId)
      continue;
    if (Record.state == CodeState::Published)
      return false;
    Record.state = CodeState::Failed;
    return true;
  }
  return false;
}

const CodeRecord *CodeReuseTable::getRecord(uint64_t CodeId) const {
  for (const CodeRecord &Record : records_)
    if (Record.codeId == CodeId)
      return &Record;
  return nullptr;
}

bool CodeReuseTable::bindLogical(const LogicalKey &Logical, uint64_t CodeId) {
  const CodeRecord *Record = getRecord(CodeId);
  if (!Record)
    return false;
  const std::string Encoded = Logical.encode();
  for (auto &Binding : logicalBindings_) {
    if (Binding.first != Encoded)
      continue;
    if (Binding.second == CodeId)
      return true;
    // Re-pointing an existing logical version is allowed (a config change may
    // produce a different emission) and does not disturb other members.
    Binding.second = CodeId;
    recomputeStats();
    return true;
  }
  if (logicalBindings_.size() >= budget_.maxLogicalBindings) {
    ++stats_.budgetRejections;
    return false;
  }
  logicalBindings_.emplace_back(Encoded, CodeId);
  recomputeStats();
  return true;
}

bool CodeReuseTable::unbindLogical(const LogicalKey &Logical) {
  const std::string Encoded = Logical.encode();
  for (size_t I = 0; I < logicalBindings_.size(); ++I) {
    if (logicalBindings_[I].first != Encoded)
      continue;
    // Independent invalidation: the physical record and every other logical
    // binding are untouched; NO_RECLAIM keeps the code alive (§6.3).
    logicalBindings_.erase(logicalBindings_.begin() + I);
    recomputeStats();
    return true;
  }
  return false;
}

uint64_t CodeReuseTable::codeIdFor(const LogicalKey &Logical) const {
  const std::string Encoded = Logical.encode();
  for (const auto &Binding : logicalBindings_)
    if (Binding.first == Encoded)
      return Binding.second;
  return 0;
}

uint32_t CodeReuseTable::logicalRefs(uint64_t CodeId) const {
  uint32_t Count = 0;
  for (const auto &Binding : logicalBindings_)
    if (Binding.second == CodeId)
      ++Count;
  return Count;
}

void CodeReuseTable::rebuildDigestIndex() {
  digestIndex_.clear();
  for (uint32_t I = 0; I < records_.size(); ++I) {
    const std::string &Digest = records_[I].key.finalIrDigest;
    bool Found = false;
    for (auto &Bucket : digestIndex_) {
      if (Bucket.first != Digest)
        continue;
      Bucket.second.push_back(I);
      Found = true;
      break;
    }
    if (!Found)
      digestIndex_.emplace_back(Digest, std::vector<uint32_t>{I});
  }
}

void CodeReuseTable::recomputeStats() {
  stats_.codeRecords = static_cast<uint32_t>(records_.size());
  stats_.logicalBindings = static_cast<uint32_t>(logicalBindings_.size());
  stats_.failedMatches = static_cast<uint32_t>(failedMatches_.size());
  stats_.canonicalBytes = 0;
  stats_.execBytesUnique = 0;
  for (const CodeRecord &Record : records_) {
    stats_.canonicalBytes += Record.finalIr.bytes.size();
    // Physical ranges are counted once per record, never once per logical
    // version (§7): a shared emission must not inflate the byte totals.
    if (Record.state == CodeState::Published ||
        Record.state == CodeState::LinkedPending)
      stats_.execBytesUnique += Record.execBytes();
  }
  // reused_versions counts logical versions that share a physical record with
  // at least one other version.
  stats_.reusedVersions = 0;
  for (const auto &Binding : logicalBindings_)
    if (logicalRefs(Binding.second) > 1)
      ++stats_.reusedVersions;
}

//===----------------------------------------------------------------------===//
// Request attempts
//===----------------------------------------------------------------------===//

RequestToken ReuseRequestRegistry::begin(const LogicalKey &Logical) {
  reapFinished();
  const std::string Encoded = Logical.encode();
  RequestToken Token = 0;
  // A token must never be confusable with a callback from an attempt that is
  // still able to fire, so it includes the owner generation and a sequence that
  // is never reused.
  while (Token == kInvalidRequestToken) {
    Token = (ownerGeneration_ << 32) | (nextSequence_++ & 0xffffffffu);
    if ((Token >> 32) != ownerGeneration_)
      Token = kInvalidRequestToken; // sequence would alias the generation
  }
  if (attempts_.size() >= budget_.maxLiveAttempts) {
    // Reap first; if there is still no room the caller must treat the request
    // as failed and fall back rather than growing the table (§6.5).
    return kInvalidRequestToken;
  }
  AttemptRecord Record;
  Record.token = Token;
  Record.logicalKey = Encoded;
  attempts_.push_back(std::move(Record));
  ++liveCount_;

  // Supersede the previous attempt for this logical key. The old attempt stays
  // alive: its own callbacks still settle its own resources, they just cannot
  // touch this one.
  bool Replaced = false;
  for (auto &Current : currentByLogical_) {
    if (Current.first != Encoded)
      continue;
    Current.second = Token;
    Replaced = true;
    break;
  }
  if (!Replaced)
    currentByLogical_.emplace_back(Encoded, Token);
  return Token;
}

AttemptRecord *ReuseRequestRegistry::find(RequestToken Token) {
  for (AttemptRecord &Record : attempts_)
    if (Record.token == Token)
      return &Record;
  return nullptr;
}

const AttemptRecord *ReuseRequestRegistry::find(RequestToken Token) const {
  for (const AttemptRecord &Record : attempts_)
    if (Record.token == Token)
      return &Record;
  return nullptr;
}

bool ReuseRequestRegistry::cancel(RequestToken Token) {
  AttemptRecord *Record = find(Token);
  if (!Record)
    return false;
  if (Record->state != AttemptState::Active) {
    return Record->cancelRequested;
  }
  // Cancelling closes future reads and publication, but the attempt is not
  // finished until its borrow is confirmed ended (§5.8).
  Record->state = AttemptState::Cancelling;
  Record->cancelRequested = true;
  return true;
}

EventResult ReuseRequestRegistry::finishSampling(RequestToken Token,
                                                 bool Aborted) {
  EventResult Result;
  AttemptRecord *Record = find(Token);
  if (!Record)
    return Result;
  if (Record->samplingSettled) {
    Result.duplicate = true;
    return Result;
  }
  Record->samplingSettled = true;
  Record->samplingAborted = Aborted;
  Result.accepted = true;
  // A nonrepresentative holds no admission, so it is never released here; the
  // attempt's own `holdsAdmission` flag is what makes the release exactly once.
  if (Record->holdsAdmission) {
    Record->holdsAdmission = false;
    Result.releaseAdmission = true;
  }
  if (Record->state == AttemptState::Finished)
    Result.attemptFinished = true;
  reapFinished();
  return Result;
}

EventResult ReuseRequestRegistry::endCompileBorrow(RequestToken Token) {
  EventResult Result;
  AttemptRecord *Record = find(Token);
  if (!Record)
    return Result;
  if (Record->borrowEnded) {
    Result.duplicate = true;
    return Result;
  }
  Record->borrowEnded = true;
  Record->holdsBorrow = false;
  Result.accepted = true;
  Result.borrowConfirmed = true;
  // Compile borrow end does not retire the business object (§3.5) and does not
  // by itself finish an active attempt: a live one continues to publication.
  if (Record->state == AttemptState::Cancelling) {
    Record->state = AttemptState::Finished;
    Result.attemptFinished = true;
  }
  reapFinished();
  return Result;
}

EventResult ReuseRequestRegistry::publishLogical(RequestToken Token,
                                                 uint64_t CodeId) {
  EventResult Result;
  AttemptRecord *Record = find(Token);
  if (!Record)
    return Result;
  if (Record->published) {
    Result.duplicate = true;
    return Result;
  }
  if (Record->cancelRequested)
    return Result; // a cancelled attempt is never reported as published
  Record->published = true;
  Record->publishedCodeId = CodeId;
  Result.accepted = true;
  Result.logicalSettled = true;
  // Publishing settles the logical mapping only; it never releases the group
  // admission a second time.
  if (Record->state == AttemptState::Active)
    Record->state = AttemptState::Finished;
  Result.attemptFinished = true;
  reapFinished();
  return Result;
}

void ReuseRequestRegistry::setHoldsAdmission(RequestToken Token, bool Holds) {
  if (AttemptRecord *Record = find(Token))
    Record->holdsAdmission = Holds;
}

void ReuseRequestRegistry::setHoldsWaiter(RequestToken Token, bool Holds) {
  if (AttemptRecord *Record = find(Token))
    Record->holdsWaiter = Holds;
}

AttemptState ReuseRequestRegistry::state(RequestToken Token) const {
  const AttemptRecord *Record = find(Token);
  return Record ? Record->state : AttemptState::Finished;
}

const AttemptRecord *ReuseRequestRegistry::record(RequestToken Token) const {
  return find(Token);
}

bool ReuseRequestRegistry::isCurrent(RequestToken Token) const {
  const AttemptRecord *Record = find(Token);
  if (!Record)
    return false;
  for (const auto &Current : currentByLogical_)
    if (Current.first == Record->logicalKey)
      return Current.second == Token;
  return false;
}

RequestToken ReuseRequestRegistry::currentFor(const LogicalKey &Logical) const {
  const std::string Encoded = Logical.encode();
  for (const auto &Current : currentByLogical_)
    if (Current.first == Encoded)
      return Current.second;
  return kInvalidRequestToken;
}

void ReuseRequestRegistry::reapFinished() {
  uint32_t Finished = 0;
  for (const AttemptRecord &Record : attempts_)
    if (Record.state == AttemptState::Finished)
      ++Finished;
  if (Finished <= budget_.maxFinishedHistory)
    return;
  for (size_t I = 0; I < attempts_.size();) {
    if (attempts_[I].state != AttemptState::Finished) {
      ++I;
      continue;
    }
    attempts_.erase(attempts_.begin() + I);
    --liveCount_;
    --Finished;
    if (Finished <= budget_.maxFinishedHistory)
      break;
  }
}

//===----------------------------------------------------------------------===//
// Sampling sessions and bundles
//===----------------------------------------------------------------------===//

std::string SamplingSessionId::encode() const {
  std::string Out;
  appendField(Out, groupId);
  appendField(Out, profileEpoch);
  appendField(Out, session);
  return Out;
}

std::string SamplingQuality::encode() const {
  std::string Out;
  appendField(Out, dispatchCount);
  appendField(Out, dispatchQuota);
  appendField(Out, quotaEndTick);
  appendField(Out, freezeCompletedTick);
  appendField(Out, static_cast<uint64_t>(missingShards));
  appendField(Out, static_cast<uint64_t>(approximate ? 1 : 0));
  appendField(Out, static_cast<uint64_t>(countBoundaryUnknown ? 1 : 0));
  return Out;
}

std::string ProfileBundle::encode() const {
  std::string Out;
  appendField(Out, indexedProfile);
  appendField(Out, static_cast<uint64_t>(scalarSites.size()));
  for (const std::string &Site : scalarSites)
    appendField(Out, Site);
  appendField(Out, schema.encode());
  appendField(Out, targetMappingDigest);
  appendField(Out, session.encode());
  appendField(Out, quality.encode());
  appendField(Out, representative.encode());
  return Out;
}

//===----------------------------------------------------------------------===//
// Group table
//===----------------------------------------------------------------------===//

const char *llvm::ejit::groupStateName(GroupState State) {
  switch (State) {
  case GroupState::Idle:
    return "idle";
  case GroupState::Collecting:
    return "collecting";
  case GroupState::BundleFrozen:
    return "bundle-frozen";
  case GroupState::SamplingAborted:
    return "sampling-aborted";
  }
  return "unknown";
}

ReuseGroupTable::Group *ReuseGroupTable::find(uint64_t GroupId) {
  for (Group &G : groups_)
    if (G.id == GroupId)
      return &G;
  return nullptr;
}

const ReuseGroupTable::Group *ReuseGroupTable::find(uint64_t GroupId) const {
  for (const Group &G : groups_)
    if (G.id == GroupId)
      return &G;
  return nullptr;
}

ReuseGroupTable::Group *
ReuseGroupTable::findBySession(const SamplingSessionId &Session) {
  for (Group &G : groups_)
    if (G.id == Session.groupId && G.epoch == Session.profileEpoch &&
        G.session == Session.session)
      return &G;
  return nullptr;
}

uint64_t ReuseGroupTable::registerGroup(StringRef GroupKey,
                                        GroupMemberResult &Out) {
  if (groups_.size() >= budget_.maxGroups) {
    ++stats_.rejectedMembers;
    Out.role = MemberRole::Rejected;
    return 0;
  }
  Group New;
  New.id = nextGroupId_++;
  New.key = GroupKey.str();
  groups_.push_back(std::move(New));
  stats_.groups = static_cast<uint32_t>(groups_.size());
  Out.groupId = groups_.back().id;
  return groups_.back().id;
}

GroupMemberResult ReuseGroupTable::join(StringRef GroupKey,
                                        const LogicalKey &Logical,
                                        RequestToken Token) {
  GroupMemberResult Result;
  Group *Target = nullptr;
  for (Group &G : groups_)
    if (G.key == GroupKey) {
      Target = &G;
      break;
    }
  if (!Target) {
    if (registerGroup(GroupKey, Result) == 0)
      return Result; // budget exhausted -> AOT
    Target = &groups_.back();
  }
  Result.groupId = Target->id;
  Result.session = {Target->id, Target->epoch, Target->session};

  // A frozen group serves the bundle to everyone, including late joiners.
  if (Target->state == GroupState::BundleFrozen) {
    Result.role = MemberRole::Waiter;
    return Result;
  }
  if (Target->state == GroupState::SamplingAborted) {
    Result.role = MemberRole::Rejected;
    ++stats_.rejectedMembers;
    return Result;
  }

  const bool WantsRepresentative = Target->state == GroupState::Idle;
  if (WantsRepresentative &&
      stats_.activeRepresentatives < budget_.maxActiveRepresentatives) {
    Target->state = GroupState::Collecting;
    Target->admissionHeld = true;
    Target->representativeToken = Token;
    Target->representativeKey = Logical.encode();
    ++stats_.activeRepresentatives;
    Result.role = MemberRole::Representative;
    Result.acquiredAdmission = true;
    return Result;
  }

  if (Target->waiters >= budget_.maxWaitersPerGroup) {
    // Nonrepresentatives must not accumulate without bound; the member falls
    // back to AOT instead of opening a parallel compile (§5.5, §6.5).
    Result.role = MemberRole::Rejected;
    ++stats_.rejectedMembers;
    return Result;
  }
  ++Target->waiters;
  ++stats_.waiters;
  Result.role = MemberRole::Waiter;
  return Result;
}

bool ReuseGroupTable::noteRepresentativeDispatch(
    const SamplingSessionId &Session, bool *OutQuotaClosed) {
  if (OutQuotaClosed)
    *OutQuotaClosed = false;
  Group *Target = findBySession(Session);
  if (!Target || Target->state != GroupState::Collecting)
    return false;
  if (Target->quotaClosed)
    return false;
  ++Target->dispatches;
  if (Target->dispatches >= budget_.dispatchQuota) {
    // The quota counts real T1 entries. Once it closes, no new dispatch is
    // admitted for this session; the snapshot that follows is explicitly
    // approximate because calls already in flight may still be running (§5.7).
    Target->quotaClosed = true;
    if (OutQuotaClosed)
      *OutQuotaClosed = true;
  }
  return true;
}

bool ReuseGroupTable::acceptSample(const SamplingSessionId &Session,
                                   uint32_t Count) {
  Group *Target = findBySession(Session);
  if (!Target)
    return false;
  // A sample from an older epoch/session, or one that arrives after the freeze,
  // is isolated from the bundle: it is dropped and counted, never merged.
  if (Target->state != GroupState::Collecting) {
    ++stats_.lateSamplesRejected;
    return false;
  }
  (void)Count;
  return true;
}

ReuseGroupTable::FreezeResult
ReuseGroupTable::freezeBundle(const SamplingSessionId &Session,
                              ProfileBundle Bundle) {
  FreezeResult Result;
  Group *Target = findBySession(Session);
  if (!Target)
    return Result;
  if (Target->state == GroupState::BundleFrozen) {
    Result.duplicate = true;
    return Result;
  }
  if (Target->state != GroupState::Collecting)
    return Result;

  Bundle.session = Session;
  Bundle.frozen = true;
  Target->bundle = std::move(Bundle);
  Target->bundleBytes = Target->bundle.encode().size();
  if (stats_.bundleBytes + Target->bundleBytes > budget_.maxBundleBytes) {
    // Exhaustion is explicit: the group ends without a bundle and its members
    // use AOT. Nothing is silently trimmed into a half-valid profile. The
    // freeze itself did not happen, but the admission must still be returned.
    Target->bundle = ProfileBundle();
    Target->bundleBytes = 0;
    FreezeResult Aborted = abortSampling(Session);
    Aborted.accepted = false;
    return Aborted;
  }
  stats_.bundleBytes += Target->bundleBytes;
  Target->state = GroupState::BundleFrozen;
  ++stats_.frozenBundles;
  if (Target->admissionHeld) {
    Target->admissionHeld = false;
    --stats_.activeRepresentatives;
    ++stats_.admissionReleases;
    Result.releaseAdmission = true;
  }
  Result.accepted = true;
  // Waiters are advanced by the bundle becoming ready, never by waiting for a
  // per-member T1 completion event that does not exist for them (§5.5).
  return Result;
}

ReuseGroupTable::FreezeResult
ReuseGroupTable::abortSampling(const SamplingSessionId &Session) {
  FreezeResult Result;
  Group *Target = findBySession(Session);
  if (!Target)
    return Result;
  if (Target->state == GroupState::SamplingAborted) {
    Result.duplicate = true;
    return Result;
  }
  Target->state = GroupState::SamplingAborted;
  ++stats_.abortedGroups;
  if (Target->admissionHeld) {
    Target->admissionHeld = false;
    --stats_.activeRepresentatives;
    ++stats_.admissionReleases;
    Result.releaseAdmission = true;
  }
  Result.accepted = true;
  return Result;
}

bool ReuseGroupTable::reselectRepresentative(uint64_t GroupId,
                                             const LogicalKey &NewMember,
                                             RequestToken Token,
                                             GroupMemberResult *Out) {
  Group *Target = find(GroupId);
  if (!Target)
    return false;
  if (Target->reselects >= budget_.maxRepresentativeReselects)
    return false;
  if (Target->state == GroupState::BundleFrozen)
    return false;
  if (stats_.activeRepresentatives >= budget_.maxActiveRepresentatives &&
      !Target->admissionHeld)
    return false;

  ++Target->reselects;
  ++stats_.reselects;
  // New epoch and session: the old partial counters are never mixed into the
  // new round, and the old session's late writes are rejected by session id.
  ++Target->epoch;
  Target->session = nextSession_++;
  Target->dispatches = 0;
  Target->quotaClosed = false;
  Target->state = GroupState::Collecting;
  Target->representativeToken = Token;
  Target->representativeKey = NewMember.encode();
  if (!Target->admissionHeld) {
    Target->admissionHeld = true;
    ++stats_.activeRepresentatives;
  }
  if (Out) {
    Out->role = MemberRole::Representative;
    Out->groupId = Target->id;
    Out->session = {Target->id, Target->epoch, Target->session};
    Out->acquiredAdmission = false; // the group already held the slot
  }
  return true;
}

const ProfileBundle *ReuseGroupTable::bundle(uint64_t GroupId) const {
  const Group *Target = find(GroupId);
  if (!Target || Target->state != GroupState::BundleFrozen)
    return nullptr;
  return &Target->bundle;
}

GroupState ReuseGroupTable::state(uint64_t GroupId) const {
  const Group *Target = find(GroupId);
  return Target ? Target->state : GroupState::Idle;
}

uint32_t ReuseGroupTable::waiterCount(uint64_t GroupId) const {
  const Group *Target = find(GroupId);
  return Target ? Target->waiters : 0;
}

bool ReuseGroupTable::holdsAdmission(uint64_t GroupId) const {
  const Group *Target = find(GroupId);
  return Target ? Target->admissionHeld : false;
}
