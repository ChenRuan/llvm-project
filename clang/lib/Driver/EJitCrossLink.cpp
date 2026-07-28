//===-- EJitCrossLink.cpp - EJIT Cross-TU Inlining at Link Time -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "EJitCrossLink.h"
#include "clang/Driver/Driver.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/ExecutionEngine/EJIT/EJitCommon.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdlib>
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/EarlyCSE.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace llvm;
using namespace llvm::ejit;

namespace {

// ---- Helpers (mirror static functions in EJitRegisterBitcode.cpp) ----

static bool hasMDStringEntry(const MDNode *Node, StringRef Name) {
  for (const MDOperand &Op : Node->operands()) {
    if (auto *Sub = dyn_cast<MDNode>(Op.get())) {
      if (Sub->getNumOperands() < 1)
        continue;
      if (auto *S = dyn_cast<MDString>(Sub->getOperand(0)))
        if (S->getString() == Name)
          return true;
    }
  }
  return false;
}

static void collectEntryFunctions(Module &M,
                                  SmallVectorImpl<Function *> &EntryFuncs) {
  for (Function &F : M.functions()) {
    MDNode *MD = F.getMetadata(MD_EJIT_METADATA);
    if (MD && hasMDStringEntry(MD, TAG_EJIT_ENTRY))
      EntryFuncs.push_back(&F);
  }
}

static void computeTransitiveClosure(
    SmallVectorImpl<Function *> &EntryFuncs,
    SetVector<Function *> &ClosureFuncs) {
  SmallVector<Function *, 16> Worklist(EntryFuncs.begin(), EntryFuncs.end());
  while (!Worklist.empty()) {
    Function *F = Worklist.pop_back_val();
    if (!ClosureFuncs.insert(F))
      continue;
    for (BasicBlock &BB : *F) {
      for (Instruction &I : BB) {
        if (auto *CI = dyn_cast<CallInst>(&I)) {
          Function *Callee = CI->getCalledFunction();
          if (Callee && !Callee->isDeclaration() && !Callee->isIntrinsic())
            Worklist.push_back(Callee);
        }
      }
    }
  }
}

static void reAnnotateMayConst(Module &M) {
  DenseMap<const GlobalVariable *, SmallVector<uint64_t, 4>> Offsets;
  for (GlobalVariable &GV : M.globals()) {
    MDNode *MD = GV.getMetadata(MD_EJIT_METADATA);
    if (!MD)
      continue;
    for (const MDOperand &Op : MD->operands()) {
      auto *Entry = dyn_cast<MDNode>(Op.get());
      if (!Entry || Entry->getNumOperands() < 2)
        continue;
      auto *Tag = dyn_cast<MDString>(Entry->getOperand(0));
      if (!Tag || Tag->getString() != TAG_EJIT_MAY_CONST_FIELD)
        continue;
      if (auto *CI = mdconst::dyn_extract<ConstantInt>(Entry->getOperand(1)))
        Offsets[&GV].push_back(CI->getZExtValue());
    }
  }
  if (Offsets.empty())
    return;
  for (Function &F : M.functions()) {
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        auto *LI = dyn_cast<LoadInst>(&I);
        if (!LI || LI->hasMetadata("ejit.may_const") || LI->isVolatile() ||
            LI->isAtomic())
          continue;
        GlobalVariable *RootGV = nullptr;
        uint64_t ByteOff = 0;
        Value *Ptr = LI->getPointerOperand();
        if (auto *GEP = dyn_cast<GEPOperator>(Ptr)) {
          RootGV = dyn_cast<GlobalVariable>(GEP->getPointerOperand());
          if (!RootGV)
            continue;
          APInt Off(64, 0);
          if (!GEP->accumulateConstantOffset(M.getDataLayout(), Off))
            continue;
          ByteOff = Off.getZExtValue();
        } else if (auto *GV = dyn_cast<GlobalVariable>(Ptr)) {
          RootGV = GV;
        } else {
          continue;
        }
        auto It = Offsets.find(RootGV);
        if (It == Offsets.end() || !llvm::is_contained(It->second, ByteOff))
          continue;
        // Check the load fits within the may_const field boundary.
        // A wider load straddles the next field, which is not constant.
        TypeSize AccessSize = M.getDataLayout().getTypeStoreSize(LI->getType());
        if (AccessSize.isScalable())
          continue;
        {
          // Compute the size of the field at this offset.
          // If not found, conservatively skip.
          StructType *STy = dyn_cast<StructType>(RootGV->getValueType());
          if (!STy)
            continue;
          const StructLayout *SL =
              M.getDataLayout().getStructLayout(STy);
          unsigned FieldIdx = SL->getElementContainingOffset(ByteOff);
          uint64_t FieldEnd = SL->getElementOffset(FieldIdx) +
                              M.getDataLayout().getTypeStoreSize(
                                  STy->getElementType(FieldIdx));
          if (ByteOff + AccessSize.getFixedValue() > FieldEnd)
            continue;
        }
        LI->setMetadata("ejit.may_const", MDNode::get(M.getContext(), {}));
      }
    }
  }
}

