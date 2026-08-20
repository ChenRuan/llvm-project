//===-- EJitRegisterBitcode.cpp - EmbeddedJIT Bitcode Extraction ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/EmbeddedJIT/EJitPasses.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/Debug.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/EarlyCSE.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/Process.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/ExecutionEngine/EJIT/EJitRegistryEntry.h"

using namespace llvm;
using namespace llvm::ejit;

extern cl::opt<bool> EnableEJitGlobalCtors;
extern cl::opt<std::string> EJitDumpBitcodeDir;
extern cl::opt<bool> EJitWarnNoSpecialization;
extern cl::opt<bool> EJitWarnUnusedDim;
extern cl::opt<bool> EJitReportMayConst;
extern cl::opt<unsigned> EJitWarnFewMayConst;

#define DEBUG_TYPE "ejit-register-bitcode"

static void collectEntryFunctions(Module &M,
                                  SmallVectorImpl<Function *> &EntryFuncs) {
  for (Function &F : M.functions()) {
    MDNode *MD = F.getMetadata(MD_EJIT_METADATA);
    if (hasMDStringEntry(MD, TAG_EJIT_ENTRY))
      EntryFuncs.push_back(&F);
  }
}

static const GlobalVariable *findRootGV(const Value *V, APInt &Offset,
                                        const DataLayout &DL);

/// Resolve a value to the underlying GlobalVariable it references, walking
/// through bitcasts, addrspacecasts and constant-offset GEP chains (mirrors
/// findRootGV). This is the single definition of "this operand references a
/// global", shared by the closure collector and both symbol-registration
/// emitters. Keeping them on one helper avoids the class of bug where a
/// global is kept in the extracted bitcode (as an external declaration) by
/// the collector but never registered by the emitters — which the JIT linker
/// then fails to resolve.
static GlobalVariable *rootGlobal(Value *V, const DataLayout &DL) {
  APInt Offset;
  return const_cast<GlobalVariable *>(findRootGV(V, Offset, DL));
}

static void collectReferencedGlobals(Function &F,
                                     SetVector<GlobalVariable *> &Globals) {
  const DataLayout &DL = F.getParent()->getDataLayout();
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      for (Value *Op : I.operands())
        if (auto *GV = rootGlobal(Op, DL))
          Globals.insert(GV);
}

static void computeTransitiveClosure(
    const SmallVectorImpl<Function *> &EntryFuncs,
    SetVector<Function *> &ClosureFuncs,
    SetVector<GlobalVariable *> &ClosureGlobals) {

  SmallVector<Function *, 16> Worklist(EntryFuncs.begin(), EntryFuncs.end());
  while (!Worklist.empty()) {
    Function *F = Worklist.pop_back_val();
    if (!ClosureFuncs.insert(F))
      continue;
    collectReferencedGlobals(*F, ClosureGlobals);
    for (BasicBlock &BB : *F)
      for (Instruction &I : BB)
        if (auto *CB = dyn_cast<CallBase>(&I))
          if (Function *Callee = CB->getCalledFunction())
            if (!Callee->isDeclaration() && !Callee->isIntrinsic())
              Worklist.push_back(Callee);
  }
}

/// Walk a GEP chain from a load's pointer operand down to the root
/// GlobalVariable, accumulating the total byte offset.
static const GlobalVariable *findRootGV(const Value *V, APInt &Offset,
                                         const DataLayout &DL) {
  Offset = APInt(DL.getPointerSizeInBits(0), 0);
  while (V) {
    V = V->stripPointerCasts();
    if (isa<GlobalVariable>(V))
      return cast<GlobalVariable>(V);
    auto *GEP = dyn_cast<GEPOperator>(V);
    if (!GEP)
      return nullptr;
    SmallVector<Value *, 4> IdxList;
    for (auto I = GEP->idx_begin(), E = GEP->idx_end(); I != E; ++I) {
      if (!isa<ConstantInt>(*I))
        return nullptr;
      IdxList.push_back(*I);
    }
    Offset += DL.getIndexedOffsetInType(GEP->getSourceElementType(), IdxList);
    V = GEP->getPointerOperand();
  }
  return nullptr;
}

/// Re-annotate loads with !ejit.may_const using GV-level offset metadata.
/// Optimization passes may drop per-load metadata; this restores it from
/// the !ejit.may_const_field entries on the GV's !ejit.metadata.
static void reAnnotateMayConst(Module &M) {
  const DataLayout &DL = M.getDataLayout();
  LLVMContext &Ctx = M.getContext();
  auto MayConstKind = Ctx.getMDKindID(MD_EJIT_MAY_CONST);

  // Build offset map from GV metadata
  DenseMap<const GlobalVariable *, SmallVector<uint64_t, 4>> mayConstMap;
  for (GlobalVariable &GV : M.globals()) {
    MDNode *MD = GV.getMetadata(MD_EJIT_METADATA);
    if (!MD)
      continue;
    SmallVector<uint64_t, 4> offsets;
    for (const MDOperand &Op : MD->operands()) {
      auto *Sub = dyn_cast<MDNode>(Op.get());
      if (!Sub || Sub->getNumOperands() < 2)
        continue;
      auto *Tag = dyn_cast<MDString>(Sub->getOperand(0));
      if (!Tag || Tag->getString() != TAG_EJIT_MAY_CONST_FIELD)
        continue;
      if (auto *CI = mdconst::dyn_extract<ConstantInt>(Sub->getOperand(1)))
        offsets.push_back(CI->getZExtValue());
    }
    if (!offsets.empty())
      mayConstMap[&GV] = std::move(offsets);
  }
  if (mayConstMap.empty())
    return;

  // Re-annotate matching loads
  unsigned count = 0;
  for (Function &F : M.functions()) {
    if (F.isDeclaration())
      continue;
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        auto *LI = dyn_cast<LoadInst>(&I);
        if (!LI || LI->hasMetadata(MayConstKind))
          continue;
        // Never folded, so never re-annotated.
        if (LI->isVolatile() || LI->isAtomic())
          continue;
        // The recorded offsets are element-relative, so match on the field
        // coordinate rather than the total offset from the global.
        const GlobalVariable *GV = nullptr;
        auto Off = ejitMayConstFieldOffset(LI->getPointerOperand(), DL, GV);
        if (!Off || !GV)
          continue;
        auto it = mayConstMap.find(GV);
        if (it == mayConstMap.end())
          continue;
        if (!is_contained(it->second, *Off))
          continue;
        // The offset only says where the load starts. A wider load straddles the
        // next field, which is free to change.
        TypeSize AccessSize = DL.getTypeStoreSize(LI->getType());
        if (AccessSize.isScalable() ||
            !ejitAccessFitsMayConstField(GV, *Off, AccessSize.getFixedValue(),
                                         DL))
          continue;
        LI->setMetadata(MayConstKind, MDNode::get(Ctx, {}));
        count++;
      }
    }
  }
  LLVM_DEBUG(dbgs() << "ejit-register-bitcode: re-annotated " << count
                    << " may_const load(s)\n");
}

