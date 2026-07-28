//===-- EJitCrossLink.h - EJIT Cross-TU Inlining at Link Time -------------===//

#ifndef CLANG_LIB_DRIVER_EJITCROSSLINK_H
#define CLANG_LIB_DRIVER_EJITCROSSLINK_H

#include "llvm/ADT/ArrayRef.h"
#include <string>

namespace clang {
namespace driver {

std::string runEJitCrossLink(llvm::ArrayRef<std::string> InputFiles,
                              const std::string &TargetTriple);

} // namespace driver
} // namespace clang

#endif