static void preOptimizeBitcode(Module &M) {
#ifndef NDEBUG
  return;
#endif
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PassBuilder PB;
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  ModulePassManager MPM;
  MPM.addPass(AlwaysInlinerPass());
  // Cross-TU inlining happens in the post-merge inliner below (Phase 5).
  FunctionPassManager FPM;
  FPM.addPass(PromotePass());
  FPM.addPass(EarlyCSEPass());
  FPM.addPass(InstCombinePass());
  FPM.addPass(SimplifyCFGPass());
  MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));
  MPM.run(M, MAM);
}

static std::string serializeToBitcode(const Module &M) {
  std::string BC;
  raw_string_ostream OS(BC);
  WriteBitcodeToFile(M, OS);
  return BC;
}

static GlobalVariable *embedBitcode(Module &M, const std::string &Bitcode,
                                    const std::string &FuncName) {
  LLVMContext &Ctx = M.getContext();
  auto *ArrTy = ArrayType::get(Type::getInt8Ty(Ctx), Bitcode.size());
  auto *Const = ConstantDataArray::get(
      Ctx, ArrayRef<uint8_t>(
               reinterpret_cast<const uint8_t *>(Bitcode.data()),
               Bitcode.size()));
  std::string GVName =
      (Twine(GV_EJIT_BITCODE) + "_" + FuncName).str();
  auto *GV = new GlobalVariable(M, ArrTy, true, GlobalValue::InternalLinkage,
                                Const, GVName);
  GV->setAlignment(Align(1));
  return GV;
}

