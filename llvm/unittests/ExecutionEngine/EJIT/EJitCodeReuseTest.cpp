//===-- EJitCodeReuseTest.cpp - PR230 code-reuse core unit tests ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// These tests pin the cold-path contracts of EJIT_VERSION_CODE_REUSE_SPEC.md:
// identity separation (§5.2), digest-as-index with exact comparison (§5.3),
// independent logical invalidation and exact physical byte accounting (§6.3,
// §7), request-attempt isolation and the three idempotent events (§5.8),
// representative-only sampling with bounded budgets (§5.5, §5.6) and the
// frozen-bundle contract (§5.7).
//
// The tests drive the state machine directly. They do not claim ORC/JITLink or
// SRE behavior: nothing here executes JIT code.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitCodeReuse.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"

#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::ejit;

namespace {

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

static std::unique_ptr<Module> parse(StringRef IR, LLVMContext &Ctx) {
  SMDiagnostic Err;
  auto M = parseAssemblyString(IR, Err, Ctx);
  if (!M)
    Err.print("EJitCodeReuseTest", errs());
  return M;
}

static LogicalKey makeLogical(StringRef Entry, unsigned Cell, unsigned Trp,
                              uint64_t Generation = 7) {
  LogicalKey Key;
  Key.entry = Entry.str();
  Key.sourceId = "src-rev-1";
  Key.dimensions.push_back({"cell", Cell});
  Key.dimensions.push_back({"trp", Trp});
  Key.lifecycleVersions.push_back(0);
  Key.lifecycleVersions.push_back(0);
  Key.runtimeGeneration = Generation;
  return Key;
}

static BindingEnvironment makeBindings(StringRef Symbol, uint64_t Address) {
  BindingEnvironment Env;
  BindingEntry Entry;
  Entry.symbol = Symbol.str();
  Entry.address = Address;
  Entry.kind = 0;
  Env.entries.push_back(Entry);
  Env.normalize();
  return Env;
}

static CodeKey makeCodeKey(StringRef Entry, StringRef IrDigest,
                           uint64_t SymbolAddress = 0x1000) {
  CodeKey Key;
  Key.sourceId = "src-rev-1";
  Key.entry = Entry.str();
  Key.compilerPolicy = "async+pgo+shared-v1";
  Key.bindings = makeBindings("helper", SymbolAddress);
  Key.finalIrDigest = IrDigest.str();
  return Key;
}

//===----------------------------------------------------------------------===//
// §5.3 — canonical comparison image
//===----------------------------------------------------------------------===//

TEST(EJitCodeReuse, CanonicalImageIgnoresNamesAndPaths) {
  LLVMContext Ctx;
  const char *A = R"(
    define i32 @f(i32 %cell, i32 %x) {
    entry:
      %gain = add i32 %cell, 1
      %r = mul i32 %gain, %x
      ret i32 %r
    }
  )";
  // Same code: different module id, different local names, plus debug info.
  const char *B = R"(
    define i32 @f(i32 %c, i32 %y) !dbg !4 {
    entry:
      %0 = add i32 %c, 1
      %1 = mul i32 %0, %y
      ret i32 %1
    }
    !llvm.dbg.cu = !{!0}
    !0 = distinct !DICompileUnit(language: DW_LANG_C, file: !1,
                                 producer: "x", isOptimized: false,
                                 runtimeVersion: 0, emissionKind: FullDebug)
    !1 = !DIFile(filename: "a.c", directory: "/tmp")
    !4 = distinct !DISubprogram(name: "f", scope: !1, file: !1, line: 1,
                                type: !5, scopeLine: 1,
                                spFlags: DISPFlagDefinition, unit: !0)
    !5 = !DISubroutineType(types: !6)
    !6 = !{}
  )";
  auto MA = parse(A, Ctx);
  auto MB = parse(B, Ctx);
  ASSERT_TRUE(MA);
  ASSERT_TRUE(MB);
  MA->setModuleIdentifier("/build/one.bc");
  MB->setModuleIdentifier("/other/two.bc");
  MB->setSourceFileName("b.c");

  CanonicalModule CA = canonicalizeModule(*MA);
  CanonicalModule CB = canonicalizeModule(*MB);
  EXPECT_EQ(CA.digest, CB.digest);
  EXPECT_EQ(CA.bytes, CB.bytes);
}

