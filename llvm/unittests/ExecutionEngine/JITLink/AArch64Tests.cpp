//===------- AArch64Tests.cpp - Unit tests for the AArch64 backend --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include <llvm/BinaryFormat/ELF.h>
#include <llvm/ExecutionEngine/JITLink/aarch64.h>

#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::jitlink;
using namespace llvm::jitlink::aarch64;

TEST(AArch64, EmptyLinkGraph) {
  LinkGraph G("foo", std::make_shared<orc::SymbolStringPool>(),
              Triple("arm64-apple-darwin"), SubtargetFeatures(),
              getEdgeKindName);
  EXPECT_EQ(G.getName(), "foo");
  EXPECT_EQ(G.getTargetTriple().str(), "arm64-apple-darwin");
  EXPECT_EQ(G.getPointerSize(), 8U);
  EXPECT_EQ(G.getEndianness(), llvm::endianness::little);
  EXPECT_TRUE(G.external_symbols().empty());
  EXPECT_TRUE(G.absolute_symbols().empty());
  EXPECT_TRUE(G.defined_symbols().empty());
  EXPECT_TRUE(G.blocks().empty());
}

TEST(AArch64, GOTAndStubs) {
  LinkGraph G("foo", std::make_shared<orc::SymbolStringPool>(),
              Triple("arm64-apple-darwin"), SubtargetFeatures(),
              getEdgeKindName);

  auto &External = G.addExternalSymbol("external", 0, false);

  // First table accesses. We expect the graph to be empty:
  EXPECT_EQ(G.findSectionByName(GOTTableManager::getSectionName()), nullptr);
  EXPECT_EQ(G.findSectionByName(PLTTableManager::getSectionName()), nullptr);

  {
    // Create first GOT and PLT table managers and request a PLT stub. This
    // should force creation of both a PLT stub and GOT entry.
    GOTTableManager GOT(G);
    PLTTableManager PLT(G, GOT);

    PLT.getEntryForTarget(G, External);
  }

  auto *GOTSec = G.findSectionByName(GOTTableManager::getSectionName());
  EXPECT_NE(GOTSec, nullptr);
  if (GOTSec) {
    // Expect one entry in the GOT now.
    EXPECT_EQ(GOTSec->symbols_size(), 1U);
    EXPECT_EQ(GOTSec->blocks_size(), 1U);
  }

  auto *PLTSec = G.findSectionByName(PLTTableManager::getSectionName());
  EXPECT_NE(PLTSec, nullptr);
  if (PLTSec) {
    // Expect one entry in the PLT.
    EXPECT_EQ(PLTSec->symbols_size(), 1U);
    EXPECT_EQ(PLTSec->blocks_size(), 1U);
  }

  {
    // Create second GOT and PLT table managers and request a PLT stub. This
    // should force creation of both a PLT stub and GOT entry.
    GOTTableManager GOT(G);
    PLTTableManager PLT(G, GOT);

    PLT.getEntryForTarget(G, External);
  }

  EXPECT_EQ(G.findSectionByName(GOTTableManager::getSectionName()), GOTSec);
  if (GOTSec) {
    // Expect the same one entry in the GOT.
    EXPECT_EQ(GOTSec->symbols_size(), 1U);
    EXPECT_EQ(GOTSec->blocks_size(), 1U);
  }

  EXPECT_EQ(G.findSectionByName(PLTTableManager::getSectionName()), PLTSec);
  if (PLTSec) {
    // Expect the same one entry in the GOT.
    EXPECT_EQ(PLTSec->symbols_size(), 1U);
    EXPECT_EQ(PLTSec->blocks_size(), 1U);
  }
}

static void checkPointerJumpStubRelaxation(StringRef TripleName,
                                           uint64_t CallAddress,
                                           uint64_t TargetAddress,
                                           bool ExpectDirect) {
  LinkGraph G("stub-relax", std::make_shared<orc::SymbolStringPool>(),
              Triple(TripleName), SubtargetFeatures(), getEdgeKindName);

  auto &Text =
      G.createSection("__text", orc::MemProt::Read | orc::MemProt::Exec);
  const char CallContent[4] = {0x00, 0x00, 0x00, (char)0x94u};
  auto &CallBlock = G.createContentBlock(Text, CallContent,
                                         orc::ExecutorAddr(CallAddress), 4, 0);
  auto &Target = G.addExternalSymbol("aot_target", 0, true);
  CallBlock.addEdge(Branch26PCRel, 0, Target, 0);
  Edge &CallEdge = *CallBlock.edges().begin();

  GOTTableManager GOT(G);
  PLTTableManager PLT(G, GOT);
  ASSERT_TRUE(PLT.visitEdge(G, &CallBlock, CallEdge));
  Symbol *Stub = &CallEdge.getTarget();
  ASSERT_NE(Stub, &Target);

  G.makeAbsolute(Target, orc::ExecutorAddr(TargetAddress));
  cantFail(optimizePointerJumpStubBranches(G));

  if (ExpectDirect)
    EXPECT_EQ(&CallEdge.getTarget(), &Target);
  else
    EXPECT_EQ(&CallEdge.getTarget(), Stub);
}

TEST(AArch64, PointerJumpStubBranchRelaxedInRange) {
  checkPointerJumpStubRelaxation("aarch64-unknown-linux-gnu", 0x10000000,
                                 0x17fffffc, true);
}

TEST(AArch64, PointerJumpStubBranchRetainedOutOfRange) {
  checkPointerJumpStubRelaxation("aarch64-unknown-linux-gnu", 0x10000000,
                                 0x18000000, false);
}

TEST(AArch64, PointerJumpStubBranchRelaxedInRangeBigEndian) {
  checkPointerJumpStubRelaxation("aarch64_be-unknown-linux-gnu", 0x10000000,
                                 0x10004000, true);
}