/// Generate a static registry table in .ejit_bitcode section (same format as
/// PASS1's generateRegistryTable). The runtime walks [__start_ejit_bitcode,
/// __stop_ejit_bitcode) to consume it. This avoids generating a constructor-
/// based ejit_auto_register that would conflict with the one from lipo/ejit.o.
static void generateRegistryTable(
    Module &M, const SmallVectorImpl<Function *> &EntryFuncs,
    const SmallVectorImpl<GlobalVariable *> &BitcodeGVs,
    const SetVector<Function *> &ClosureFuncs) {
  LLVMContext &Ctx = M.getContext();
  auto *I32Ty = Type::getInt32Ty(Ctx);
  auto *PtrTy = PointerType::getUnqual(Ctx);
  auto *I64Ty = Type::getInt64Ty(Ctx);

  // { i32 type, ptr name1, ptr name2, ptr data, i64 size }
  StructType *EntryTy =
      StructType::get(Ctx, {I32Ty, PtrTy, PtrTy, PtrTy, I64Ty},
                      /*isPacked=*/false);
  SmallVector<Constant *, 16> Entries;

  // EJIT_REG_BITCODE (type=0) per entry function
  for (size_t I = 0; I < EntryFuncs.size(); ++I) {
    Constant *NameStr =
        ConstantDataArray::getString(Ctx, EntryFuncs[I]->getName(), true);
    auto *NameGV = new GlobalVariable(
        M, NameStr->getType(), true, GlobalValue::PrivateLinkage, NameStr,
        ".ejit.str.");
    Entries.push_back(ConstantStruct::get(EntryTy, {
        ConstantInt::get(I32Ty, 0),                          // EJIT_REG_BITCODE
        ConstantExpr::getBitCast(NameGV, PtrTy),             // name1
        ConstantPointerNull::get(PtrTy),                     // name2 = null
        ConstantExpr::getBitCast(BitcodeGVs[I], PtrTy),      // bitcode data
        ConstantInt::get(
            I64Ty, cast<ArrayType>(BitcodeGVs[I]->getValueType())->getNumElements()),
    }));
  }

  // EJIT_REG_SYMBOL (type=3) for external references
  SmallPtrSet<const Function *, 8> SymbolsDone;
  for (Function *F : ClosureFuncs) {
    for (const BasicBlock &BB : *F) {
      for (const Instruction &I : BB) {
        if (const CallBase *CB = dyn_cast<CallBase>(&I)) {
          if (Function *Callee = CB->getCalledFunction()) {
            if (!Callee->isIntrinsic() && Callee->isDeclaration() &&
                SymbolsDone.insert(Callee).second) {
              Constant *NameStr = ConstantDataArray::getString(
                  Ctx, Callee->getName(), true);
              auto *NameGV = new GlobalVariable(
                  M, NameStr->getType(), true,
                  GlobalValue::PrivateLinkage, NameStr, ".ejit.str.");
              Entries.push_back(ConstantStruct::get(EntryTy, {
                  ConstantInt::get(I32Ty, 3),                  // EJIT_REG_SYMBOL
                  ConstantExpr::getBitCast(NameGV, PtrTy),     // name1
                  ConstantPointerNull::get(PtrTy),             // name2 = null
                  ConstantExpr::getBitCast(
                      const_cast<Function *>(Callee), PtrTy),  // addr
                  ConstantInt::get(I64Ty, 0),                  // size = 0
              }));
            }
          }
        }
      }
    }
  }

  auto *ArrTy = ArrayType::get(EntryTy, Entries.size());
  auto *Arr = ConstantArray::get(ArrTy, Entries);
  auto *TableGV = new GlobalVariable(M, ArrTy, false,
                                     GlobalValue::PrivateLinkage, Arr,
                                     ".ejit.registry.bitcode");
  TableGV->setSection(".ejit_bitcode");
  appendToCompilerUsed(M, {TableGV});
}

/// Write the merged module as a bitcode temp file. lld handles .bc
/// natively so there is no need to compile to a .o separately.
static bool writeTempBitcode(Module &M, SmallString<128> &OutPath) {
  if (std::error_code EC =
          sys::fs::createTemporaryFile("ejit_cross", "bc", OutPath)) {
    errs() << "ejit-cross-link: cannot create temp: " << EC.message() << "\n";
    return false;
  }
  std::error_code EC;
  raw_fd_ostream OS(OutPath, EC, sys::fs::OF_None);
  if (EC)
    return false;
  WriteBitcodeToFile(M, OS);
  return true;
}

} // end anonymous namespace