/// Run pre-optimization on the extracted bitcode at AOT time to reduce
/// JIT compilation pressure. In debug/shared builds this is a no-op
/// (cyclic link dependency: LLVMPasses <-> LLVMEmbeddedJIT).
#ifdef NDEBUG
static void preOptimizeBitcode(Module &M) {
  PassBuilder PB;
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerModuleAnalyses(MAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  // 1. Inline: AlwaysInline + cost-based inliner for small functions
  {
    ModulePassManager MPM;
    MPM.addPass(AlwaysInlinerPass());
    MPM.addPass(PB.buildModuleInlinerPipeline(
        llvm::OptimizationLevel::O2, ThinOrFullLTOPhase::None));
    MPM.run(M, MAM);
  }

  // 2. Mem2Reg: promote allocas from inlined code to SSA
  {
    FunctionPassManager FPM;
    FPM.addPass(PromotePass());
    for (Function &F : M.functions())
      if (!F.isDeclaration())
        FPM.run(F, FAM);
  }

  // 3. EarlyCSE + InstCombine: simplify and fold redundant computations
  {
    FunctionPassManager FPM;
    FPM.addPass(EarlyCSEPass());
    FPM.addPass(InstCombinePass());
    for (Function &F : M.functions())
      if (!F.isDeclaration())
        FPM.run(F, FAM);
  }

  // 4. SimplifyCFG: flatten branches, merge blocks
  {
    FunctionPassManager FPM;
    FPM.addPass(SimplifyCFGPass());
    for (Function &F : M.functions())
      if (!F.isDeclaration())
        FPM.run(F, FAM);
  }

  // 5. Restore !ejit.may_const metadata that passes may have dropped
  reAnnotateMayConst(M);
}
#else
static void preOptimizeBitcode(Module &) {}
#endif

/// Dump the extracted bitcode module to EJitDumpBitcodeDir (if set) for
/// debugging — e.g. to confirm an ejit_entry function is emitted as a
/// definition rather than just a declaration. Parallel-safe under -j: the
/// filename embeds the process id (distinct per concurrent clang) and the
/// sanitized module name, so no two invocations share a file and writes
/// never serialize. Writes both .ll (readable: grep "define @Fn" vs
/// "declare @Fn") and .bc.
static void dumpExtractedBitcode(const Module &M, StringRef Dir) {
  if (Dir.empty())
    return;
  if (std::error_code EC = sys::fs::create_directories(Dir))
    return;
  std::string Stem;
  StringRef Name = M.getName();
  if (Name.empty())
    Name = "ejit_module";
  auto IsAlnum = [](char C) {
    return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') ||
           (C >= '0' && C <= '9');
  };
  for (char C : Name)
    Stem.push_back(IsAlnum(C) ? C : '_');
  unsigned Pid = sys::Process::getProcessId();
  std::string Base = (Twine(Dir) + "/" + Twine(Pid) + "_" + Stem).str();

  std::error_code EC;
  raw_fd_ostream LL(Base + ".ll", EC);
  if (!EC)
    M.print(LL, nullptr);
  raw_fd_ostream BC(Base + ".bc", EC);
  if (!EC)
    WriteBitcodeToFile(M, BC);
}

/// Diagnostic: count globals carrying !ejit.metadata, and within that the
/// period_arr / may_const_field tags. Gated on EJitDumpBitcodeDir so it only
/// runs when the dump is enabled. Used to pinpoint where the struct-field
/// pass's GV metadata is lost during extraction (clone → preOptimize →
/// externalization).
static void logEJitGlobalMeta(const char *label, const Module &M) {
  if (EJitDumpBitcodeDir.empty())
    return;
  unsigned withMeta = 0, periodArr = 0, mayConstField = 0;
  for (const GlobalVariable &GV : M.globals()) {
    MDNode *MD = GV.getMetadata(MD_EJIT_METADATA);
    if (!MD)
      continue;
    ++withMeta;
    for (const MDOperand &Op : MD->operands()) {
      auto *Sub = dyn_cast<MDNode>(Op.get());
      if (!Sub || Sub->getNumOperands() < 2)
        continue;
      if (auto *Tag = dyn_cast<MDString>(Sub->getOperand(0))) {
        if (Tag->getString() == TAG_EJIT_PERIOD_ARR)
          ++periodArr;
        else if (Tag->getString() == TAG_EJIT_MAY_CONST_FIELD)
          ++mayConstField;
      }
    }
  }
  errs() << "ejit-register-bitcode: " << label
         << " globals=" << M.global_size() << " withMeta=" << withMeta
         << " periodArr=" << periodArr << " mayConstField=" << mayConstField
         << "\n";
}

namespace {

/// Per-function direct info used by the specialization diagnostic.
struct EjitFuncDiagInfo {
  bool HasMayConstLoad = false;
  unsigned MayConstCount = 0;        // # of !ejit.may_const loads (direct)
  unsigned MayConstInLoopCount = 0;  // subset sitting inside a loop
  SetVector<StringRef> RefsPeriodArr; // deduped period names referenced
};

/// One ejit_entry's declared dimensions, parsed from the original module's
/// function metadata (robust to whether the extracted module's function
/// metadata survived clone + preOptimize).
struct EjitEntryDiag {
  std::string Name;
  SmallVector<std::string, 4> DeclaredDims;
};

} // namespace

/// Return true if \p BB lies on a CFG cycle (reachable from itself via a
/// non-empty path), i.e. it may execute more than once. This is a lightweight
/// loop-membership test that avoids pulling LoopInfo / PassBuilder (and their
/// shared-library link dependencies) into LLVMEmbeddedJIT. For natural loops
/// it agrees with LoopInfo; it also accepts irreducible cycles, which is fine
/// for an informational "may execute repeatedly" signal.
static bool isOnCfgCycle(const BasicBlock *BB) {
  SmallPtrSet<const BasicBlock *, 16> Visited;
  SmallVector<const BasicBlock *, 16> WL(successors(BB).begin(),
                                         successors(BB).end());
  while (!WL.empty()) {
    const BasicBlock *N = WL.pop_back_val();
    if (N == BB)
      return true;
    if (!Visited.insert(N).second)
      continue;
    append_range(WL, successors(N));
  }
  return false;
}

/// Collect all basic blocks of \p F that lie on a CFG cycle.
static void computeLoopBBs(Function &F,
                           SmallPtrSetImpl<const BasicBlock *> &LoopBBs) {
  for (const BasicBlock &BB : F)
    if (isOnCfgCycle(&BB))
      LoopBBs.insert(&BB);
}

/// Compute function-local diagnostic info for \p F. \p LoopBBs is
/// optional (non-null only when the may_const count report is enabled).
static void
computeEjitFuncDiagInfo(Function &F, EjitFuncDiagInfo &Info, unsigned MayConstKind,
                        const SmallPtrSetImpl<const BasicBlock *> *LoopBBs) {
  const DataLayout &DL = F.getParent()->getDataLayout();
  SetVector<GlobalVariable *> GVs;
  for (BasicBlock &BB : F) {
    const bool InLoop = LoopBBs && LoopBBs->count(&BB);
    for (Instruction &I : BB) {
      if (auto *Ld = dyn_cast<LoadInst>(&I))
        if (Ld->hasMetadata(MayConstKind)) {
          Info.HasMayConstLoad = true;
          ++Info.MayConstCount;
          if (InLoop)
            ++Info.MayConstInLoopCount;
        }
      // Collect referenced GVs for period-arr lookup in the same traversal.
      for (Value *Op : I.operands())
        if (auto *GV = rootGlobal(Op, DL))
          GVs.insert(GV);
    }
  }
  // Derive period-arr names from collected GVs.
  for (GlobalVariable *GV : GVs) {
    MDNode *GMD = GV->getMetadata(MD_EJIT_METADATA);
    if (!GMD)
      continue;
    for (const MDOperand &Op : GMD->operands()) {
      auto *Sub = dyn_cast<MDNode>(Op.get());
      if (!Sub || Sub->getNumOperands() < 2)
        continue;
      auto *Tag = dyn_cast<MDString>(Sub->getOperand(0));
      if (!Tag || Tag->getString() != TAG_EJIT_PERIOD_ARR)
        continue;
      if (auto *PN = dyn_cast<MDString>(Sub->getOperand(1)))
        Info.RefsPeriodArr.insert(PN->getString());
    }
  }
}

/// AOT specialization diagnostics on the extracted bitcode (post-preOptimize),
/// which is exactly what the JIT will specialize.
///   #1: ejit_entry whose post-inline body reads no ejit_may_const field.
///   #2: ejit_entry that declares ejit_period_arr_ind(P) but its body never
///       indexes an ejit_period_arr(P).
/// AOT pre-inlining has already run. Residual callees execute as AOT and must
/// not contribute to the specialization-value report.
static void
runSpecializationDiagnostic(Module &Extracted,
                            const SmallVectorImpl<Function *> &EntryFuncs) {
  if (!EJitWarnNoSpecialization && !EJitWarnUnusedDim && !EJitReportMayConst &&
      !EJitWarnFewMayConst)
    return;

  SmallVector<EjitEntryDiag, 4> Entries;
  for (const Function *F : EntryFuncs) {
    EjitEntryDiag ED;
    ED.Name = F->getName().str();
    if (MDNode *MD = F->getMetadata(MD_EJIT_METADATA))
      for (const MDOperand &Op : MD->operands()) {
        auto *Sub = dyn_cast<MDNode>(Op.get());
        if (!Sub || Sub->getNumOperands() < 2)
          continue;
        auto *Tag = dyn_cast<MDString>(Sub->getOperand(0));
        if (!Tag || Tag->getString() != TAG_EJIT_PERIOD_ARR_IND)
          continue;
        if (auto *PN = dyn_cast<MDString>(Sub->getOperand(1)))
          ED.DeclaredDims.push_back(PN->getString().str());
      }
    Entries.push_back(std::move(ED));
  }

  unsigned MayConstKind = Extracted.getContext().getMDKindID(MD_EJIT_MAY_CONST);

  // Direct info per defined function in the extracted module. Loop membership
  // (for the may_const count report) uses a lightweight CFG-cycle test so the
  // report does not pull LoopInfo / PassBuilder into LLVMEmbeddedJIT.
  DenseMap<const Function *, EjitFuncDiagInfo> Info;
  for (Function &F : Extracted) {
    if (F.isDeclaration())
      continue;
    SmallPtrSet<const BasicBlock *, 16> LoopBBs;
    bool NeedLoops = EJitReportMayConst || EJitWarnFewMayConst > 0;
    if (NeedLoops)
      computeLoopBBs(F, LoopBBs);
    computeEjitFuncDiagInfo(F, Info[&F], MayConstKind,
                            NeedLoops ? &LoopBBs : nullptr);
  }

  // Emit diagnostics per entry (locate in the extracted module by name).
  for (const EjitEntryDiag &ED : Entries) {
    const Function *EF = Extracted.getFunction(ED.Name);
    if (!EF || EF->isDeclaration())
      continue;
    auto It = Info.find(EF);
    if (It == Info.end())
      continue;

    // #1: no ejit_may_const read in the post-inline entry body.
    if (EJitWarnNoSpecialization && !It->second.HasMayConstLoad)
      errs() << "EJit warning: ejit_entry function '" << EF->getName()
             << "' reads no ejit_may_const field in its post-inline body; "
                "no JIT specialization value, consider removing "
                "ejit_entry\n";

    // #2: declared dimension never referenced by the post-inline body.
    if (EJitWarnUnusedDim)
      for (const std::string &P : ED.DeclaredDims)
        if (It->second.RefsPeriodArr.count(P) == 0)
          errs() << "EJit warning: ejit_entry function '" << EF->getName()
                 << "' declares ejit_period_arr_ind('" << P
                 << "') but its post-inline body never indexes an "
                    "ejit_period_arr('"
                 << P << "'); unused specialization dimension, consider "
                        "removing it\n";
  }

  // Per-entry may_const read counts in the post-inline body.
  if (EJitReportMayConst || EJitWarnFewMayConst > 0) {
    for (const EjitEntryDiag &ED : Entries) {
      const Function *EF = Extracted.getFunction(ED.Name);
      if (!EF || EF->isDeclaration())
        continue;
      auto It = Info.find(EF);
      if (It == Info.end())
        continue;
      unsigned K = It->second.MayConstCount;
      unsigned J = It->second.MayConstInLoopCount;

      // Info report (not a warning): summary of all may-const reads.
      if (EJitReportMayConst)
        errs() << "EJit info: ejit_entry function '" << EF->getName() << "': "
               << K << " ejit_may_const read" << (K == 1 ? "" : "s") << " ("
               << J << " in loops)\n";

      // Warning #3: too few may-const reads for meaningful specialization.
      // A low count means the JIT has little to fold, but doesn't mean the
      // entry is misconfigured — the significance depends on what those loads
      // gate.  This only flags the count for manual review.
      // A load inside a loop is high specialization value regardless of
      // count — a single loop load executes thousands of times.  Only
      // warn when there are no loop loads AND the total is below threshold.
      if (EJitWarnFewMayConst > 0 && K < EJitWarnFewMayConst && J == 0)
        errs() << "EJit warning: ejit_entry function '" << EF->getName()
               << "' has only " << K << " ejit_may_const read"
               << (K == 1 ? "" : "s") << " in its post-inline body"
               << " (threshold: " << EJitWarnFewMayConst
               << "); low specialization surface, consider adding more"
                  " may-const fields\n";
    }
  }
}

struct AotFallbackSymbol {
  std::string JitName;
  Function *AotFunction = nullptr;
};

struct ExtractedBitcode {
  std::string Data;
  SmallVector<AotFallbackSymbol, 16> AotFallbacks;
};

static std::string makeAotFallbackName(const Module &M, StringRef FuncName) {
  MD5 Hash;
  Hash.update(M.getModuleIdentifier());
  Hash.update(StringRef("\0", 1));
  Hash.update(FuncName);
  MD5::MD5Result Result;
  Hash.final(Result);
  return ("__ejit_aot_" + utohexstr(Result.high(), true, 16) +
          utohexstr(Result.low(), true, 16));
}

static Function *createEntryAotFallback(Function &F, StringRef AotName) {
  Function *Clone = Function::Create(
      F.getFunctionType(), GlobalValue::InternalLinkage,
      AotName + ".body", F.getParent());
  Clone->copyAttributesFrom(&F);

  ValueToValueMapTy VMap;
  VMap[&F] = Clone;
  auto DestArg = Clone->arg_begin();
  for (Argument &Arg : F.args()) {
    DestArg->setName(Arg.getName());
    VMap[&Arg] = &*DestArg++;
  }
  SmallVector<ReturnInst *, 8> Returns;
  CloneFunctionInto(Clone, &F, VMap, CloneFunctionChangeType::GlobalChanges,
                    Returns);

  // The copy is a direct AOT body, not another public specialization entry.
  // Clearing EJIT function metadata prevents the later wrapper pass from
  // turning it into a dispatcher while retaining ordinary LLVM attributes.
  Clone->setMetadata(MD_EJIT_METADATA, nullptr);
  Clone->setVisibility(GlobalValue::DefaultVisibility);
  Clone->setDSOLocal(true);
  return Clone;
}

static ExtractedBitcode extractAndSerialize(Module &M,
    const SetVector<Function *> &Funcs,
    const SetVector<GlobalVariable *> &Globals,
    const SmallVectorImpl<Function *> &EntryFuncs) {

  auto Extracted = CloneModule(M);
  DenseSet<StringRef> FuncNames;
  for (Function *F : Funcs)
    FuncNames.insert(F->getName());

  DenseSet<StringRef> GlobalNames;
  for (GlobalVariable *GV : Globals)
    GlobalNames.insert(GV->getName());

  SmallVector<Function *, 16> FuncsToDelete;
  for (Function &F : Extracted->functions())
    if (!FuncNames.count(F.getName()))
      FuncsToDelete.push_back(&F);
  for (Function *F : FuncsToDelete) {
    if (F->isDeclaration())
      continue; // Keep declarations (intrinsics, external refs)
    F->replaceAllUsesWith(UndefValue::get(F->getType()));
    F->deleteBody();
    F->eraseFromParent();
  }

  SmallVector<GlobalVariable *, 16> GVToDelete;
  for (GlobalVariable &GV : Extracted->globals())
    if (!GlobalNames.count(GV.getName()))
      GVToDelete.push_back(&GV);
  for (GlobalVariable *GV : GVToDelete) {
    if (GV->isDeclaration())
      continue; // Keep declarations (external refs)
    GV->replaceAllUsesWith(UndefValue::get(GV->getType()));
    GV->eraseFromParent();
  }

  // Pre-optimize the extracted bitcode to reduce JIT compilation pressure.
  // InstCombine + Mem2Reg + SimplifyCFG folds constant chains, promotes
  // allocas, and cleans up dead branches before serialization.
  logEJitGlobalMeta("extract-after-clone", *Extracted);
  preOptimizeBitcode(*Extracted);
  logEJitGlobalMeta("extract-after-preOpt", *Extracted);

  ExtractedBitcode Result;
  for (Function &F : Extracted->functions()) {
    if (F.isDeclaration() || F.isIntrinsic())
      continue;
    Function *AotF = M.getFunction(F.getName());
    if (!AotF)
      continue;
    std::string AotName = makeAotFallbackName(M, F.getName());
    F.setMetadata(MD_EJIT_AOT_SYMBOL,
                  MDNode::get(F.getContext(), MDString::get(F.getContext(),
                                                            AotName)));
    Function *Fallback = AotF;
    if (hasMDStringEntry(AotF->getMetadata(MD_EJIT_METADATA),
                         TAG_EJIT_ENTRY))
      Fallback = createEntryAotFallback(*AotF, AotName);
    Result.AotFallbacks.push_back({std::move(AotName), Fallback});
  }

  // Specialization diagnostics on the post-preOptimize extracted module (the
  // exact bitcode the JIT will specialize). Must run before the extern
  // conversion below so GV definitions (and their !ejit.metadata) are intact.
  runSpecializationDiagnostic(*Extracted, EntryFuncs);

  // Convert kept non-constant global definitions to external declarations
  // so the JIT linker resolves them from the host process. Constants (e.g.
  // version strings, lookup tables) are kept as-is since they're embedded
  // in the bitcode and don't need external resolution.
  for (GlobalVariable &GV : Extracted->globals()) {
    if (GV.isDeclaration() || GV.isConstant())
      continue;
    GV.setInitializer(nullptr);
    GV.setLinkage(GlobalValue::ExternalLinkage);
  }
  logEJitGlobalMeta("extract-after-extern", *Extracted);

  // Optionally dump the extracted module for debugging (e.g. to confirm an
  // ejit_entry function is emitted as a definition, not a declaration).
  // Parallel-safe: filename embeds PID + module name (see dumpExtractedBitcode).
  if (!EJitDumpBitcodeDir.empty())
    dumpExtractedBitcode(*Extracted, EJitDumpBitcodeDir);

  raw_string_ostream OS(Result.Data);
  WriteBitcodeToFile(*Extracted, OS);
  OS.flush();
  return Result;
}

static GlobalVariable *embedBitcode(Module &M, const std::string &Bitcode) {
  LLVMContext &Ctx = M.getContext();
  SmallVector<uint8_t, 0> Bytes;
  Bytes.reserve(Bitcode.size());
  for (char C : Bitcode)
    Bytes.push_back(static_cast<uint8_t>(C));

  auto *ArrTy = ArrayType::get(Type::getInt8Ty(Ctx), Bitcode.size());
  auto *Const = ConstantDataArray::get(Ctx, Bytes);
  auto *GV = new GlobalVariable(M, ArrTy, true, GlobalValue::InternalLinkage,
                                Const, GV_EJIT_BITCODE);
  GV->setAlignment(Align(1));
  // Bitcode lives in default section (.rodata for const); no custom section
  // needed — bare-metal environments may not support custom ELF sections.
  return GV;
}

static void collectFunctionsFromConstant(Constant *C,
                                         SmallPtrSetImpl<Function *> &Funcs);

/// Collect external symbols (functions + globals) referenced by the
/// closure and generate ejit_register_symbol calls so the JIT can resolve
/// them without dlsym — suitable for bare-metal embedded environments.
static void generateSymbolRegisters(
    Module &M,
    const SetVector<Function *> &ClosureFuncs,
    const SetVector<GlobalVariable *> &ClosureGlobals,
    const SmallVectorImpl<AotFallbackSymbol> &AotFallbacks,
    Function *AutoReg) {
  LLVMContext &Ctx = M.getContext();
  const DataLayout &DL = M.getDataLayout();
  auto *VoidTy = Type::getVoidTy(Ctx);
  auto *PtrTy = PointerType::getUnqual(Ctx);

  M.getOrInsertFunction(FN_REGISTER_SYMBOL,
      FunctionType::get(VoidTy, {PtrTy, PtrTy}, false));

  std::set<std::string> registered;

  auto isPeriodVar = [&](GlobalVariable &GV) -> bool {
    return GV.hasMetadata(MD_EJIT_METADATA);
  };

  BasicBlock *BB = &AutoReg->getEntryBlock();
  Instruction *InsertBefore = BB->getTerminator();

  // Every definition that survived AOT inlining may become a declaration when
  // another entry is specialized. Register it under its TU-unique fallback
  // name, including static helpers and other ejit_entry functions.
  for (const AotFallbackSymbol &Sym : AotFallbacks) {
    if (!Sym.AotFunction || !registered.insert(Sym.JitName).second)
      continue;
    IRBuilder<> Builder(InsertBefore);
    Builder.CreateCall(M.getFunction(FN_REGISTER_SYMBOL),
                       {Builder.CreateGlobalString(Sym.JitName),
                        Builder.CreateBitCast(Sym.AotFunction, PtrTy)});
  }

  for (Function *F : ClosureFuncs) {
    for (BasicBlock &Blk : *F) {
      for (Instruction &I : Blk) {
        // External function calls
        if (auto *CI = dyn_cast<CallInst>(&I)) {
          if (Function *Callee = CI->getCalledFunction()) {
            if (Callee->isDeclaration() && !Callee->isIntrinsic()) {
              std::string Name = Callee->getName().str();
              if (registered.insert(Name).second) {
                IRBuilder<> Builder(InsertBefore);
                Builder.CreateCall(M.getFunction(FN_REGISTER_SYMBOL),
                    {Builder.CreateGlobalString(Name),
                     Builder.CreateBitCast(Callee, PtrTy)});
              }
            }
          }
        }
        // External global variable references. A const global *with a local
        // definition* (initializer) is embedded in the extracted bitcode by
        // extractAndSerialize, so it needs no registration. A const global that
        // is only a *declaration* (extern const, no initializer in this TU)
        // cannot be embedded and must be resolved from the host process at JIT
        // link time, so it MUST be registered — dropping it leaves an
        // unresolved external that fails JITLink. Resolve through bitcasts/GEPs
        // via rootGlobal so every global the collector kept in the extracted
        // bitcode is actually registered here.
        for (Use &U : I.operands()) {
          auto *GV = rootGlobal(U.get(), DL);
          if (!GV || (GV->isConstant() && !GV->isDeclaration()))
            continue;
          if (GV->isDeclaration() || !isPeriodVar(*GV)) {
            std::string Name = GV->getName().str();
            if (registered.insert(Name).second) {
              IRBuilder<> Builder(InsertBefore);
              Builder.CreateCall(M.getFunction(FN_REGISTER_SYMBOL),
                  {Builder.CreateGlobalString(Name),
                   Builder.CreateBitCast(GV, PtrTy)});
            }
          }
        }
      }
    }
  }

  // Scan GlobalVariable initializers for function pointers stored in
  // constant aggregates (e.g., const arrays of structs with fn_ptr
  // fields used as indirect-call tables).  The instruction-level scan
  // above only catches direct calls and direct GV operand references;
  // indirect calls through loaded function pointers are missed because
  // CI->getCalledFunction() returns nullptr.
  for (GlobalVariable *GV : ClosureGlobals) {
    if (!GV->hasInitializer())
      continue;
    SmallPtrSet<Function *, 8> FuncsInInit;
    collectFunctionsFromConstant(GV->getInitializer(), FuncsInInit);
    for (Function *F : FuncsInInit) {
      if (!F->isDeclaration() || F->isIntrinsic())
        continue;
      std::string Name = F->getName().str();
      if (registered.insert(Name).second) {
        IRBuilder<> Builder(InsertBefore);
        Builder.CreateCall(M.getFunction("ejit_register_symbol"),
            {Builder.CreateGlobalString(Name),
             Builder.CreateBitCast(F, PtrTy)});
      }
    }
  }
}

/// Recursively walk a Constant (initializer of a GlobalVariable) and
/// collect all Function declarations reachable through constant
/// aggregates, structs, and expressions.  This discovers indirect-call
/// targets stored in jump tables / callback arrays that are missed by
/// the instruction-level direct-call scan.
static void collectFunctionsFromConstant(Constant *C,
                                         SmallPtrSetImpl<Function *> &Funcs) {
  if (auto *F = dyn_cast<Function>(C)) {
    Funcs.insert(F);
    return;
  }
  // Stop at GlobalValues (other than Function, which is handled above)
  // to avoid following references to other global variables.
  if (isa<GlobalValue>(C))
    return;
  for (Value *Op : C->operands())
    collectFunctionsFromConstant(cast<Constant>(Op), Funcs);
}

static void
generateRegistryTable(Module &M, const SmallVectorImpl<Function *> &EntryFuncs,
                      const SetVector<Function *> &ClosureFuncs,
                      const SetVector<GlobalVariable *> &ClosureGlobals,
                      const SmallVectorImpl<AotFallbackSymbol> &AotFallbacks,
                      GlobalVariable *BitcodeGV);

static void generateRegisterCall(Module &M, GlobalVariable *BitcodeGV,
                                 const SmallVectorImpl<Function *> &EntryFuncs,
                                 const SetVector<Function *> &ClosureFuncs,
                                 const SetVector<GlobalVariable *> &ClosureGlobals,
                                 const SmallVectorImpl<AotFallbackSymbol> &AotFallbacks) {
  LLVMContext &Ctx = M.getContext();
  auto *VoidTy = Type::getVoidTy(Ctx);
  auto *PtrTy = PointerType::getUnqual(Ctx);
  auto *I64Ty = Type::getInt64Ty(Ctx);

  M.getOrInsertFunction(FN_REGISTER_BITCODE,
      FunctionType::get(VoidTy, {PtrTy, PtrTy, I64Ty}, false));

  Function *AutoReg = M.getFunction(FN_AUTO_REGISTER);
  if (!AutoReg) {
    AutoReg = Function::Create(FunctionType::get(VoidTy, false),
                               GlobalValue::InternalLinkage,
                               FN_AUTO_REGISTER, &M);
    BasicBlock::Create(Ctx, "entry", AutoReg);
    ReturnInst::Create(Ctx, &AutoReg->getEntryBlock());
  }

  BasicBlock *EntryBB = &AutoReg->getEntryBlock();
  Instruction *Ret = EntryBB->getTerminator();
  FunctionCallee Callee = M.getFunction(FN_REGISTER_BITCODE);

  for (Function *F : EntryFuncs) {
    IRBuilder<> Builder(Ret);
    Builder.CreateCall(Callee, {
        Builder.CreateGlobalString(F->getName()),
        Builder.CreateBitCast(BitcodeGV, PtrTy),
        ConstantInt::get(I64Ty, BitcodeGV->getValueType()->getArrayNumElements())
    });
  }

  // Auto-register external symbols referenced by the closure so the JIT
  // can resolve them without manual ejit_register_symbol calls.
  generateSymbolRegisters(M, ClosureFuncs, ClosureGlobals, AotFallbacks,
                          AutoReg);

  if (EnableEJitGlobalCtors)
    appendToGlobalCtors(M, AutoReg, EJIT_CTOR_PRIORITY);

  // Always build the static registry table for bare-metal / testing fallback.
  generateRegistryTable(M, EntryFuncs, ClosureFuncs, ClosureGlobals,
                        AotFallbacks, BitcodeGV);
}

/// Emit this translation unit's bitcode registry entries as a private array in
/// the ".ejit_bitcode" section. The linker concatenates these across all TUs;
/// a linker script brackets the section so ejit_init() can walk the
/// [__start_ejit_bitcode, __stop_ejit_bitcode) range on bare-metal where global
/// constructors are unavailable.
static void
generateRegistryTable(Module &M, const SmallVectorImpl<Function *> &EntryFuncs,
                      const SetVector<Function *> &ClosureFuncs,
                      const SetVector<GlobalVariable *> &ClosureGlobals,
                      const SmallVectorImpl<AotFallbackSymbol> &AotFallbacks,
                      GlobalVariable *BitcodeGV) {
  LLVMContext &Ctx = M.getContext();
  auto *I32Ty = Type::getInt32Ty(Ctx);
  auto *PtrTy = PointerType::getUnqual(Ctx);
  auto *I64Ty = Type::getInt64Ty(Ctx);

  // Struct: { i32 type, ptr name1, ptr name2, ptr data, i64 size }
  StructType *EntryTy = StructType::get(
      Ctx, {I32Ty, PtrTy, PtrTy, PtrTy, I64Ty}, /*isPacked=*/false);

  SmallVector<Constant *, 16> Entries;

  // Bitcode entries — use CreateGlobalStringPtr to avoid name clashes
  // with existing functions in the module.
  for (Function *F : EntryFuncs) {
    Constant *NameStr = ConstantDataArray::getString(Ctx, F->getName(), true);
    auto *NameGV = new GlobalVariable(M, NameStr->getType(), true,
        GlobalValue::PrivateLinkage, NameStr, ".ejit.str.");
    Entries.push_back(ConstantStruct::get(EntryTy, {
        ConstantInt::get(I32Ty, EJIT_REG_BITCODE),           // EJIT_REG_BITCODE
        ConstantExpr::getBitCast(NameGV, PtrTy),             // name1 string
        ConstantPointerNull::get(PtrTy),                     // name2 = NULL
        ConstantExpr::getBitCast(BitcodeGV, PtrTy),          // bitcode data ptr
        ConstantInt::get(I64Ty,
            BitcodeGV->getValueType()->getArrayNumElements()),// bitcode size
    }));
  }

  // Function symbols: TU-unique AOT fallbacks for retained definitions, plus
  // ordinary names for pre-existing external declarations.
  std::set<std::string> SymbolsDone;
  auto addNamedSymbol = [&](StringRef Name, const Function *F) {
    if (!F || F->isIntrinsic() || Name.empty() ||
        !SymbolsDone.insert(Name.str()).second)
      return;
    Constant *NameStr = ConstantDataArray::getString(Ctx, Name, true);
    auto *NameGV = new GlobalVariable(M, NameStr->getType(), true,
        GlobalValue::PrivateLinkage, NameStr, ".ejit.str.");
    Entries.push_back(ConstantStruct::get(EntryTy, {
        ConstantInt::get(I32Ty, EJIT_REG_SYMBOL),
        ConstantExpr::getBitCast(NameGV, PtrTy),
        ConstantPointerNull::get(PtrTy),
        ConstantExpr::getBitCast(const_cast<Function *>(F), PtrTy),
        ConstantInt::get(I64Ty, 0),
    }));
  };
  auto addExternalSymbol = [&](const Function *F) {
    if (F && F->isDeclaration())
      addNamedSymbol(F->getName(), F);
  };
  for (const AotFallbackSymbol &Sym : AotFallbacks)
    addNamedSymbol(Sym.JitName, Sym.AotFunction);
  for (Function *F : ClosureFuncs) {
    for (const BasicBlock &BB : *F) {
      for (const Instruction &I : BB) {
        if (const CallBase *CB = dyn_cast<CallBase>(&I))
          if (Function *Callee = CB->getCalledFunction())
            addExternalSymbol(Callee);
      }
    }
  }

  // Also collect external function declarations referenced through
  // GlobalVariable initializers (e.g., function pointers stored in
  // constant struct arrays used as indirect-call targets).  The
  // instruction scanning loop above only catches direct calls.
  for (GlobalVariable *GV : ClosureGlobals) {
    if (!GV->hasInitializer())
      continue;
    SmallPtrSet<Function *, 8> FuncsInInit;
    collectFunctionsFromConstant(GV->getInitializer(), FuncsInInit);
    for (Function *F : FuncsInInit)
      addExternalSymbol(F);
  }

  // Global variable symbol entries. Resolve through bitcasts/GEPs via
  // rootGlobal so registration matches what collectReferencedGlobals kept in
  // the extracted bitcode.
  SmallPtrSet<const GlobalVariable *, 4> GVsDone;
  const DataLayout &DL = M.getDataLayout();
  for (Function *F : ClosureFuncs) {
    for (const BasicBlock &BB : *F) {
      for (const Instruction &I : BB) {
        for (const Value *Op : I.operands()) {
          const GlobalVariable *GV =
              rootGlobal(const_cast<Value *>(Op), DL);
          // Skip const globals that have a local definition (they're embedded
          // in the bitcode), but keep const *declarations* (extern const) so
          // they get registered and resolved from the host at JIT link time.
          if (!GV || (GV->isConstant() && !GV->isDeclaration()) ||
              GV->getName().starts_with("llvm."))
            continue;
          if (!GVsDone.insert(GV).second)
            continue;
          Constant *NameStr = ConstantDataArray::getString(Ctx, GV->getName(), true);
          auto *NameGV = new GlobalVariable(M, NameStr->getType(), true,
              GlobalValue::PrivateLinkage, NameStr, ".ejit.str.");
          Entries.push_back(ConstantStruct::get(EntryTy, {
              ConstantInt::get(I32Ty, EJIT_REG_SYMBOL),                // EJIT_REG_SYMBOL
              ConstantExpr::getBitCast(NameGV, PtrTy),   // name1 string
              ConstantPointerNull::get(PtrTy),
              ConstantExpr::getBitCast(
                  const_cast<GlobalVariable *>(GV), PtrTy),
              ConstantInt::get(I64Ty, 0),
          }));
        }
      }
    }
  }

  // No sentinel entry: the runtime iterates the linker-provided
  // [__start_ejit_bitcode, __stop_ejit_bitcode) range over the dedicated
  // section, so each translation unit contributes only its own entries.
  if (Entries.empty())
    return;

  ArrayType *ArrayTy = ArrayType::get(EntryTy, Entries.size());
  Constant *ArrayInit = ConstantArray::get(ArrayTy, Entries);

  // Private linkage + a dedicated section. Every TU emits its own *local*
  // array into ".ejit_bitcode"; the linker concatenates them across TUs. The
  // leading-dot name is the conventional ELF spelling but is NOT a valid C
  // identifier, so the linker does NOT auto-synthesize __start_/__stop_ — a
  // linker script must bracket the section (see ejit_registry.ld). A fixed
  // *external* symbol here would instead produce "duplicate symbol" link
  // errors as soon as more than one TU defines ejit_entry functions.
  // llvm.used keeps the array alive under --gc-sections.
  auto *GV = new GlobalVariable(M, ArrayTy, /*isConstant=*/true,
                                GlobalValue::PrivateLinkage, ArrayInit,
                                ".ejit.registry.bitcode");
  GV->setSection(SECT_EJIT_BITCODE);
  GV->setAlignment(M.getDataLayout().getABITypeAlign(EntryTy));
  appendToUsed(M, {GV});
}

PreservedAnalyses
EJitRegisterBitcodePass::run(Module &M, ModuleAnalysisManager &) {
  LLVM_DEBUG(dbgs() << "ejit-register-bitcode: running on " << M.getName() << "\n");
  SmallVector<Function *, 4> EntryFuncs;
  collectEntryFunctions(M, EntryFuncs);
  if (EntryFuncs.empty()) {
    LLVM_DEBUG(dbgs() << "ejit-register-bitcode: no entry functions, skip\n");
    return PreservedAnalyses::all();
  }

  SetVector<Function *> ClosureFuncs;
  SetVector<GlobalVariable *> ClosureGlobals;
  computeTransitiveClosure(EntryFuncs, ClosureFuncs, ClosureGlobals);
  LLVM_DEBUG(dbgs() << "ejit-register-bitcode: closure " << ClosureFuncs.size()
                    << " funcs, " << ClosureGlobals.size() << " globals\n");
  if (ClosureFuncs.empty())
    return PreservedAnalyses::all();

  ExtractedBitcode Bitcode =
      extractAndSerialize(M, ClosureFuncs, ClosureGlobals, EntryFuncs);
  GlobalVariable *BitcodeGV = embedBitcode(M, Bitcode.Data);
  generateRegisterCall(M, BitcodeGV, EntryFuncs, ClosureFuncs, ClosureGlobals,
                       Bitcode.AotFallbacks);

  return PreservedAnalyses::none();
}