TEST(EJitCodeReuse, CanonicalImageKeepsSemantics) {
  LLVMContext Ctx;
  auto MA = parse(R"(
    define i32 @f(i32 %x) {
    entry:
      %r = add i32 %x, 1
      ret i32 %r
    }
  )",
                  Ctx);
  auto MB = parse(R"(
    define i32 @f(i32 %x) {
    entry:
      %r = add i32 %x, 2
      ret i32 %r
    }
  )",
                  Ctx);
  ASSERT_TRUE(MA);
  ASSERT_TRUE(MB);
  CanonicalModule CA = canonicalizeModule(*MA);
  CanonicalModule CB = canonicalizeModule(*MB);
  // A folded instance value shows up here, so two configurations that produced
  // different IR can never compare equal.
  EXPECT_NE(CA.digest, CB.digest);
  EXPECT_NE(CA.bytes, CB.bytes);
}

TEST(EJitCodeReuse, CanonicalImageKeepsMetadataAndAttributes) {
  LLVMContext Ctx;
  auto MA = parse(R"(
    define i32 @f(i32 %x) noinline {
    entry:
      %r = add i32 %x, 1
      ret i32 %r
    }
  )",
                  Ctx);
  auto MB = parse(R"(
    define i32 @f(i32 %x) {
    entry:
      %r = add i32 %x, 1
      ret i32 %r
    }
  )",
                  Ctx);
  ASSERT_TRUE(MA);
  ASSERT_TRUE(MB);
  EXPECT_NE(canonicalizeModule(*MA).digest, canonicalizeModule(*MB).digest);
}

//===----------------------------------------------------------------------===//
// §5.2/§5.3 — reuse table: exact proof, collisions, independent invalidation
//===----------------------------------------------------------------------===//

TEST(EJitCodeReuse, ReusesOnlyOnExactMatch) {
  CodeReuseTable Table;
  const std::string Bytes = "canonical-ir-A";
  const std::string Digest = "digest-A";
  auto Add = Table.addCandidate(makeCodeKey("f", Digest), {Digest, Bytes});
  ASSERT_TRUE(Add.ok);
  ASSERT_TRUE(Table.markLinked(Add.codeId, 0x4000, {{0x4000, 64}}, {}, "near"));
  ASSERT_TRUE(Table.markPublished(Add.codeId));

  ReuseLookup Hit = Table.lookup(makeCodeKey("f", Digest), Bytes);
  EXPECT_EQ(Hit.decision, ReuseDecision::ReusedPublished);
  EXPECT_EQ(Hit.codeId, Add.codeId);
  EXPECT_FALSE(Hit.digestCollision);

  // Same digest, different bytes: a collision must compile independently.
  ReuseLookup Collision =
      Table.lookup(makeCodeKey("f", Digest), "canonical-ir-B");
  EXPECT_EQ(Collision.decision, ReuseDecision::CompileIndependently);
  EXPECT_TRUE(Collision.digestCollision);
  EXPECT_EQ(Table.stats().digestCollisions, 1u);
  EXPECT_EQ(Table.stats().failedMatches, 1u);

  // A different binding environment is not a match either, even with identical
  // IR bytes.
  CodeKey OtherBinding = makeCodeKey("f", Digest, /*SymbolAddress=*/0x9000);
  ReuseLookup BindingMiss = Table.lookup(OtherBinding, Bytes);
  EXPECT_EQ(BindingMiss.decision, ReuseDecision::CompileIndependently);

  // Different entry: never shared.
  ReuseLookup EntryMiss = Table.lookup(makeCodeKey("g", Digest), Bytes);
  EXPECT_EQ(EntryMiss.decision, ReuseDecision::CompileIndependently);

  CodeKey OtherSource = makeCodeKey("f", Digest);
  OtherSource.sourceId = "src-rev-2";
  EXPECT_EQ(Table.lookup(OtherSource, Bytes).decision,
            ReuseDecision::CompileIndependently);
}

TEST(EJitCodeReuse, PendingRecordIsReusedAsWaiter) {
  CodeReuseTable Table;
  auto Add = Table.addCandidate(makeCodeKey("f", "d1"), {"d1", "ir"});
  ASSERT_TRUE(Add.ok);
  // Linked but not published: a second logical version must wait, not receive
  // an address and not allocate another page.
  ASSERT_TRUE(Table.markLinked(Add.codeId, 0x5000, {{0x5000, 32}}, {}, "near"));
  ReuseLookup Pending = Table.lookup(makeCodeKey("f", "d1"), "ir");
  EXPECT_EQ(Pending.decision, ReuseDecision::ReusedPending);
  EXPECT_EQ(Pending.codeId, Add.codeId);
}

