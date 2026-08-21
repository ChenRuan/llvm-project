//===-- EJitBranchProfile.cpp - Online-PGO branch audit -------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitBranchProfile.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ProfDataUtils.h"

#include <algorithm>

using namespace llvm;
using namespace llvm::ejit;

namespace {

uint64_t ceilPercent(uint64_t Total, uint32_t Percent) {
  return (Total / 100) * Percent + ((Total % 100) * Percent + 99) / 100;
}

uint64_t floorPercent(uint64_t Total, uint32_t Percent) {
  return (Total / 100) * Percent + ((Total % 100) * Percent) / 100;
}

} // namespace

std::vector<EJitBranchProfileSummary>
llvm::ejit::analyzeBranchProfiles(const Module &M,
                                  const std::string &rootName) {
  std::vector<EJitBranchProfileSummary> Result;

  for (const Function &F : M) {
    if (F.isDeclaration() || F.empty() ||
        F.getName().starts_with("__llvm_profile"))
      continue;

    EJitBranchProfileSummary Summary;
    Summary.functionName = F.getName().str();
    Summary.isRoot = F.getName() == rootName;
    if (auto Count = F.getEntryCount())
      Summary.entryCount = Count->getCount();

    for (const BasicBlock &BB : F) {
      Summary.instructionCount += BB.size();
      const Instruction *Term = BB.getTerminator();
      if (!Term || Term->getNumSuccessors() < 2)
        continue;

      ++Summary.branchSites;
      SmallVector<uint32_t, 8> Weights;
      if (!extractBranchWeights(*Term, Weights) ||
          Weights.size() != Term->getNumSuccessors())
        continue;

      uint64_t Total = 0;
      uint64_t Max = 0;
      uint32_t ZeroEdges = 0;
      for (uint32_t Weight : Weights) {
        Total += Weight;
        Max = std::max(Max, static_cast<uint64_t>(Weight));
        ZeroEdges += Weight == 0;
      }
      if (Total == 0)
        continue;

      ++Summary.profiledSites;
      Summary.zeroCountEdges += ZeroEdges;
      Summary.biasedSites95 += Max >= ceilPercent(Total, 95);
      Summary.balancedSites60 += Max <= floorPercent(Total, 60);
    }

    Result.push_back(std::move(Summary));
  }

  return Result;
}
