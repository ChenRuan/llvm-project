//===-- EJitStructFieldPass.cpp - JIT Constant Substitution ---------------===//

#include "llvm/ExecutionEngine/EJIT/EJitStructFieldPass.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include <cassert>
#include <cstring>

using namespace llvm;
using namespace llvm::ejit;

#define DEBUG_TYPE "ejit-struct-field"

void EJitStructFieldPass::initFromModule(Module &M) {
  EJIT_DIAG_VERBOSE("struct-field initFromModule module=%s globals=%zu",
                    M.getName().str().c_str(), M.global_size());
  // Build GV period map.
  for (GlobalVariable &GV : M.globals()) {
    MDNode *MD = GV.getMetadata(MD_EJIT_METADATA);
    if (!MD)
      continue;

    for (const MDOperand &Op : MD->operands()) {
      auto *Sub = dyn_cast<MDNode>(Op.get());
      if (!Sub || Sub->getNumOperands() < 2)
        continue;

      auto *Tag = dyn_cast<MDString>(Sub->getOperand(0));
      if (!Tag)
        continue;

      if (Tag->getString() == TAG_EJIT_PERIOD_ARR) {
        auto *PN = dyn_cast<MDString>(Sub->getOperand(1));
        size_t sz = 0;
        if (Sub->getNumOperands() >= 3)
          if (auto *CI = mdconst::dyn_extract<ConstantInt>(Sub->getOperand(2)))
            sz = CI->getZExtValue();
        if (PN)
          gvPeriodMap_[&GV] = {PN->getString().str(), true, sz};
      } else if (Tag->getString() == TAG_EJIT_PERIOD) {
        auto *PN = dyn_cast<MDString>(Sub->getOperand(1));
        std::string pn = PN ? PN->getString().str() : "";
        gvPeriodMap_[&GV] = {pn, false, 0};
      }
    }

    // Build may_const field offset map (v1.7 fallback for when
    // optimization passes drop per-load !ejit.may_const metadata).
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
      mayConstFieldMap_[&GV] = std::move(offsets);
  }

  mapsBuilt_ = true;
#ifdef EJIT_DIAG_ENABLE
  EJIT_DIAG_DEBUG("struct-field initFromModule module=%s globals=%zu "
                  "gvPeriod=%zu mayConstField=%zu",
                  M.getName().str().c_str(), M.global_size(), gvPeriodMap_.size(),
                  mayConstFieldMap_.size());
#endif
}

//===----------------------------------------------------------------------===//
// IR analysis helpers
//===----------------------------------------------------------------------===//

/// Walk pointer casts and GEP chains up to the root GlobalVariable.
static const GlobalVariable *findRootGV(const Value *V) {
  V = V->stripPointerCasts();
  if (auto *GV = dyn_cast<GlobalVariable>(V))
    return GV;
  if (auto *GEP = dyn_cast<GEPOperator>(V))
    return findRootGV(GEP->getPointerOperand());
  return nullptr;
}

static const Argument *findRootArgument(const Value *V) {
  V = V->stripPointerCasts();
  if (auto *Arg = dyn_cast<Argument>(V))
    return Arg;
  if (auto *GEP = dyn_cast<GEPOperator>(V))
    return findRootArgument(GEP->getPointerOperand());
  return nullptr;
}

static bool functionBindsArgument(const Function &F, unsigned ArgIndex) {
  MDNode *MD = F.getMetadata(MD_EJIT_METADATA);
  if (!MD)
    return false;
  for (const MDOperand &Op : MD->operands()) {
    auto *Sub = dyn_cast<MDNode>(Op.get());
    if (!Sub || Sub->getNumOperands() < 3)
      continue;
    auto *Tag = dyn_cast<MDString>(Sub->getOperand(0));
    auto *Idx = mdconst::dyn_extract<ConstantInt>(Sub->getOperand(2));
    if (Tag && Tag->getString() == TAG_EJIT_BOUND_PTR && Idx &&
        Idx->getZExtValue() == ArgIndex)
      return true;
  }
  return false;
}

static std::optional<uint64_t> accumulateArgumentOffset(const DataLayout &DL,
                                                        const Value *PtrOp,
                                                        const Argument *Root);