TEST(EJitCodeReuse, IndependentLogicalInvalidation) {
  CodeReuseTable Table;
  auto Add = Table.addCandidate(makeCodeKey("f", "d1"), {"d1", "ir"});
  ASSERT_TRUE(Add.ok);
  ASSERT_TRUE(Table.markLinked(Add.codeId, 0x5000, {{0x5000, 32}}, {}, "near"));
  ASSERT_TRUE(Table.markPublished(Add.codeId));

  LogicalKey L0 = makeLogical("f", 0, 0);
  LogicalKey L1 = makeLogical("f", 1, 0);
  ASSERT_TRUE(Table.bindLogical(L0, Add.codeId));
  ASSERT_TRUE(Table.bindLogical(L1, Add.codeId));
  EXPECT_EQ(Table.logicalRefs(Add.codeId), 2u);

  // cell0 changes configuration: only its logical mapping goes away.
  ASSERT_TRUE(Table.unbindLogical(L0));
  EXPECT_EQ(Table.codeIdFor(L0), 0u);
  EXPECT_EQ(Table.codeIdFor(L1), Add.codeId);
  ASSERT_NE(Table.getRecord(Add.codeId), nullptr);
  EXPECT_EQ(Table.getRecord(Add.codeId)->state, CodeState::Published);
  EXPECT_EQ(Table.logicalRefs(Add.codeId), 1u);

  // Re-binding the same physical record after re-grouping is a normal path.
  ASSERT_TRUE(Table.bindLogical(L0, Add.codeId));
  EXPECT_EQ(Table.stats().codeRecords, 1u);
  // Physical bytes are counted once even with three logical references.
  EXPECT_EQ(Table.stats().execBytesUnique, 32u);
}

TEST(EJitCodeReuse, BudgetExhaustionReportsInsteadOfGrowing) {
  ReuseTableBudget Budget;
  Budget.maxCodeRecords = 2;
  CodeReuseTable Table(Budget);
  ASSERT_TRUE(Table.addCandidate(makeCodeKey("a", "d"), {"d", "ir-a"}).ok);
  ASSERT_TRUE(Table.addCandidate(makeCodeKey("b", "d"), {"d", "ir-b"}).ok);
  auto Third = Table.addCandidate(makeCodeKey("c", "d"), {"d", "ir-c"});
  EXPECT_FALSE(Third.ok);
  EXPECT_EQ(Table.stats().budgetRejections, 1u);

  ReuseTableBudget Bytes;
  Bytes.maxCanonicalBytes = 4;
  CodeReuseTable Small(Bytes);
  EXPECT_FALSE(
      Small.addCandidate(makeCodeKey("a", "d"), {"d", "0123456789"}).ok);
}

TEST(EJitCodeReuse, PublicationIsIdempotentAndNeverDowngrades) {
  CodeReuseTable Table;
  auto Add = Table.addCandidate(makeCodeKey("f", "d"), {"d", "ir"});
  ASSERT_TRUE(Add.ok);
  EXPECT_FALSE(Table.markPublished(Add.codeId)) << "cannot publish before link";
  ASSERT_TRUE(Table.markLinked(Add.codeId, 0x10, {{0x10, 8}}, {}, "near"));
  ASSERT_TRUE(Table.markPublished(Add.codeId));
  EXPECT_TRUE(Table.markPublished(Add.codeId))
      << "repeat publication is a no-op";
  EXPECT_FALSE(Table.markFailed(Add.codeId)) << "published code is not failed";
}

//===----------------------------------------------------------------------===//
// §5.8 — request attempts and the three events
//===----------------------------------------------------------------------===//