namespace clang {
namespace driver {

std::string runEJitCrossLink(ArrayRef<std::string> InputFiles,
                              const std::string &TargetTriple) {
  // Phase 1: check for .ejit_cross sections
  bool HasCross = false;
  for (const std::string &F : InputFiles) {
    auto Buf = MemoryBuffer::getFile(F);
    if (!Buf)
      continue;
    auto Obj = object::ObjectFile::createObjectFile(
        Buf->get()->getMemBufferRef());
    if (!Obj)
      continue;
    for (const object::SectionRef &Sec : (*Obj)->sections()) {
      auto N = Sec.getName();
      if (N && *N == ".ejit_cross") {
        HasCross = true;
        break;
      }
    }
    if (HasCross)
      break;
  }
  if (!HasCross)
    return {};

  // Phase 2: parse .ejit_cross sections → Modules
  LLVMContext Ctx;
  std::unique_ptr<Module> Composite;

  for (const std::string &F : InputFiles) {
    auto Buf = MemoryBuffer::getFile(F);
    if (!Buf)
      continue;
    auto Obj = object::ObjectFile::createObjectFile(
        Buf->get()->getMemBufferRef());
    if (!Obj)
      continue;
    for (const object::SectionRef &Sec : (*Obj)->sections()) {
      auto N = Sec.getName();
      if (!N || *N != ".ejit_cross")
        continue;
      auto C = Sec.getContents();
      if (!C)
        continue;
      auto M = parseBitcodeFile(MemoryBufferRef(*C, F), Ctx);
      if (!M)
        continue;
      if (!Composite) {
        Composite = std::move(*M);
      } else {
        Linker L(*Composite);
        L.linkInModule(std::move(*M), Linker::Flags::None);
      }
    }
  }
  if (!Composite)
    return {};

  // Phase 3: find ejit_entry functions
  SmallVector<Function *, 4> EntryFuncs;
  collectEntryFunctions(*Composite, EntryFuncs);
  if (EntryFuncs.empty())
    return {};

  // Phase 4: closure + trim
  SetVector<Function *> ClosureFuncs;
  computeTransitiveClosure(EntryFuncs, ClosureFuncs);
  if (!ClosureFuncs.empty()) {
    for (Function &F : make_early_inc_range(Composite->functions())) {
      if (!F.isDeclaration() && !ClosureFuncs.contains(&F))
        F.eraseFromParent();
    }
  }

  // Phase 5: cross-TU inlining
  {
    LoopAnalysisManager LAM;
    FunctionAnalysisManager FAM;
    CGSCCAnalysisManager CGAM;
    ModuleAnalysisManager MAM;
    PassBuilder PB;
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
    ModulePassManager MPM;
    MPM.addPass(AlwaysInlinerPass());
    MPM.run(*Composite, MAM);
  }

  // Phase 6: preOptimize + reAnnotate
  preOptimizeBitcode(*Composite);
  reAnnotateMayConst(*Composite);

  // Phase 7: per-function bitcode
  auto TmpM = std::make_unique<Module>("ejit_cross_link", Ctx);
  TmpM->setDataLayout(Composite->getDataLayoutStr());
  std::string TT = TargetTriple.empty()
                       ? Composite->getTargetTriple().str()
                       : TargetTriple;
  TmpM->setTargetTriple(Triple(TT));

  SmallVector<Function *, 4> ValidEntryFuncs;
  SmallVector<GlobalVariable *, 4> BCGVs;
  for (Function *F : EntryFuncs) {
    auto PerFunc = CloneModule(*Composite);
    Function *ClonedF = PerFunc->getFunction(F->getName());
    if (!ClonedF)
      continue;

    SetVector<Function *> PerClosure;
    SmallVector<Function *, 1> SingleEntry{ClonedF};
    computeTransitiveClosure(SingleEntry, PerClosure);

    for (Function &FF : make_early_inc_range(PerFunc->functions())) {
      if (!FF.isDeclaration() && !PerClosure.contains(&FF))
        FF.eraseFromParent();
    }
    preOptimizeBitcode(*PerFunc);

    BCGVs.push_back(embedBitcode(*TmpM, serializeToBitcode(*PerFunc),
                                  F->getName().str()));
    ValidEntryFuncs.push_back(F);
  }

  // Phase 8: generate ejit_auto_register
  generateRegistryTable(*TmpM, ValidEntryFuncs, BCGVs, ClosureFuncs);

  // Phase 9: write temp bitcode (writeTempBitcode creates the file)
  SmallString<128> TmpPath;
  if (!writeTempBitcode(*TmpM, TmpPath))
    return {};
  return std::string(TmpPath.str());
}

} // namespace driver
} // namespace clang