static bool isBoundMayConstLoad(LoadInst *LI, const Function &F,
                                unsigned ArgIndex, const DataLayout &DL) {
  if (LI->isVolatile() || LI->isAtomic() || ArgIndex >= F.arg_size())
    return false;
  const Argument *Root = findRootArgument(LI->getPointerOperand());
  if (!Root || Root->getArgNo() != ArgIndex)
    return false;
  if (LI->hasMetadata(MD_EJIT_MAY_CONST))
    return true;

  auto Offset = accumulateArgumentOffset(DL, LI->getPointerOperand(), Root);
  TypeSize AccessSize = DL.getTypeStoreSize(LI->getType());
  if (!Offset || AccessSize.isScalable())
    return false;

  MDNode *MD = F.getMetadata(MD_EJIT_METADATA);
  if (!MD)
    return false;
  for (const MDOperand &Op : MD->operands()) {
    auto *Sub = dyn_cast<MDNode>(Op.get());
    if (!Sub || Sub->getNumOperands() < 4)
      continue;
    auto *Tag = dyn_cast<MDString>(Sub->getOperand(0));
    auto *Idx = mdconst::dyn_extract<ConstantInt>(Sub->getOperand(2));
    if (!Tag || Tag->getString() != TAG_EJIT_BOUND_PTR || !Idx ||
        Idx->getZExtValue() != ArgIndex)
      continue;
    for (unsigned I = 4; I < Sub->getNumOperands(); ++I) {
      auto *Field = dyn_cast<MDNode>(Sub->getOperand(I));
      if (!Field || Field->getNumOperands() != 2)
        continue;
      auto *FieldOffset =
          mdconst::dyn_extract<ConstantInt>(Field->getOperand(0));
      auto *FieldSize = mdconst::dyn_extract<ConstantInt>(Field->getOperand(1));
      if (FieldOffset && FieldSize) {
        uint64_t Begin = FieldOffset->getZExtValue();
        uint64_t Size = FieldSize->getZExtValue();
        if (*Offset >= Begin && *Offset - Begin <= Size &&
            AccessSize.getFixedValue() <= Size - (*Offset - Begin))
          return true;
      }
    }
  }
  return false;
}

/// If all GEP indices are ConstantInt, compute the cumulative byte offset.
/// Returns std::nullopt if any index is not constant.
static std::optional<uint64_t>
computeGEPOffset(const GEPOperator *GEP, const DataLayout &DL) {
  SmallVector<Value *, 4> IdxList;
  for (auto I = GEP->idx_begin(), E = GEP->idx_end(); I != E; ++I) {
    if (!isa<ConstantInt>(*I))
      return std::nullopt;
    IdxList.push_back(*I);
  }
  return DL.getIndexedOffsetInType(GEP->getSourceElementType(), IdxList);
}

/// Walk a GEP chain from the load's pointer operand down to the root
/// global variable, accumulating the total byte offset. All GEP indices
/// must be constants (already folded by InstCombine after param substitution).
static std::optional<uint64_t>
accumulateFullOffset(const DataLayout &DL, const Value *PtrOp) {
  APInt total(DL.getPointerSizeInBits(0), 0);

  while (PtrOp) {
    PtrOp = PtrOp->stripPointerCasts();
    if (isa<GlobalVariable>(PtrOp))
      break;

    auto *GEP = dyn_cast<GEPOperator>(PtrOp);
    if (!GEP)
      return std::nullopt;

    auto off = computeGEPOffset(GEP, DL);
    if (!off)
      return std::nullopt;
    total += APInt(total.getBitWidth(), *off);

    PtrOp = GEP->getPointerOperand();
  }

  return total.getZExtValue();
}

static std::optional<uint64_t> accumulateArgumentOffset(const DataLayout &DL,
                                                        const Value *PtrOp,
                                                        const Argument *Root) {
  APInt Total(DL.getPointerSizeInBits(0), 0);
  while (PtrOp) {
    PtrOp = PtrOp->stripPointerCasts();
    if (PtrOp == Root)
      return Total.getZExtValue();
    auto *GEP = dyn_cast<GEPOperator>(PtrOp);
    if (!GEP)
      return std::nullopt;
    auto Off = computeGEPOffset(GEP, DL);
    if (!Off)
      return std::nullopt;
    Total += APInt(Total.getBitWidth(), *Off);
    PtrOp = GEP->getPointerOperand();
  }
  return std::nullopt;
}