TEST(EJitCodeReuse, StaleAttemptCannotSettleANewAttempt) {
  ReuseRequestRegistry Registry(/*OwnerGeneration=*/3);
  const LogicalKey Key = makeLogical("f", 2, 1);

  RequestToken R1 = Registry.begin(Key);
  ASSERT_NE(R1, kInvalidRequestToken);
  Registry.setHoldsAdmission(R1, true);
  ASSERT_TRUE(Registry.cancel(R1));
  EXPECT_EQ(Registry.state(R1), AttemptState::Cancelling);

  // The same logical key, version, group and profile epoch: still a new token.
  RequestToken R2 = Registry.begin(Key);
  ASSERT_NE(R2, kInvalidRequestToken);
  EXPECT_NE(R1, R2);
  EXPECT_TRUE(Registry.isCurrent(R2));
  EXPECT_FALSE(Registry.isCurrent(R1));
  Registry.setHoldsAdmission(R2, true);

  // R1's late callbacks settle R1's own resources only.
  EventResult OldSampling = Registry.finishSampling(R1, /*Aborted=*/true);
  EXPECT_TRUE(OldSampling.accepted);
  EXPECT_TRUE(OldSampling.releaseAdmission);
  EXPECT_FALSE(OldSampling.logicalSettled);
  EventResult OldBorrow = Registry.endCompileBorrow(R1);
  EXPECT_TRUE(OldBorrow.borrowConfirmed);
  EventResult OldPublish = Registry.publishLogical(R1, 42);
  EXPECT_FALSE(OldPublish.accepted) << "a cancelled attempt is not published";

  // R2 is untouched by all of that.
  EXPECT_EQ(Registry.state(R2), AttemptState::Active);
  const AttemptRecord *R2Record = Registry.record(R2);
  ASSERT_NE(R2Record, nullptr);
  EXPECT_FALSE(R2Record->samplingSettled);
  EXPECT_FALSE(R2Record->borrowEnded);
  EXPECT_FALSE(R2Record->published);
  EXPECT_TRUE(R2Record->holdsAdmission);

  // R2 settles exactly once.
  EventResult Sampling = Registry.finishSampling(R2, false);
  EXPECT_TRUE(Sampling.accepted);
  EXPECT_TRUE(Sampling.releaseAdmission);
  EXPECT_TRUE(Registry.finishSampling(R2, false).duplicate);
  EXPECT_TRUE(Registry.endCompileBorrow(R2).borrowConfirmed);
  EXPECT_TRUE(Registry.endCompileBorrow(R2).duplicate);
  EventResult Publish = Registry.publishLogical(R2, 7);
  EXPECT_TRUE(Publish.logicalSettled);
  EXPECT_FALSE(Publish.releaseAdmission)
      << "publication must not release the group admission again";
  EXPECT_TRUE(Registry.publishLogical(R2, 7).duplicate);
}

TEST(EJitCodeReuse, CancellationRequiresBorrowEndBeforeFinish) {
  ReuseRequestRegistry Registry(2);
  const LogicalKey Key = makeLogical("f", 0, 0);
  RequestToken Token = Registry.begin(Key);
  ASSERT_TRUE(Registry.cancel(Token));
  EXPECT_EQ(Registry.state(Token), AttemptState::Cancelling);
  // A cancel callback followed by a read/publish callback must not report a
  // logical publication.
  EXPECT_FALSE(Registry.publishLogical(Token, 3).accepted);
  EventResult Borrow = Registry.endCompileBorrow(Token);
  EXPECT_TRUE(Borrow.borrowConfirmed);
  EXPECT_TRUE(Borrow.attemptFinished);
  EXPECT_EQ(Registry.state(Token), AttemptState::Finished);
}

TEST(EJitCodeReuse, UnknownAndDuplicateTokensAreRejected) {
  ReuseRequestRegistry Registry(1);
  EXPECT_FALSE(Registry.cancel(12345));
  EXPECT_FALSE(Registry.finishSampling(12345, false).accepted);
  EXPECT_FALSE(Registry.endCompileBorrow(12345).accepted);
  EXPECT_FALSE(Registry.publishLogical(12345, 1).accepted);

  RequestToken Token = Registry.begin(makeLogical("f", 0, 0));
  ASSERT_NE(Token, kInvalidRequestToken);
  // Events are separate: ending the borrow does not settle sampling or publish.
  EXPECT_TRUE(Registry.endCompileBorrow(Token).accepted);
  const AttemptRecord *Record = Registry.record(Token);
  ASSERT_NE(Record, nullptr);
  EXPECT_FALSE(Record->samplingSettled);
  EXPECT_FALSE(Record->published);
}

//===----------------------------------------------------------------------===//
// §5.5-§5.7 — group sampling: one representative, bounded, session-isolated
//===----------------------------------------------------------------------===//

