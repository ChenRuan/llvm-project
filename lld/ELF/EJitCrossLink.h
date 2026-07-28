//===-- EJitCrossLink.h - EJIT Cross-TU Inlining at Link Time -------------===//

#ifndef LLD_ELF_EJITCROSSLINK_H
#define LLD_ELF_EJITCROSSLINK_H

#include "llvm/ADT/ArrayRef.h"
#include <string>

namespace lld {
namespace elf {

std::string runEJitCrossLink(llvm::ArrayRef<std::string> InputFiles,
                              const std::string &TargetTriple);

} // namespace elf
} // namespace lld

#endif