/// Check whether a load is (or can be treated as) a may_const access.
static bool
isMayConstLoad(const LoadInst *LI, const MayConstOffsetMap &mayConstFieldMap,
               const DataLayout &DL) {
  // Never substituted. Checked ahead of the metadata, not just ahead of the
  // fallback, so that a pass which copies !ejit.may_const onto a volatile or
  // atomic load cannot defeat the frontend's exclusion.
  if (LI->isVolatile() || LI->isAtomic())
    return false;

  if (LI->hasMetadata(MD_EJIT_MAY_CONST))
    return true;

  // v1.7 fallback for loads whose marker an earlier pass dropped. The recorded
  // offsets are element-relative, so match on the field coordinate rather than
  // the total offset from the global, and require the access to fit inside the
  // field it starts at.
  const GlobalVariable *RootGV = nullptr;
  auto Off = ejitMayConstFieldOffset(LI->getPointerOperand(), DL, RootGV);
  if (Off && RootGV) {
    auto It = mayConstFieldMap.find(RootGV);
    if (It != mayConstFieldMap.end() && is_contained(It->second, *Off)) {
      TypeSize AccessSize = DL.getTypeStoreSize(LI->getType());
      if (!AccessSize.isScalable() &&
          ejitAccessFitsMayConstField(RootGV, *Off, AccessSize.getFixedValue(),
                                      DL))
        return true;
    }
  }
  return false;
}

#ifdef EJIT_SRE_PGO_BRANCH_AUDIT
std::vector<EJitMayConstLoadSite>
EJitStructFieldPass::collectMayConstLoadSites(const Module &M) const {
  assert(mapsBuilt_ && "initFromModule() must precede may_const audit");
  std::vector<EJitMayConstLoadSite> Sites;
  const DataLayout &DL = M.getDataLayout();
  for (const Function &F : M) {
    if (F.isDeclaration())
      continue;
    for (const BasicBlock &BB : F) {
      for (const Instruction &I : BB) {
        const auto *LI = dyn_cast<LoadInst>(&I);
        if (!LI || !isMayConstLoad(LI, mayConstFieldMap_, DL))
          continue;

        EJitMayConstLoadSite Site;
        Site.functionName = F.getName().str();
        if (MDNode *SiteMD = LI->getMetadata(MayConstAuditSiteMD))
          if (SiteMD->getNumOperands() == 1)
            if (auto *SiteID =
                    mdconst::dyn_extract<ConstantInt>(SiteMD->getOperand(0)))
              Site.siteId = SiteID->getZExtValue();
        const GlobalVariable *RootGV = nullptr;
        if (auto Offset =
                ejitMayConstFieldOffset(LI->getPointerOperand(), DL, RootGV)) {
          Site.fieldOffset = *Offset;
          Site.hasFieldOffset = true;
        }
        if (RootGV)
          Site.globalName = RootGV->getName().str();
        if (DebugLoc Loc = LI->getDebugLoc()) {
          Site.sourceFile = Loc->getFilename().str();
          Site.sourceLine = Loc.getLine();
          Site.sourceColumn = Loc.getCol();
        }
        Sites.push_back(std::move(Site));
      }
    }
  }
  return Sites;
}