TEST(EJitCodeReuse, OneRepresentativePerGroupAndWaitersDoNotTakeAdmission) {
  GroupBudget Budget;
  Budget.maxActiveRepresentatives = 4;
  ReuseGroupTable Table(Budget);
  ReuseRequestRegistry Registry(1);

  const std::string GroupKey = "group-A";
  std::vector<GroupMemberResult> Members;
  for (unsigned Cell = 0; Cell < 6; ++Cell) {
    RequestToken Token = Registry.begin(makeLogical("f", Cell, 0));
    Members.push_back(Table.join(GroupKey, makeLogical("f", Cell, 0), Token));
  }
  EXPECT_EQ(Members[0].role, MemberRole::Representative);
  EXPECT_TRUE(Members[0].acquiredAdmission);
  unsigned Waiters = 0;
  for (unsigned I = 1; I < Members.size(); ++I) {
    EXPECT_EQ(Members[I].role, MemberRole::Waiter);
    EXPECT_FALSE(Members[I].acquiredAdmission);
    // Every member shares the group's session identity.
    EXPECT_EQ(Members[I].session, Members[0].session);
    ++Waiters;
  }
  EXPECT_EQ(Waiters, 5u);
  EXPECT_EQ(Table.stats().activeRepresentatives, 1u);
  EXPECT_EQ(Table.waiterCount(Members[0].groupId), 5u);

  // Only real T1 dispatches count, and only for the representative's session.
  bool QuotaClosed = false;
  for (uint64_t I = 0; I < 63; ++I) {
    EXPECT_TRUE(Table.noteRepresentativeDispatch(Members[0].session,
                                                  &QuotaClosed));
    EXPECT_FALSE(QuotaClosed);
  }
  EXPECT_TRUE(
      Table.noteRepresentativeDispatch(Members[0].session, &QuotaClosed));
  EXPECT_TRUE(QuotaClosed) << "the 64th real dispatch closes the quota";
  EXPECT_FALSE(
      Table.noteRepresentativeDispatch(Members[0].session, &QuotaClosed))
      << "no new dispatch is admitted after the quota closes";

  // AOT fallback never counts: a different session cannot consume this quota.
  SamplingSessionId OtherSession{Members[0].session.groupId,
                                 Members[0].session.profileEpoch, 999};
  EXPECT_FALSE(Table.noteRepresentativeDispatch(OtherSession, nullptr));
}

TEST(EJitCodeReuse, FrozenBundleIsSharedAndImmutable) {
  ReuseGroupTable Table;
  ReuseRequestRegistry Registry(1);
  const std::string GroupKey = "group-B";
  GroupMemberResult Rep = Table.join(GroupKey, makeLogical("f", 0, 0),
                                     Registry.begin(makeLogical("f", 0, 0)));
  ASSERT_EQ(Rep.role, MemberRole::Representative);
  // Waiters and late joiners read the same object.
  GroupMemberResult Waiter = Table.join(
      GroupKey, makeLogical("f", 1, 0), Registry.begin(makeLogical("f", 1, 0)));
  ASSERT_EQ(Waiter.role, MemberRole::Waiter);
  EXPECT_EQ(Table.bundle(Rep.groupId), nullptr)
      << "no bundle before the freeze";

  ProfileBundle Bundle;
  Bundle.indexedProfile = "prof-bytes";
  Bundle.scalarSites.push_back("scalar-site-1");
  Bundle.schema.policy = "pgo-fdo+vp";
  Bundle.schema.schemaDigest = "schema-1";
  Bundle.schema.functionCount = 2;
  Bundle.schema.scalarSites = 1;
  Bundle.targetMappingDigest = "vpmap-1";
  Bundle.quality.dispatchCount = 64;
  Bundle.quality.quotaEndTick = 1000;
  Bundle.quality.freezeCompletedTick = 1010;
  Bundle.quality.missingShards = 1;
  Bundle.representative = makeLogical("f", 0, 0);

  auto Freeze = Table.freezeBundle(Rep.session, Bundle);
  EXPECT_TRUE(Freeze.accepted);
  EXPECT_TRUE(Freeze.releaseAdmission)
      << "the representative's admission is returned exactly here";
  EXPECT_FALSE(Table.holdsAdmission(Rep.groupId));
  EXPECT_EQ(Table.state(Rep.groupId), GroupState::BundleFrozen);
  EXPECT_TRUE(Table.freezeBundle(Rep.session, Bundle).duplicate);
  EXPECT_EQ(Table.stats().admissionReleases, 1u);

  const ProfileBundle *Stored = Table.bundle(Rep.groupId);
  ASSERT_NE(Stored, nullptr);
  EXPECT_TRUE(Stored->frozen);
  EXPECT_EQ(Stored->indexedProfile, "prof-bytes");
  EXPECT_EQ(Stored->scalarSites.size(), 1u) << "the side table must ride along";
  EXPECT_EQ(Stored->schema.schemaDigest, "schema-1");
  EXPECT_EQ(Stored->targetMappingDigest, "vpmap-1");
  EXPECT_EQ(Stored->quality.dispatchCount, 64u);
  EXPECT_TRUE(Stored->quality.approximate)
      << "64 dispatches is a bounded approximate snapshot, not 64 returns";
  EXPECT_EQ(Stored->quality.missingShards, 1u);

  // A late joiner is served by the frozen bundle rather than by a new sampler.
  GroupMemberResult Late = Table.join(
      GroupKey, makeLogical("f", 5, 0), Registry.begin(makeLogical("f", 5, 0)));
  EXPECT_EQ(Late.role, MemberRole::Waiter);
  EXPECT_EQ(Late.groupId, Rep.groupId);
}

