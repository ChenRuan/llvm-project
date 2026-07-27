//===-- EJitDirectCallStubsTest.cpp ---------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitDirectCallStubs.h"

#include "llvm/ExecutionEngine/JITLink/aarch64.h"
#include "llvm/Support/Endian.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::jitlink;

namespace {

class EJitInlineLongCallTest : public testing::Test {
protected:
  EJitInlineLongCallTest()
      : G("inline-long-call", std::make_shared<orc::SymbolStringPool>(),
          Triple("aarch64_be-unknown-linux-gnu"), SubtargetFeatures(),
          aarch64::getEdgeKindName),
        Text(G.createSection(".text", orc::MemProt::Read | orc::MemProt::Exec)),
        GOT(G.createSection(aarch64::GOTTableManager::getSectionName(),
                            orc::MemProt::Read | orc::MemProt::Exec)) {}

  Symbol &createGOTEntry(uint64_t TargetAddress, bool Callable = true) {
    auto &Target =
        G.addAbsoluteSymbol("aot_target", orc::ExecutorAddr(TargetAddress), 0,
                            Linkage::Strong, Scope::Default, true);
    Target.setCallable(Callable);
    auto &GOTBlock = G.createMutableContentBlock(
        GOT, 8, orc::ExecutorAddr(0x20000000), 8, 0);
    GOTBlock.addEdge(aarch64::Pointer64, 0, Target, 0);
    return G.addAnonymousSymbol(GOTBlock, 0, 8, false, false);
  }

  Block &createCallBlock(Symbol &GOTEntry, bool SeparateLdr = false) {
    size_t Size = SeparateLdr ? 16 : 12;
    auto &B = G.createMutableContentBlock(Text, Size,
                                          orc::ExecutorAddr(0x10000000), 4, 0);
    MutableArrayRef<char> C = B.getAlreadyMutableContent();
    support::endian::write32le(C.data(), 0x90000015u); // ADRP x21
    size_t LdrOffset = SeparateLdr ? 8 : 4;
    if (SeparateLdr)
      support::endian::write32le(C.data() + 4,
                                 0x2a0003f3u); // MOV w19, w0
    support::endian::write32le(C.data() + LdrOffset,
                               0xf94002b5u); // LDR x21, [x21]
    support::endian::write32le(C.data() + LdrOffset + 4,
                               0xd63f02a0u); // BLR x21
    B.addEdge(aarch64::Page21, 0, GOTEntry, 0);
    B.addEdge(aarch64::PageOffset12, LdrOffset, GOTEntry, 0);
    return B;
  }

  LinkGraph G;
  Section &Text;
  Section &GOT;
};

static void expectSuccess(Error Err) {
  if (Err) {
    ADD_FAILURE() << toString(std::move(Err));
  }
}

TEST_F(EJitInlineLongCallTest, RewritesInRangeCallSite) {
  // 0xb0000000 - 0x10000000 = 2.5GiB: outside BL, inside ADRP.
  Symbol &GOTEntry = createGOTEntry(0xb0000000);
  Block &B = createCallBlock(GOTEntry);

  expectSuccess(ejit::relaxAArch64InlineGOTCalls(G));
  EXPECT_EQ(support::endian::read32le(B.getContent().data() + 4),
            0x910002b5u); // ADD x21, x21, #lo12
  for (Edge &E : B.edges())
    EXPECT_EQ(E.getTarget().getAddress(), orc::ExecutorAddr(0xb0000000));
}

TEST_F(EJitInlineLongCallTest, MatchesHoistedNonAdjacentAddressLoad) {
  Symbol &GOTEntry = createGOTEntry(0xb0000000);
  Block &B = createCallBlock(GOTEntry, true);

  expectSuccess(ejit::relaxAArch64InlineGOTCalls(G));
  EXPECT_EQ(support::endian::read32le(B.getContent().data() + 4),
            0x2a0003f3u); // intervening instruction unchanged
  EXPECT_EQ(support::endian::read32le(B.getContent().data() + 8), 0x910002b5u);
}

TEST_F(EJitInlineLongCallTest, KeepsGOTCallBeyondAdrpRange) {
  Symbol &GOTEntry = createGOTEntry(0x210000000);
  Block &B = createCallBlock(GOTEntry);

  expectSuccess(ejit::relaxAArch64InlineGOTCalls(G));
  EXPECT_EQ(support::endian::read32le(B.getContent().data() + 4), 0xf94002b5u);
  for (Edge &E : B.edges())
    EXPECT_EQ(&E.getTarget(), &GOTEntry);
}

TEST_F(EJitInlineLongCallTest, IgnoresNonCallableGOTTarget) {
  Symbol &GOTEntry = createGOTEntry(0xb0000000, false);
  Block &B = createCallBlock(GOTEntry);

  expectSuccess(ejit::relaxAArch64InlineGOTCalls(G));
  EXPECT_EQ(support::endian::read32le(B.getContent().data() + 4), 0xf94002b5u);
}

} // namespace
