//===-- EJitBranchProfile.h - Online-PGO branch audit -----------*- C++ -*-===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITBRANCHPROFILE_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITBRANCHPROFILE_H

#include <cstdint>
#include <string>
#include <vector>

namespace llvm {

class Module;

namespace ejit {

struct EJitBranchProfileSummary {
  std::string functionName;
  uint64_t entryCount = 0;
  uint64_t instructionCount = 0;
  uint32_t branchSites = 0;
  uint32_t profiledSites = 0;
  uint32_t biasedSites95 = 0;
  uint32_t balancedSites60 = 0;
  uint32_t zeroCountEdges = 0;
  bool isRoot = false;
};

/// Summarize the branch weights attached by PGOInstrumentationUse. This is a
/// read-only audit: it does not alter IR or add runtime instrumentation.
std::vector<EJitBranchProfileSummary>
analyzeBranchProfiles(const Module &M, const std::string &rootName);

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITBRANCHPROFILE_H