TEST(EJitCodeReuse, LateAndForeignSamplesAreIsolated) {
  ReuseGroupTable Table;
  ReuseRequestRegistry Registry(1);
  const std::string GroupKey = "group-C";
  GroupMemberResult Rep = Table.join(GroupKey, makeLogical("f", 0, 0),
                                     Registry.begin(makeLogical("f", 0, 0)));
  ASSERT_EQ(Rep.role, MemberRole::Representative);
  EXPECT_TRUE(Table.acceptSample(Rep.session, 3));

  // A sample tagged with another group/epoch/session never reaches this bundle.
  SamplingSessionId Foreign{Rep.session.groupId, Rep.session.profileEpoch + 1,
                            Rep.session.session};
  EXPECT_FALSE(Table.acceptSample(Foreign, 3));
  SamplingSessionId OtherGroup{Rep.session.groupId + 100,
                               Rep.session.profileEpoch, Rep.session.session};
  EXPECT_FALSE(Table.acceptSample(OtherGroup, 3));

  ProfileBundle Bundle;
  Bundle.indexedProfile = "p";
  ASSERT_TRUE(Table.freezeBundle(Rep.session, Bundle).accepted);
  // After the freeze the session is closed: late writers are dropped and
  // counted, never merged into the immutable bundle.
  EXPECT_FALSE(Table.acceptSample(Rep.session, 9));
  EXPECT_GE(Table.stats().lateSamplesRejected, 1u);
}

TEST(EJitCodeReuse, RepresentativeReselectIsBoundedAndStartsANewSession) {
  GroupBudget Budget;
  Budget.maxRepresentativeReselects = 2;
  ReuseGroupTable Table(Budget);
  ReuseRequestRegistry Registry(1);
  const std::string GroupKey = "group-D";
  GroupMemberResult Rep = Table.join(GroupKey, makeLogical("f", 0, 0),
                                     Registry.begin(makeLogical("f", 0, 0)));
  ASSERT_EQ(Rep.role, MemberRole::Representative);

  // The representative goes cold: the group aborts its sampling, releases the
  // admission exactly once, and a new member may take over with a new epoch.
  auto Abort = Table.abortSampling(Rep.session);
  EXPECT_TRUE(Abort.accepted);
  EXPECT_TRUE(Abort.releaseAdmission);
  EXPECT_TRUE(Table.abortSampling(Rep.session).duplicate);
  EXPECT_EQ(Table.stats().admissionReleases, 1u);
  EXPECT_EQ(Table.state(Rep.groupId), GroupState::SamplingAborted);

  GroupMemberResult Second;
  ASSERT_TRUE(Table.reselectRepresentative(
      Rep.groupId, makeLogical("f", 1, 0),
      Registry.begin(makeLogical("f", 1, 0)), &Second));
  EXPECT_EQ(Second.role, MemberRole::Representative);
  EXPECT_NE(Second.session.session, Rep.session.session);
  EXPECT_NE(Second.session.profileEpoch, Rep.session.profileEpoch)
      << "a new round must not mix old partial counters";
  EXPECT_TRUE(Table.holdsAdmission(Rep.groupId));
  // Old-session samples are rejected by session identity.
  EXPECT_FALSE(Table.acceptSample(Rep.session, 1));

  ASSERT_TRUE(Table.abortSampling(Second.session).accepted);
  GroupMemberResult Third;
  ASSERT_TRUE(Table.reselectRepresentative(
      Rep.groupId, makeLogical("f", 2, 0),
      Registry.begin(makeLogical("f", 2, 0)), &Third));
  ASSERT_TRUE(Table.abortSampling(Third.session).accepted);

  GroupMemberResult Fourth;
  EXPECT_FALSE(Table.reselectRepresentative(
      Rep.groupId, makeLogical("f", 3, 0),
      Registry.begin(makeLogical("f", 3, 0)), &Fourth))
      << "the retry budget must stop the reselect loop";
}