std::vector<EJitMayConstLoadSite>
EJitStructFieldPass::instrumentMayConstLoadSites(Module &M) {
  std::vector<EJitMayConstLoadSite> Sites = collectMayConstLoadSites(M);
  if (Sites.empty())
    return Sites;

  LLVMContext &Ctx = M.getContext();
  Type *I64 = Type::getInt64Ty(Ctx);
  ArrayType *CounterTy = ArrayType::get(I64, Sites.size());
  auto *Counters = new GlobalVariable(
      M, CounterTy, false, GlobalValue::ExternalLinkage,
      ConstantAggregateZero::get(CounterTy), MayConstCounterName);
  Counters->setAlignment(Align(8));

  const DataLayout &DL = M.getDataLayout();
  uint64_t SiteIndex = 0;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        auto *LI = dyn_cast<LoadInst>(&I);
        if (!LI || !isMayConstLoad(LI, mayConstFieldMap_, DL))
          continue;

        const uint64_t SiteID = SiteIndex + 1;
        LI->setMetadata(
            MayConstAuditSiteMD,
            MDNode::get(Ctx, ConstantAsMetadata::get(
                                 ConstantInt::get(I64, SiteID))));
        Sites[SiteIndex].siteId = SiteID;

        IRBuilder<> Builder(LI);
        Value *Counter = Builder.CreateInBoundsGEP(
            CounterTy, Counters,
            {Builder.getInt32(0), Builder.getInt64(SiteIndex)});
        Builder.CreateAtomicRMW(AtomicRMWInst::Add, Counter,
                                Builder.getInt64(1), Align(8),
                                AtomicOrdering::Monotonic);
        ++SiteIndex;
      }
    }
  }
  assert(SiteIndex == Sites.size() && "may_const site inventory changed");
  return Sites;
}

void EJitStructFieldPass::removeMayConstLoadInstrumentation(Module &M) {
  GlobalVariable *Counters = M.getGlobalVariable(MayConstCounterName);
  if (!Counters)
    return;

  SmallVector<AtomicRMWInst *, 16> Increments;
  for (Function &F : M)
    for (BasicBlock &BB : F)
      for (Instruction &I : BB)
        if (auto *RMW = dyn_cast<AtomicRMWInst>(&I))
          if (getUnderlyingObject(RMW->getPointerOperand()) == Counters)
            Increments.push_back(RMW);

  for (AtomicRMWInst *RMW : Increments) {
    auto *Address = dyn_cast<Instruction>(RMW->getPointerOperand());
    RMW->eraseFromParent();
    if (Address)
      RecursivelyDeleteTriviallyDeadInstructions(Address);
  }
  assert(Counters->use_empty() && "may_const counter still has users");
  Counters->eraseFromParent();
}
#endif

//===----------------------------------------------------------------------===//
// Runtime value helpers
//===----------------------------------------------------------------------===//

/// Resolve the runtime base address of a global variable (array or static).
static void *resolveBase(const GlobalVariable *GV, const GVPeriodInfo &info,
                         PeriodArrayRegistry &reg) {
  if (info.isArray) {
    const auto *arrs = reg.getArrays(info.periodName);
    if (!arrs || arrs->empty())
      return nullptr;
    if (arrs->size() == 1)
      return arrs->front().baseAddr;
    const auto *paInfo = reg.getArrayInfo(GV->getName().str());
    return paInfo ? paInfo->baseAddr : nullptr;
  }
  return reg.getStaticVarAddr(GV->getName().str());
}

/// Create an LLVM Constant from raw memory bytes.
static Constant *createConstantFromMemory(const void *addr, Type *Ty,
                                          const DataLayout &DL) {
  LLVMContext &Ctx = Ty->getContext();
  unsigned byteSize = DL.getTypeStoreSize(Ty);

  if (Ty->isIntegerTy()) {
    // Only integers that fit in a single 64-bit word are materialized here.
    // Wider integers (e.g. __int128, or _BitInt(N) with N > 64) are left
    // un-substituted.
    if (byteSize > 8)
      return nullptr;
    uint64_t raw = 0;
    std::memcpy(&raw, addr, byteSize);
    if (!DL.isLittleEndian())
      raw >>= (8 - byteSize) * 8;
    return ConstantInt::get(Ty, APInt(byteSize * 8, raw));
  }
  if (Ty->isFloatTy()) {
    float v;
    std::memcpy(&v, addr, sizeof(v));
    return ConstantFP::get(Ty, v);
  }
  if (Ty->isDoubleTy()) {
    double v;
    std::memcpy(&v, addr, sizeof(v));
    return ConstantFP::get(Ty, v);
  }
  if (Ty->isPointerTy()) {
    uint64_t raw = 0;
    std::memcpy(&raw, addr, sizeof(raw));
    return ConstantExpr::getIntToPtr(
        ConstantInt::get(Type::getInt64Ty(Ctx), raw), Ty);
  }
  return nullptr;
}

