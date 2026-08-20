//===-- EJitFunctionIsolation.cpp - Function-only JIT boundary -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ExecutionEngine/EJIT/EJitCommon.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"

using namespace llvm;
using namespace llvm::ejit;

bool llvm::ejit::detail::isolateFunctionForSpecialization(Module &M,
                                                           StringRef fnName) {
  Function *Entry = M.getFunction(fnName);
  if (!Entry || Entry->isDeclaration())
    return false;

  SmallVector<std::pair<Function *, std::string>, 16> Fallbacks;
  StringSet<> FallbackNames;
  for (Function &F : M.functions()) {
    if (&F == Entry || F.isDeclaration() || F.isIntrinsic())
      continue;

    MDNode *MD = F.getMetadata(MD_EJIT_AOT_SYMBOL);
    auto *Name = MD && MD->getNumOperands() == 1
                     ? dyn_cast<MDString>(MD->getOperand(0))
                     : nullptr;
    if (!Name || Name->getString().empty() ||
        !FallbackNames.insert(Name->getString()).second)
      return false;
    if (GlobalValue *Existing = M.getNamedValue(Name->getString()))
      if (Existing != &F)
        return false;
    Fallbacks.push_back({&F, Name->getString().str()});
  }

  for (auto &[F, AotName] : Fallbacks) {
    F->setName(AotName);
    F->deleteBody();
    F->setLinkage(GlobalValue::ExternalLinkage);
    F->setVisibility(GlobalValue::DefaultVisibility);
    F->setDSOLocal(false);
    F->setDLLStorageClass(GlobalValue::DefaultStorageClass);
    F->setUnnamedAddr(GlobalValue::UnnamedAddr::None);
    F->setSection("");
    if (F->hasComdat())
      F->setComdat(nullptr);
  }
  return true;
}