TEST(EJitCodeReuse, AdmissionCapFallsBackInsteadOfCompilingIndependently) {
  GroupBudget Budget;
  Budget.maxActiveRepresentatives = 2;
  ReuseGroupTable Table(Budget);
  ReuseRequestRegistry Registry(1);

  GroupMemberResult G1 = Table.join("g1", makeLogical("f", 0, 0),
                                    Registry.begin(makeLogical("f", 0, 0)));
  GroupMemberResult G2 = Table.join("g2", makeLogical("f", 0, 0),
                                    Registry.begin(makeLogical("f", 0, 0)));
  EXPECT_EQ(G1.role, MemberRole::Representative);
  EXPECT_EQ(G2.role, MemberRole::Representative);
  EXPECT_EQ(Table.stats().activeRepresentatives, 2u);

  // The third group has no admission slot: its member waits (AOT) rather than
  // opening a third sampler, and must not be silently treated as a sampler.
  GroupMemberResult G3 = Table.join("g3", makeLogical("f", 0, 0),
                                    Registry.begin(makeLogical("f", 0, 0)));
  EXPECT_EQ(G3.role, MemberRole::Waiter);
  EXPECT_FALSE(G3.acquiredAdmission);
  EXPECT_EQ(Table.stats().activeRepresentatives, 2u);

  // Finishing g1 admits the next sampler.
  ProfileBundle Bundle;
  Bundle.indexedProfile = "p1";
  ASSERT_TRUE(Table.freezeBundle(G1.session, Bundle).accepted);
  EXPECT_EQ(Table.stats().activeRepresentatives, 1u);
}

TEST(EJitCodeReuse, GroupAndWaiterBudgetsAreEnforced) {
  GroupBudget Budget;
  Budget.maxGroups = 2;
  Budget.maxWaitersPerGroup = 3;
  ReuseGroupTable Table(Budget);
  ReuseRequestRegistry Registry(1);

  ASSERT_EQ(Table.join("g1", makeLogical("f", 0, 0),
                       Registry.begin(makeLogical("f", 0, 0)))
                .role,
            MemberRole::Representative);
  // Three waiters fit; the fourth is rejected instead of growing the table.
  for (unsigned I = 0; I < 3; ++I)
    EXPECT_EQ(Table.join("g1", makeLogical("f", I + 1, 0),
                         Registry.begin(makeLogical("f", I + 1, 0)))
                  .role,
              MemberRole::Waiter);
  EXPECT_EQ(Table.join("g1", makeLogical("f", 9, 0),
                       Registry.begin(makeLogical("f", 9, 0)))
                .role,
            MemberRole::Rejected);
  EXPECT_EQ(Table.stats().rejectedMembers, 1u);

  // maxGroups bounds the group table itself.
  EXPECT_EQ(Table.join("g2", makeLogical("f", 0, 0),
                       Registry.begin(makeLogical("f", 0, 0)))
                .role,
            MemberRole::Representative);
  EXPECT_EQ(Table.join("g3", makeLogical("f", 0, 0),
                       Registry.begin(makeLogical("f", 0, 0)))
                .role,
            MemberRole::Rejected);
}