/// Replace a load of a pointer-valued period global with the registered
/// pointee address. Pointer-form ejit_period/ejit_period_arr globals are
/// registered by the address of their pointer slot, so resolveBase() returns
/// the slot and createConstantFromMemory() performs the required dereference.
///
/// This load is intentionally not required to carry !ejit.may_const: the
/// pointer is the address root of the period object, while the annotation
/// belongs to fields inside that object. Materializing the root lets IPSCCP
/// propagate it through a non-inlined helper, after which the normal
/// may_const-field replacement can resolve the field load.
static Constant *
tryReplacePeriodPointerBase(LoadInst *LI, const GVPeriodMap &gvMap,
                            PeriodArrayRegistry &reg, const DataLayout &DL) {
  if (LI->isVolatile() || LI->isAtomic() || !LI->getType()->isPointerTy())
    return nullptr;

  auto *GV = dyn_cast<GlobalVariable>(
      LI->getPointerOperand()->stripPointerCasts());
  if (!GV || !GV->getValueType()->isPointerTy())
    return nullptr;

  auto It = gvMap.find(GV);
  if (It == gvMap.end())
    return nullptr;

  void *PointerSlot = resolveBase(GV, It->second, reg);
  if (!PointerSlot)
    return nullptr;
  return createConstantFromMemory(PointerSlot, LI->getType(), DL);
}

/// Resolve a may_const load after IPSCCP/InstCombine has propagated a period
/// pointer into a callee and folded its GEP into an absolute address. Accept
/// the address only when it matches a declared may_const field of a registered
/// pointer-form period object; arbitrary inttoptr loads must remain untouched.
static Constant *tryReplacePeriodAbsoluteAddress(
    LoadInst *LI, const GVPeriodMap &gvMap,
    const MayConstOffsetMap &mayConstFieldMap, PeriodArrayRegistry &reg,
    const DataLayout &DL) {
  const Value *Ptr = LI->getPointerOperand();
  APInt ExtraOffset(DL.getIndexTypeSizeInBits(Ptr->getType()), 0);
  const Value *Base = Ptr->stripAndAccumulateConstantOffsets(
      DL, ExtraOffset, /*AllowNonInbounds=*/true);

  auto *IntToPtr = dyn_cast<ConstantExpr>(Base);
  if (!IntToPtr || IntToPtr->getOpcode() != Instruction::IntToPtr)
    return nullptr;
  auto *AddressInt = dyn_cast<ConstantInt>(IntToPtr->getOperand(0));
  if (!AddressInt)
    return nullptr;

  APInt Absolute = AddressInt->getValue().zextOrTrunc(ExtraOffset.getBitWidth());
  Absolute += ExtraOffset;
  uintptr_t Target = static_cast<uintptr_t>(Absolute.getZExtValue());

  for (const auto &[GV, Info] : gvMap) {
    if (!GV->getValueType()->isPointerTy())
      continue;
    auto FieldIt = mayConstFieldMap.find(GV);
    if (FieldIt == mayConstFieldMap.end())
      continue;

    void *PointerSlot = resolveBase(GV, Info, reg);
    if (!PointerSlot)
      continue;
    void *Pointee = nullptr;
    std::memcpy(&Pointee, PointerSlot, sizeof(Pointee));
    uintptr_t ObjectBase = reinterpret_cast<uintptr_t>(Pointee);
    if (!ObjectBase || Target < ObjectBase)
      continue;

    uint64_t FieldOffset = Target - ObjectBase;
    if (!llvm::is_contained(FieldIt->second, FieldOffset))
      continue;
    return createConstantFromMemory(reinterpret_cast<const void *>(Target),
                                    LI->getType(), DL);
  }
  return nullptr;
}

