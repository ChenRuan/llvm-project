//===-- EJitBranchProfileTest.cpp -----------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitBranchProfile.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ProfDataUtils.h"
#include "gtest/gtest.h"

#include <algorithm>

using namespace llvm;
using namespace llvm::ejit;

namespace {

Function *makeBranchFunction(Module &M, StringRef Name,
                             ArrayRef<uint32_t> Weights = {}) {
  LLVMContext &Ctx = M.getContext();
  auto *FT =
      FunctionType::get(Type::getVoidTy(Ctx), {Type::getInt1Ty(Ctx)}, false);
  Function *F = Function::Create(FT, Function::ExternalLinkage, Name, M);
  auto *Entry = BasicBlock::Create(Ctx, "entry", F);
  auto *Taken = BasicBlock::Create(Ctx, "taken", F);
  auto *Other = BasicBlock::Create(Ctx, "other", F);

  IRBuilder<> Builder(Entry);
  auto *Branch = Builder.CreateCondBr(F->getArg(0), Taken, Other);
  if (!Weights.empty())
    setBranchWeights(*Branch, Weights, false);
  Builder.SetInsertPoint(Taken);
  Builder.CreateRetVoid();
  Builder.SetInsertPoint(Other);
  Builder.CreateRetVoid();
  return F;
}

const EJitBranchProfileSummary &
findSummary(const std::vector<EJitBranchProfileSummary> &Summaries,
            StringRef Name) {
  auto It = std::find_if(
      Summaries.begin(), Summaries.end(),
      [Name](const auto &Summary) { return Summary.functionName == Name; });
  EXPECT_NE(It, Summaries.end());
  return *It;
}

TEST(EJitBranchProfile, ClassifiesBranchShapeWithoutChangingIR) {
  LLVMContext Ctx;
  Module M("branch-audit", Ctx);
  Function *Hot = makeBranchFunction(M, "hot", {99, 1});
  Hot->setEntryCount(123);
  makeBranchFunction(M, "balanced", {50, 50});
  makeBranchFunction(M, "zero_edge", {100, 0});
  makeBranchFunction(M, "missing");
  Function::Create(FunctionType::get(Type::getVoidTy(Ctx), false),
                   Function::ExternalLinkage, "declaration", M);

  auto Summaries = analyzeBranchProfiles(M, "hot");
  ASSERT_EQ(Summaries.size(), 4u);

  const auto &HotSummary = findSummary(Summaries, "hot");
  EXPECT_TRUE(HotSummary.isRoot);
  EXPECT_EQ(HotSummary.entryCount, 123u);
  EXPECT_EQ(HotSummary.branchSites, 1u);
  EXPECT_EQ(HotSummary.profiledSites, 1u);
  EXPECT_EQ(HotSummary.biasedSites95, 1u);
  EXPECT_EQ(HotSummary.balancedSites60, 0u);
  EXPECT_GT(HotSummary.instructionCount, 0u);

  const auto &Balanced = findSummary(Summaries, "balanced");
  EXPECT_EQ(Balanced.biasedSites95, 0u);
  EXPECT_EQ(Balanced.balancedSites60, 1u);

  const auto &ZeroEdge = findSummary(Summaries, "zero_edge");
  EXPECT_EQ(ZeroEdge.biasedSites95, 1u);
  EXPECT_EQ(ZeroEdge.zeroCountEdges, 1u);

  const auto &Missing = findSummary(Summaries, "missing");
  EXPECT_EQ(Missing.branchSites, 1u);
  EXPECT_EQ(Missing.profiledSites, 0u);
}

} // namespace