TEST(EJitCodeReuse, BundleByteBudgetAbortsInsteadOfTrimming) {
  GroupBudget Budget;
  Budget.maxBundleBytes = 8;
  ReuseGroupTable Table(Budget);
  ReuseRequestRegistry Registry(1);
  GroupMemberResult Rep = Table.join("g1", makeLogical("f", 0, 0),
                                     Registry.begin(makeLogical("f", 0, 0)));
  ASSERT_EQ(Rep.role, MemberRole::Representative);

  ProfileBundle Bundle;
  Bundle.indexedProfile = "a-long-indexed-profile-payload";
  auto Freeze = Table.freezeBundle(Rep.session, Bundle);
  EXPECT_FALSE(Freeze.accepted);
  // The group ends without a bundle; it must not expose a truncated profile.
  EXPECT_EQ(Table.state(Rep.groupId), GroupState::SamplingAborted);
  EXPECT_EQ(Table.bundle(Rep.groupId), nullptr);
  EXPECT_TRUE(Freeze.releaseAdmission);
  EXPECT_EQ(Table.stats().bundleBytes, 0u);
}

//===----------------------------------------------------------------------===//
// Group key identity: the same prefix/schema/bindings share one group
//===----------------------------------------------------------------------===//

TEST(EJitCodeReuse, ProfileGroupKeySeparatesSchemaAndBindings) {
  ProfileGroupKey A;
  A.sourceId = "src";
  A.entry = "f";
  A.policy = "shared-v1";
  A.prefixDigest = "prefix";
  A.schema.policy = "fdo";
  A.schema.schemaDigest = "schema-1";
  A.bindings = makeBindings("helper", 0x1000);

  ProfileGroupKey B = A;
  EXPECT_TRUE(A == B);
  EXPECT_EQ(A.encode(), B.encode());

  B.schema.schemaDigest = "schema-2";
  EXPECT_FALSE(A == B) << "an incompatible PGO schema must not share a group";

  ProfileGroupKey C = A;
  C.bindings = makeBindings("helper", 0x2000);
  EXPECT_FALSE(A == C) << "a different resolved address must not share a group";

  ProfileGroupKey D = A;
  D.schema.disabledKinds.push_back("indirect-call");
  EXPECT_FALSE(A == D)
      << "a disabled VP kind is part of the schema identity, not noise";
}

TEST(EJitCodeReuse, EncodingIsCollisionFreeAcrossFieldSplits) {
  LogicalKey A = makeLogical("f", 1, 2);
  LogicalKey B;
  B.entry = "f1";
  B.sourceId = "src-rev-1";
  B.dimensions.push_back({"cell", 2});
  B.dimensions.push_back({"trp", 0});
  B.lifecycleVersions = A.lifecycleVersions;
  B.runtimeGeneration = A.runtimeGeneration;
  // Different field splits must not encode to the same key material.
  EXPECT_NE(A.encode(), B.encode());

  BindingEnvironment E1;
  E1.entries.push_back({"ab", 1, 0});
  E1.entries.push_back({"c", 2, 0});
  BindingEnvironment E2;
  E2.entries.push_back({"a", 1, 0});
  E2.entries.push_back({"bc", 2, 0});
  E1.normalize();
  E2.normalize();
  EXPECT_NE(E1.encode(), E2.encode());
}

//===----------------------------------------------------------------------===//
// Exact physical byte accounting (§7)
//===----------------------------------------------------------------------===//

TEST(EJitCodeReuse, PhysicalBytesCountedOncePerRecord) {
  CodeReuseTable Table;
  auto Add = Table.addCandidate(makeCodeKey("f", "d"), {"d", "ir"});
  ASSERT_TRUE(Add.ok);
  ASSERT_TRUE(Table.markLinked(Add.codeId, 0x1000,
                               {{0x1000, 100}, {0x2000, 28}}, {{0x3000, 64}},
                               "near"));
  ASSERT_TRUE(Table.markPublished(Add.codeId));
  for (unsigned I = 0; I < 6; ++I)
    ASSERT_TRUE(Table.bindLogical(makeLogical("f", I, 0), Add.codeId));

  EXPECT_EQ(Table.stats().codeRecords, 1u);
  EXPECT_EQ(Table.stats().logicalBindings, 6u);
  EXPECT_EQ(Table.stats().reusedVersions, 6u);
  // 100 + 28 counted once, not 6 times.
  EXPECT_EQ(Table.stats().execBytesUnique, 128u);
  const CodeRecord *Record = Table.getRecord(Add.codeId);
  ASSERT_NE(Record, nullptr);
  EXPECT_EQ(Record->pool, "near");
  EXPECT_EQ(Record->state, CodeState::Published);
}

} // namespace