static Constant *tryReplaceBoundPointer(LoadInst *LI, const Function &F,
                                        const uint8_t *Data, uint32_t Size,
                                        uint32_t ArgIndex,
                                        const DataLayout &DL) {
  if (!Data || !Size || ArgIndex >= F.arg_size() ||
      !functionBindsArgument(F, ArgIndex))
    return nullptr;
  const Argument *Root = findRootArgument(LI->getPointerOperand());
  if (!Root || Root->getArgNo() != ArgIndex)
    return nullptr;
  auto Offset = accumulateArgumentOffset(DL, LI->getPointerOperand(), Root);
  TypeSize AccessSize = DL.getTypeStoreSize(LI->getType());
  if (!Offset || AccessSize.isScalable() || *Offset > Size ||
      AccessSize.getFixedValue() > Size - *Offset)
    return nullptr;
  return createConstantFromMemory(Data + *Offset, LI->getType(), DL);
}

//===----------------------------------------------------------------------===//
// Load replacement helpers — one per access pattern
//===----------------------------------------------------------------------===//

/// Pattern 1: load directly from a GlobalVariable (scalar static variable).
static Constant *
tryReplaceDirectGV(LoadInst *LI, const GlobalVariable *GV,
                   const GVPeriodMap &gvMap, PeriodArrayRegistry &reg,
                   const DataLayout &DL) {
  auto it = gvMap.find(GV);
  if (it == gvMap.end())
    return nullptr;

  void *base = resolveBase(GV, it->second, reg);
  if (!base)
    return nullptr;

  return createConstantFromMemory(base, LI->getType(), DL);
}

/// Pattern 2: load via a GEP chain rooted at a GlobalVariable.
/// e.g. @g_cellCfg → GEP 0, idx → GEP 0, fieldIdx → load
static Constant *
tryReplaceDirectGEP(LoadInst *LI, const Value *PtrOp,
                    const GVPeriodMap &gvMap, PeriodArrayRegistry &reg,
                    const DataLayout &DL) {
  const GlobalVariable *GV = findRootGV(PtrOp);
  if (!GV)
    return nullptr;

  auto it = gvMap.find(GV);
  if (it == gvMap.end())
    return nullptr;

  auto byteOffset = accumulateFullOffset(DL, PtrOp);
  if (!byteOffset)
    return nullptr;

  void *base = resolveBase(GV, it->second, reg);
  if (!base)
    return nullptr;

  auto *fieldAddr = static_cast<const uint8_t *>(base) + *byteOffset;
  return createConstantFromMemory(fieldAddr, LI->getType(), DL);
}

/// Pattern 3: load via an indirect pointer — first load a pointer from a GV,
/// then GEP into the pointed-to data.
/// e.g. %ptr = load ptr, ptr @g_pCfg  → GEP %S, ptr %ptr, i32 0, i32 0
static Constant *
tryReplaceIndirect(LoadInst *LI, const Value *PtrOp,
                   const GVPeriodMap &gvMap, PeriodArrayRegistry &reg,
                   const DataLayout &DL) {
  // Walk the GEP chain from the load's pointer operand to find
  // the base LoadInst that reads the pointer value from a GV.
  const Value *V = PtrOp;
  SmallVector<const GEPOperator *, 4> FieldGEPs;
  while (V) {
    V = V->stripPointerCasts();
    if (auto *GEP = dyn_cast<GEPOperator>(V)) {
      FieldGEPs.push_back(GEP);
      V = GEP->getPointerOperand();
      continue;
    }
    break;
  }

  auto *BaseLoad = dyn_cast<LoadInst>(V);
  if (!BaseLoad)
    return nullptr;

  // Resolve the pointer-valued GV that BaseLoad reads from.
  const Value *LoadPtr = BaseLoad->getPointerOperand()->stripPointerCasts();
  const GlobalVariable *PtrGV = nullptr;
  uint64_t ptrArrayByteOff = 0;

  if (auto *DirectGV = dyn_cast<GlobalVariable>(LoadPtr)) {
    PtrGV = DirectGV;
  } else if (auto *PtrGEP = dyn_cast<GEPOperator>(LoadPtr)) {
    PtrGV = dyn_cast<GlobalVariable>(
        PtrGEP->getPointerOperand()->stripPointerCasts());
    if (PtrGV) {
      auto off = computeGEPOffset(PtrGEP, DL);
      if (!off)
        return nullptr;
      ptrArrayByteOff = *off;
    }
  }
  if (!PtrGV)
    return nullptr;

  auto it = gvMap.find(PtrGV);
  if (it == gvMap.end())
    return nullptr;

  void *gvBase = resolveBase(PtrGV, it->second, reg);
  if (!gvBase)
    return nullptr;

  // Read the stored pointer: *(void**)(gvBase + ptrArrayByteOff)
  uintptr_t ptrSlot = reinterpret_cast<uintptr_t>(gvBase) + ptrArrayByteOff;
  void *dataBase = nullptr;
  std::memcpy(&dataBase, reinterpret_cast<void *>(ptrSlot), sizeof(void *));
  if (!dataBase)
    return nullptr;

  // Compute field offset from the GEPs past the pointer dereference.
  uint64_t fieldOff = 0;
  for (auto It = FieldGEPs.rbegin(); It != FieldGEPs.rend(); ++It) {
    auto off = computeGEPOffset(*It, DL);
    if (!off)
      return nullptr;
    fieldOff += *off;
  }

  auto *fieldAddr = static_cast<const uint8_t *>(dataBase) + fieldOff;
  return createConstantFromMemory(fieldAddr, LI->getType(), DL);
}

