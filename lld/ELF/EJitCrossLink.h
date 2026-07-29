//===-- EJitCrossLink.h - EJIT Cross-TU Inlining at Link Time -------------===//

#ifndef LLD_ELF_EJITCROSSLINK_H
#define LLD_ELF_EJITCROSSLINK_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include <string>

namespace lld {
namespace elf {

struct EJitCrossLinkResult {
  std::string tempPath;
  llvm::SmallVector<std::string, 4> consumedFiles;
};

/// Merge the .ejit_cross bitcode sections of \p InputFiles, inline cross-TU
/// callees into each ejit_entry, and write a single registry bitcode module to
/// a temporary file whose path is returned.
///
/// Three-state result:
///   * an Error         - a .ejit_cross section was found but processing failed
///                        (the link must fail; never silently fall back to
///                        AOT);
///   * an empty result  - no input carried a .ejit_cross section (normal skip);
///   * a populated result - the temporary bitcode to add plus the exact input
///                          files whose sections were consumed. The caller owns
///                          cleanup of the temporary file.
llvm::Expected<EJitCrossLinkResult>
runEJitCrossLink(llvm::ArrayRef<std::string> InputFiles,
                 llvm::StringRef TargetTriple,
                 llvm::StringRef SaveTempsPrefix = {});

} // namespace elf
} // namespace lld

#endif