//===----------------------------------------------------------------------===//
// Public interface
//===----------------------------------------------------------------------===//

#ifdef EJIT_DIAG_ENABLE
/// Classify why a may_const load was NOT replaced by any pattern, for
/// diagnostics. One EJIT_DIAG line per failed load. Reasons:
///   no-root-gv        — pointer not rooted at a GlobalVariable (opaque/indirect)
///   gv-not-in-map     — root GV has no ejit.metadata (not a period var)
///   base-unresolved   — GV is a period var but not registered at runtime
///   non-const-offset  — GEP index not folded to a constant
///   unsupported-type  — createConstantFromMemory cannot build the load type
static void logReplaceFailure(LoadInst *LI, const GVPeriodMap &gvMap,
                              PeriodArrayRegistry &reg,
                              const DataLayout &DL) {
  Value *Ptr = LI->getPointerOperand();
  const GlobalVariable *GV = findRootGV(Ptr);
  if (!GV) {
    EJIT_DIAG_VERBOSE("  may_const load NOT replaced: no-root-gv");
    return;
  }
  auto it = gvMap.find(GV);
  if (it == gvMap.end()) {
    EJIT_DIAG_VERBOSE("  may_const load NOT replaced: gv-not-in-map gv=%s",
                      GV->getName().str().c_str());
    return;
  }
  if (!resolveBase(GV, it->second, reg)) {
    EJIT_DIAG_VERBOSE("  may_const load NOT replaced: base-unresolved gv=%s",
                      GV->getName().str().c_str());
    return;
  }
  if (!accumulateFullOffset(DL, Ptr)) {
    EJIT_DIAG_VERBOSE("  may_const load NOT replaced: non-const-offset gv=%s",
                      GV->getName().str().c_str());
    return;
  }
  EJIT_DIAG_VERBOSE("  may_const load NOT replaced: unsupported-type gv=%s",
                    GV->getName().str().c_str());
}
#endif

PreservedAnalyses
EJitStructFieldPass::run(Function &F, FunctionAnalysisManager &AM) {
  Module *M = F.getParent();
  if (!M) {
    EJIT_DIAG_VERBOSE("struct-field run SKIP func=%s: no parent module",
                      F.getName().str().c_str());
    return PreservedAnalyses::all();
  }

  assert(mapsBuilt_ && "EJitStructFieldPass::initFromModule() must be "
                       "called before run()");

  const DataLayout &DL = M->getDataLayout();

  // 1. Use cached module-level metadata maps (built once per module by
  //    initFromModule(), reused across all function runs).

  // 2. Scan all loads and collect replacements.
  struct Replacement { LoadInst *LI; Constant *ConstVal; };
  SmallVector<Replacement, 16> replacements;
#ifdef EJIT_DIAG_ENABLE
  size_t totalLoads = 0, mayConstLoads = 0;
  // Only the ejit_entry function (or any function that actually has may_const
  // activity / replacements) is worth a per-function diagnostic block. Silent
  // for the common case of an auxiliary callee with no may_const loads, which
  // is the dominant source of struct-field log noise on a specialization
  // module that contains many non-entry callees.
  bool isEjitEntry =
      hasMDStringEntry(F.getMetadata(MD_EJIT_METADATA), TAG_EJIT_ENTRY);
#endif

  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      auto *LI = dyn_cast<LoadInst>(&I);
      if (!LI)
        continue;
#ifdef EJIT_DIAG_ENABLE
      ++totalLoads;
#endif

      // A pointer-form period global is itself the root needed to reach nested
      // may_const fields. It has no may_const marker of its own, but replacing
      // it here exposes a concrete address to IPSCCP and later StructFieldPass
      // rounds, including when the field access lives in a non-inlined helper.
      if (Constant *C = tryReplacePeriodPointerBase(
              LI, gvPeriodMap_, registry_, DL)) {
        replacements.push_back({LI, C});
        continue;
      }

      bool BoundMayConst = isBoundMayConstLoad(LI, F, boundArgIndex_, DL);
      if (!BoundMayConst && !isMayConstLoad(LI, mayConstFieldMap_, DL))
        continue;
#ifdef EJIT_DIAG_ENABLE
      ++mayConstLoads;
#endif

      Value *PtrOp = LI->getPointerOperand();

      // Try each access pattern in order.
      Constant *C = tryReplacePeriodAbsoluteAddress(
          LI, gvPeriodMap_, mayConstFieldMap_, registry_, DL);

      // Pattern 0: an ejit_bound_ptr parameter. Only the marked load is read
      // from the owned snapshot; the pointer argument and all dynamic fields
      // remain live inputs to the specialization.
      if (!C)
        C = tryReplaceBoundPointer(LI, F, boundData_, boundSize_,
                                   boundArgIndex_, DL);

      // Pattern 1: direct GlobalVariable load (scalar static variable).
      if (!C) {
        if (auto *GV = dyn_cast<GlobalVariable>(PtrOp->stripPointerCasts()))
          C = tryReplaceDirectGV(LI, GV, gvPeriodMap_, registry_, DL);
      }

      // Pattern 2: GEP-based access (array or struct field).
      if (!C)
        C = tryReplaceDirectGEP(LI, PtrOp, gvPeriodMap_, registry_, DL);

      // Pattern 3: indirect pointer access (pointer-type period variable).
      if (!C)
        C = tryReplaceIndirect(LI, PtrOp, gvPeriodMap_, registry_, DL);

      if (C)
        replacements.push_back({LI, C});
#ifdef EJIT_DIAG_ENABLE
      else
        logReplaceFailure(LI, gvPeriodMap_, registry_, DL);
#endif
    }
  }

  // 3. Apply replacements.
  bool changed = false;
  for (auto &R : replacements) {
    R.LI->replaceAllUsesWith(R.ConstVal);
    R.LI->eraseFromParent();
    changed = true;
  }

#ifdef EJIT_DIAG_ENABLE
  // Gate the per-function block: emit for the ejit_entry function (always
  // interesting — it is the specialization target) or for any function where a
  // may_const load was seen or a replacement actually happened. Auxiliary
  // callees with no may_const activity stay silent, eliminating the bulk of
  // the struct-field log volume while preserving locatability for the
  // functions that matter. Raise the log level to VERBOSE to see this detail.
  if (isEjitEntry || mayConstLoads > 0 || !replacements.empty()) {
    EJIT_DIAG_VERBOSE("struct-field run func=%s entry=%d replaced=%zu",
                      F.getName().str().c_str(), isEjitEntry ? 1 : 0,
                      replacements.size());
    EJIT_DIAG_VERBOSE("  loads total=%zu may_const=%zu replaced=%zu",
                      totalLoads, mayConstLoads, replacements.size());
  }
#endif
  LLVM_DEBUG(if (changed) dbgs() << "ejit-struct-field: replaced "
                                 << replacements.size() << " load(s) in "
                                 << F.getName() << "\n");
  return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
