//===-- EJitSharedSpecializationTest.cpp - PR230 shared mode tests --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// PR230 shared-version code reuse, stage 1: preserved-dimension / load-only
// replacement (EJIT_VERSION_CODE_REUSE_SPEC.md §3, §4, §8.3).
//
// The contract these tests pin:
//   * cell/TRP are still the business call arguments — they are NOT replaced by
//     the compile request's instance, so dynamic loads, stores, pointer
//     arithmetic and call arguments keep their runtime meaning;
//   * the requested instances are consumed only as a read-only evaluation
//     environment for the *address* of an authorized may_const load, and only
//     the load result becomes a constant;
//   * every replace round uses that same environment, including the final round
//     that used to run with an empty context;
//   * the unmarked pointer-form period base load is not written back into the
//     IR as a real address;
//   * a helper inherits the environment only across unanimous, enumerable
//     direct calls.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitCodeReuse.h"
#include "llvm/ExecutionEngine/EJIT/EJitCommon.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptimizer.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#include "llvm/ExecutionEngine/EJIT/EJitStructFieldPass.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"

#include "gtest/gtest.h"

#include <algorithm>
#include <cstring>
#include <string>

using namespace llvm;
using namespace llvm::ejit;

namespace llvm {
namespace ejit {
// Mirrors the accessor in EJitRuntimeTest.cpp: the individual pipeline steps
// are private in production and re-exported here through the friend
// declaration.
struct EJitOptimizerTestAccess : EJitOptimizer {
  using EJitOptimizer::EJitOptimizer;
  using EJitOptimizer::preReplacePeriodIndices;
  using EJitOptimizer::runInstCombine;
  using EJitOptimizer::runInterproceduralPropagation;
  using EJitOptimizer::runOptimizationPipeline;
  using EJitOptimizer::runStructFieldPass;
};
} // namespace ejit
} // namespace llvm

namespace {

/// One period element: gain is may_const, live is ordinary dynamic data.
/// 8 bytes per element, so the array stride and the field offsets are obvious.
struct Elem {
  uint32_t Gain;
  uint32_t Live;
};

/// The array form: cfg[cell][trp]. Element (c,t) lives at (c*4 + t) * 8.
constexpr unsigned kCells = 4;
constexpr unsigned kTrps = 4;
constexpr size_t kArrayBytes = kCells * kTrps * sizeof(Elem);

/// Distinctive, non-structural gain values: a folded constant identifies
/// exactly which instance was evaluated, and none of them collides with a
/// constant the pipeline may introduce on its own (0, 1, sizes, ...).
constexpr uint32_t gainOf(unsigned C, unsigned T) {
  return 1000 * C + 100 * T + 7;
}

const char *kArrayIR = R"(
  target datalayout = "e-p:64:64-i64:64-n8:16:32:64-S128"
  %S = type { i32, i32 }
  @g_cfg = external global [4 x [4 x %S]], !ejit.metadata !4
  define i32 @f(i32 %cell, i32 %trp, i32 %x) !ejit.metadata !0 {
  entry:
    %p = getelementptr [4 x [4 x %S]], ptr @g_cfg, i32 0, i32 %cell, i32 %trp, i32 0
    %gain = load i32, ptr %p, !ejit.may_const !9
    %lp = getelementptr [4 x [4 x %S]], ptr @g_cfg, i32 0, i32 %cell, i32 %trp, i32 1
    %live = load i32, ptr %lp
    store i32 %x, ptr %lp
    %m = mul i32 %gain, %x
    %s = add i32 %m, %live
    %r = add i32 %s, %cell
    %rt = add i32 %r, %trp
    ret i32 %rt
  }
  !0 = distinct !{!1, !2, !3}
  !1 = !{!"ejit_entry"}
  !2 = !{!"ejit_period_arr_ind", !"cell", i32 0}
  !3 = !{!"ejit_period_arr_ind", !"trp", i32 1}
  !4 = distinct !{!5}
  !5 = !{!"ejit_period_arr", !"cell", i32 128}
  !9 = !{!"ejit"}
)";

/// The pointer form: @g_pcfg holds a pointer to the element array. The load of
/// that pointer is not may_const; only the fields inside the pointee are. The
/// field index is constant so the fold does not depend on an assumption — this
/// test isolates the pointer-base write-back rule.
const char *kPointerIR = R"(
  target datalayout = "e-p:64:64-i64:64-n8:16:32:64-S128"
  %S = type { i32, i32 }
  @g_pcfg = external global ptr, !ejit.metadata !4
  define i32 @fp(i32 %cell, i32 %trp, i32 %x) !ejit.metadata !0 {
  entry:
    %base = load ptr, ptr @g_pcfg
    %p = getelementptr [4 x [4 x %S]], ptr %base, i32 0, i32 2, i32 1, i32 0
    %gain = load i32, ptr %p, !ejit.may_const !9
    %m = mul i32 %gain, %x
    %r = add i32 %m, %cell
    %rt = add i32 %r, %trp
    ret i32 %rt
  }
  !0 = distinct !{!1, !2, !3}
  !1 = !{!"ejit_entry"}
  !2 = !{!"ejit_period_arr_ind", !"cell", i32 0}
  !3 = !{!"ejit_period_arr_ind", !"trp", i32 1}
  !4 = distinct !{!5}
  !5 = !{!"ejit_period", !"cell"}
  !9 = !{!"ejit"}
)";

/// Helper module: @helper is called twice from the entry with the entry's own
/// dimensions, so the environment reaches its formals.
const char *kHelperIR = R"(
  target datalayout = "e-p:64:64-i64:64-n8:16:32:64-S128"
  %S = type { i32, i32 }
  @g_cfg = external global [4 x [4 x %S]], !ejit.metadata !4
  define i32 @root(i32 %cell, i32 %trp, i32 %x) !ejit.metadata !0 {
  entry:
    %a = call i32 @helper(i32 %cell, i32 %trp, i32 %x)
    %b = call i32 @helper(i32 %cell, i32 %trp, i32 %a)
    %r = add i32 %a, %b
    ret i32 %r
  }
  define i32 @helper(i32 %c, i32 %t, i32 %x) noinline {
  entry:
    %p = getelementptr [4 x [4 x %S]], ptr @g_cfg, i32 0, i32 %c, i32 %t, i32 0
    %gain = load i32, ptr %p, !ejit.may_const !9
    %m = mul i32 %gain, %x
    ret i32 %m
  }
  !0 = distinct !{!1, !2, !3}
  !1 = !{!"ejit_entry"}
  !2 = !{!"ejit_period_arr_ind", !"cell", i32 0}
  !3 = !{!"ejit_period_arr_ind", !"trp", i32 1}
  !4 = distinct !{!5}
  !5 = !{!"ejit_period_arr", !"cell", i32 128}
  !9 = !{!"ejit"}
)";

const char *kHelperDisagreeIR = R"(
  target datalayout = "e-p:64:64-i64:64-n8:16:32:64-S128"
  %S = type { i32, i32 }
  @g_cfg = external global [4 x [4 x %S]], !ejit.metadata !4
  define i32 @root(i32 %cell, i32 %trp, i32 %x) !ejit.metadata !0 {
  entry:
    %a = call i32 @helper(i32 %cell, i32 %trp, i32 %x)
    %b = call i32 @helper(i32 3, i32 0, i32 %a)
    %r = add i32 %a, %b
    ret i32 %r
  }
  define i32 @helper(i32 %c, i32 %t, i32 %x) noinline {
  entry:
    %p = getelementptr [4 x [4 x %S]], ptr @g_cfg, i32 0, i32 %c, i32 %t, i32 0
    %gain = load i32, ptr %p, !ejit.may_const !9
    %m = mul i32 %gain, %x
    ret i32 %m
  }
  !0 = distinct !{!1, !2, !3}
  !1 = !{!"ejit_entry"}
  !2 = !{!"ejit_period_arr_ind", !"cell", i32 0}
  !3 = !{!"ejit_period_arr_ind", !"trp", i32 1}
  !4 = distinct !{!5}
  !5 = !{!"ejit_period_arr", !"cell", i32 128}
  !9 = !{!"ejit"}
)";

const char *kHelperEscapedIR = R"(
  target datalayout = "e-p:64:64-i64:64-n8:16:32:64-S128"
  %S = type { i32, i32 }
  @g_cfg = external global [4 x [4 x %S]], !ejit.metadata !4
  @table = global [1 x ptr] [ptr @helper]
  define i32 @root(i32 %cell, i32 %trp, i32 %x) !ejit.metadata !0 {
  entry:
    %a = call i32 @helper(i32 %cell, i32 %trp, i32 %x)
    ret i32 %a
  }
  define i32 @helper(i32 %c, i32 %t, i32 %x) noinline {
  entry:
    %p = getelementptr [4 x [4 x %S]], ptr @g_cfg, i32 0, i32 %c, i32 %t, i32 0
    %gain = load i32, ptr %p, !ejit.may_const !9
    %m = mul i32 %gain, %x
    ret i32 %m
  }
  !0 = distinct !{!1, !2, !3}
  !1 = !{!"ejit_entry"}
  !2 = !{!"ejit_period_arr_ind", !"cell", i32 0}
  !3 = !{!"ejit_period_arr_ind", !"trp", i32 1}
  !4 = distinct !{!5}
  !5 = !{!"ejit_period_arr", !"cell", i32 128}
  !9 = !{!"ejit"}
)";

class SharedSpecializationTest : public testing::Test {
protected:
  LLVMContext Ctx;
  std::unique_ptr<Module> M;
  std::vector<Elem> Storage;

  /// Element (c,t) has gain = gainOf(c,t) and live = 5000 + 100*c + t.
  void buildArrayStorage() {
    Storage.assign(kCells * kTrps, Elem{0, 0});
    for (unsigned c = 0; c < kCells; ++c)
      for (unsigned t = 0; t < kTrps; ++t) {
        Storage[c * kTrps + t].Gain = gainOf(c, t);
        Storage[c * kTrps + t].Live = 5000 + 100 * c + t;
      }
  }

  /// The gain of the flat element at \p Index (the layout a one-dimensional
  /// period array reads).
  uint32_t gainAtIndex(unsigned Index) const { return Storage[Index].Gain; }

  std::unique_ptr<Module> parse(StringRef IR) {
    SMDiagnostic Err;
    auto Mod = parseAssemblyString(IR, Err, Ctx);
    if (!Mod)
      Err.print("SharedSpecializationTest", errs());
    return Mod;
  }

  /// Logical identity of one instance of the entry under test. Mirrors what
  /// the compile driver would build; used to show that two versions sharing an
  /// emission are still two distinct logical versions.
  static LogicalKey makeLogical(StringRef Fn, unsigned Cell, unsigned Trp) {
    LogicalKey Key;
    Key.entry = Fn.str();
    Key.sourceId = "src-rev-1";
    Key.dimensions.push_back({"cell", Cell});
    Key.dimensions.push_back({"trp", Trp});
    Key.lifecycleVersions.push_back(0);
    Key.lifecycleVersions.push_back(0);
    Key.runtimeGeneration = 1;
    return Key;
  }

  static SpecializationContext makeCtx(StringRef Fn, unsigned Cell,
                                       unsigned Trp) {
    SpecializationContext CtxSpec;
    CtxSpec.fnName = Fn.str();
    CtxSpec.dimensions.push_back({"cell", static_cast<uint8_t>(Cell)});
    CtxSpec.dimensions.push_back({"trp", static_cast<uint8_t>(Trp)});
    CtxSpec.sharedSpecialization = true;
    return CtxSpec;
  }

  /// Run the replace rounds exactly as runPipeline orders them, with the shared
  /// evaluation environment supplied to every round.
  void runAllReplaceRounds(Module &Mod, const SpecializationContext &CtxSpec,
                           EJitOptimizerTestAccess &Opt) {
    Opt.preReplacePeriodIndices(Mod, CtxSpec);
    Opt.runInstCombine(Mod);
    Opt.runStructFieldPass(Mod, CtxSpec);
    Opt.runInterproceduralPropagation(Mod);
    Opt.runInstCombine(Mod);
    Opt.runStructFieldPass(Mod, CtxSpec);
  }

  static unsigned countLoads(Function &F) {
    unsigned N = 0;
    for (Instruction &I : instructions(F))
      if (isa<LoadInst>(I))
        ++N;
    return N;
  }

  static unsigned countStores(Function &F) {
    unsigned N = 0;
    for (Instruction &I : instructions(F))
      if (isa<StoreInst>(I))
        ++N;
    return N;
  }

  static bool countPointerLoads(Function &F) {
    for (Instruction &I : instructions(F))
      if (auto *LI = dyn_cast<LoadInst>(&I))
        if (LI->getType()->isPointerTy())
          return true;
    return false;
  }

  /// The folded value appears as an operand of some instruction. A constant is
  /// an operand, never an instruction, so scanning instructions alone would
  /// always miss it.
  static bool hasConstantOperand(Function &F, uint64_t V) {
    for (Instruction &I : instructions(F))
      for (Value *Op : I.operands())
        if (auto *CI = dyn_cast<ConstantInt>(Op))
          if (CI->getZExtValue() == V)
            return true;
    return false;
  }

  static bool argumentIsUsed(Function &F, unsigned ArgIdx) {
    return !F.getArg(ArgIdx)->use_empty();
  }
};

//===----------------------------------------------------------------------===//
// §3.1/§3.2 — only the authorized load is folded; the arguments stay real.
//===----------------------------------------------------------------------===//

TEST_F(SharedSpecializationTest, KeepsRuntimeDimensionArgumentsAndWrites) {
  buildArrayStorage();
  M = parse(kArrayIR);
  ASSERT_TRUE(M);

  PeriodArrayRegistry Registry;
  Registry.registerArray("cell", "g_cfg", Storage.data(), kArrayBytes);

  EJitOptimizerTestAccess Opt(Registry);
  SpecializationContext CtxSpec = makeCtx("f", 2, 1);
  runAllReplaceRounds(*M, CtxSpec, Opt);

  Function *F = M->getFunction("f");
  ASSERT_NE(F, nullptr);
  ASSERT_EQ(F->arg_size(), 3u);

  // The requested instance's may_const value is now a constant...
  EXPECT_TRUE(hasConstantOperand(*F, gainOf(2, 1)))
      << "the may_const gain of the requested instance must be folded";

  // ...while both dimensions remain live inputs: they still index the dynamic
  // load/store address and still reach the return value.
  EXPECT_TRUE(argumentIsUsed(*F, 0)) << "%cell must remain a live argument";
  EXPECT_TRUE(argumentIsUsed(*F, 1)) << "%trp must remain a live argument";
  EXPECT_EQ(countStores(*F), 1u)
      << "the dynamic store must not be deleted or redirected";
  EXPECT_GE(countLoads(*F), 1u)
      << "the non-may_const dynamic load must survive";

  for (Argument &A : F->args())
    EXPECT_FALSE(A.hasAttribute(Attribute::Range))
        << "shared mode must not narrow the argument";
}

TEST_F(SharedSpecializationTest, FoldsTheRequestedInstanceNotAnotherOne) {
  buildArrayStorage();

  for (unsigned Cell = 0; Cell < 2; ++Cell) {
    LLVMContext LocalCtx;
    SMDiagnostic Err;
    auto Mod = parseAssemblyString(kArrayIR, Err, LocalCtx);
    ASSERT_TRUE(Mod) << Err.getMessage().str();

    PeriodArrayRegistry Registry;
    Registry.registerArray("cell", "g_cfg", Storage.data(), kArrayBytes);

    EJitOptimizerTestAccess Opt(Registry);
    SpecializationContext CtxSpec = makeCtx("f", Cell, 1);
    runAllReplaceRounds(*Mod, CtxSpec, Opt);

    Function *F = Mod->getFunction("f");
    ASSERT_NE(F, nullptr);

    EXPECT_TRUE(hasConstantOperand(*F, gainOf(Cell, 1)))
        << "cell " << Cell << " must fold its own gain";
    const unsigned Other = Cell == 0 ? 1 : 0;
    EXPECT_FALSE(hasConstantOperand(*F, gainOf(Other, 1)))
        << "cell " << Cell << " must not fold cell " << Other << "'s gain";
  }
}

//===----------------------------------------------------------------------===//
// §4 — the final replace round must receive the same environment.
//===----------------------------------------------------------------------===//

/// A helper whose address escapes cannot be assumed in the rounds that run
/// before inlining. Once the inliner folds it into the entry, the loads use the
/// entry's own arguments and the final round must fold them — which only works
/// if that round rebuilds its mapping against the rewritten IR and receives the
/// same evaluation context as the earlier rounds.
///
/// The inliner does not preserve the per-load !ejit.may_const marker, so the
/// global's field-offset map (what the AOT pass emits alongside the marker) is
/// what keeps the inlined load recognizable as may_const here. The period array
/// is therefore one-dimensional, matching the shape the AOT annotator can
/// attribute: it admits exactly one dynamic element index.
TEST_F(SharedSpecializationTest, FinalRoundFoldsLoadsExposedByInlining) {
  const char *IR = R"(
    target datalayout = "e-p:64:64-i64:64-n8:16:32:64-S128"
    %S = type { i32, i32 }
    @g_cfg = external global [4 x %S], !ejit.metadata !4
    @table = internal global [1 x ptr] [ptr @helper]
    define i32 @root(i32 %cell, i32 %trp, i32 %x) !ejit.metadata !0 {
    entry:
      %a = call i32 @helper(i32 %cell, i32 %trp, i32 %x)
      %b = call i32 @helper(i32 %cell, i32 %trp, i32 %a)
      %lp = getelementptr [4 x %S], ptr @g_cfg, i32 0, i32 %cell, i32 1
      %live = load i32, ptr %lp
      %r = add i32 %a, %b
      %r2 = add i32 %r, %live
      %r3 = add i32 %r2, %trp
      ret i32 %r3
    }
    define internal i32 @helper(i32 %c, i32 %t, i32 %x) alwaysinline {
    entry:
      %p = getelementptr [4 x %S], ptr @g_cfg, i32 0, i32 %c, i32 0
      %gain = load i32, ptr %p, !ejit.may_const !9
      %m = mul i32 %gain, %x
      %u = add i32 %m, %t
      ret i32 %u
    }
    !0 = distinct !{!1, !2, !3}
    !1 = !{!"ejit_entry"}
    !2 = !{!"ejit_period_arr_ind", !"cell", i32 0}
    !3 = !{!"ejit_period_arr_ind", !"trp", i32 1}
    !4 = distinct !{!5, !6}
    !5 = !{!"ejit_period_arr", !"cell", i32 32}
    !6 = !{!"ejit_may_const_field", i32 0}
    !9 = !{!"ejit"}
  )";
  buildArrayStorage();
  M = parse(IR);
  ASSERT_TRUE(M);

  PeriodArrayRegistry Registry;
  Registry.registerArray("cell", "g_cfg", Storage.data(), kArrayBytes);

  EJitOptimizerTestAccess Opt(Registry);
  SpecializationContext CtxSpec = makeCtx("root", 2, 3);
  // Tier-2 order: the two early replace rounds, light-opt, then the module
  // inliner, then runOptimizationPipeline's final replace round.
  CtxSpec.tier = CompileTier::PGOUse;
  Opt.runPipeline(*M, CtxSpec);

  Function *Root = M->getFunction("root");
  ASSERT_NE(Root, nullptr);
  // The one-dimensional array's element index is the cell dimension, so the
  // folded value is the gain of Storage[cell].
  EXPECT_TRUE(hasConstantOperand(*Root, gainAtIndex(2)))
      << "the post-inlining round must fold with the shared environment";
  EXPECT_TRUE(argumentIsUsed(*Root, 0)) << "%cell must stay a live argument";
  EXPECT_TRUE(argumentIsUsed(*Root, 1)) << "%trp must stay a live argument";
}

TEST_F(SharedSpecializationTest, FinalReplaceRoundUsesTheSharedContext) {
  buildArrayStorage();

  // With the shared context the last replace round folds the authorized load
  // ...
  {
    M = parse(kArrayIR);
    ASSERT_TRUE(M);
    PeriodArrayRegistry Registry;
    Registry.registerArray("cell", "g_cfg", Storage.data(), kArrayBytes);
    EJitOptimizerTestAccess Opt(Registry);
    SpecializationContext CtxSpec = makeCtx("f", 3, 2);
    Opt.runOptimizationPipeline(*M, OptimizationLevel::L2,
                                CompileTier::Baseline, &CtxSpec);
    Function *F = M->getFunction("f");
    ASSERT_NE(F, nullptr);
    EXPECT_TRUE(hasConstantOperand(*F, gainOf(3, 2)));
    EXPECT_EQ(countStores(*F), 1u);
    EXPECT_TRUE(argumentIsUsed(*F, 0));
    EXPECT_TRUE(argumentIsUsed(*F, 1));
  }

  // ... while the pre-existing context-less round leaves it alone, because in
  // non-shared mode nothing rewrote the arguments either. This is the
  // differential that proves the context is what enables the fold.
  {
    LLVMContext LocalCtx;
    SMDiagnostic Err;
    auto Mod = parseAssemblyString(kArrayIR, Err, LocalCtx);
    ASSERT_TRUE(Mod) << Err.getMessage().str();
    PeriodArrayRegistry Registry;
    Registry.registerArray("cell", "g_cfg", Storage.data(), kArrayBytes);
    EJitOptimizerTestAccess Opt(Registry);
    Opt.runOptimizationPipeline(*Mod, OptimizationLevel::L2,
                                CompileTier::Baseline, nullptr);
    Function *F = Mod->getFunction("f");
    ASSERT_NE(F, nullptr);
    EXPECT_FALSE(hasConstantOperand(*F, gainOf(3, 2)))
        << "without the shared context the address is not evaluable";
  }
}

//===----------------------------------------------------------------------===//
// §3.2 — the unmarked pointer-form base load is not written back.
//===----------------------------------------------------------------------===//

TEST_F(SharedSpecializationTest, PointerFormBaseLoadIsNotMaterialized) {
  buildArrayStorage();
  void *Base = Storage.data();

  for (bool Shared : {true, false}) {
    LLVMContext LocalCtx;
    SMDiagnostic Err;
    auto Mod = parseAssemblyString(kPointerIR, Err, LocalCtx);
    ASSERT_TRUE(Mod) << Err.getMessage().str();

    PeriodArrayRegistry Registry;
    // A pointer-form period global is registered by the address of its slot;
    // the pass dereferences it while evaluating the field address.
    Registry.registerStaticVar("g_pcfg", &Base);

    EJitOptimizerTestAccess Opt(Registry);
    SpecializationContext CtxSpec = makeCtx("fp", 1, 2);
    CtxSpec.sharedSpecialization = Shared;
    Opt.runStructFieldPass(*Mod, CtxSpec);

    Function *F = Mod->getFunction("fp");
    ASSERT_NE(F, nullptr);

    EXPECT_TRUE(hasConstantOperand(*F, gainOf(2, 1)))
        << "the may_const field load resolves through the registered slot";

    if (Shared)
      EXPECT_TRUE(countPointerLoads(*F))
          << "shared mode must not publish the real base address into the IR";
    else
      EXPECT_FALSE(countPointerLoads(*F))
          << "non-shared behavior keeps materializing the base pointer";
  }
}

//===----------------------------------------------------------------------===//
// §3.4 — helper propagation over unanimous, enumerable direct calls.
//===----------------------------------------------------------------------===//

TEST_F(SharedSpecializationTest, HelperInheritsEnvironmentOnUnanimousCalls) {
  buildArrayStorage();
  M = parse(kHelperIR);
  ASSERT_TRUE(M);

  PeriodArrayRegistry Registry;
  Registry.registerArray("cell", "g_cfg", Storage.data(), kArrayBytes);

  EJitOptimizerTestAccess Opt(Registry);
  SpecializationContext CtxSpec = makeCtx("root", 2, 3);
  runAllReplaceRounds(*M, CtxSpec, Opt);

  Function *Helper = M->getFunction("helper");
  ASSERT_NE(Helper, nullptr);
  EXPECT_TRUE(hasConstantOperand(*Helper, gainOf(2, 3)))
      << "both call sites pass the entry's own dimensions";
}

TEST_F(SharedSpecializationTest, HelperIsNotSpecializedWhenCallsDisagree) {
  buildArrayStorage();
  M = parse(kHelperDisagreeIR);
  ASSERT_TRUE(M);

  PeriodArrayRegistry Registry;
  Registry.registerArray("cell", "g_cfg", Storage.data(), kArrayBytes);

  EJitOptimizerTestAccess Opt(Registry);
  SpecializationContext CtxSpec = makeCtx("root", 2, 1);
  runAllReplaceRounds(*M, CtxSpec, Opt);

  Function *Helper = M->getFunction("helper");
  ASSERT_NE(Helper, nullptr);
  EXPECT_FALSE(hasConstantOperand(*Helper, gainOf(2, 1)))
      << "a formal whose call sites disagree must not be assumed";
  EXPECT_FALSE(hasConstantOperand(*Helper, gainOf(3, 0)))
      << "the other call site's instance must not be assumed either";
}

TEST_F(SharedSpecializationTest, HelperWithEscapedAddressIsNotSpecialized) {
  buildArrayStorage();
  M = parse(kHelperEscapedIR);
  ASSERT_TRUE(M);

  PeriodArrayRegistry Registry;
  Registry.registerArray("cell", "g_cfg", Storage.data(), kArrayBytes);

  EJitOptimizerTestAccess Opt(Registry);
  SpecializationContext CtxSpec = makeCtx("root", 2, 1);
  runAllReplaceRounds(*M, CtxSpec, Opt);

  Function *Helper = M->getFunction("helper");
  ASSERT_NE(Helper, nullptr);
  EXPECT_FALSE(hasConstantOperand(*Helper, gainOf(2, 1)))
      << "an address-taken helper has call sites this walk cannot see";
}

//===----------------------------------------------------------------------===//
// §8.3 — V1 support matrix.
//===----------------------------------------------------------------------===//

TEST(SharedSpecializationSupportMatrix, RejectsUnsupportedCombinations) {
  Config C;
  // OFF is always fine, whatever the other fields say.
  C.enableSharedSpecialization = false;
  C.compileMode = CompileMode::Sync;
  C.enablePgo = false;
  EXPECT_TRUE(checkSharedSpecializationSupport(C).supported);

  C.enableSharedSpecialization = true;
  C.compileMode = CompileMode::Async;
  C.enablePgo = true;
  C.enableProfileAudit = false;
  EXPECT_TRUE(checkSharedSpecializationSupport(C).supported);

  Config Sync = C;
  Sync.compileMode = CompileMode::Sync;
  auto SyncResult = checkSharedSpecializationSupport(Sync);
  EXPECT_FALSE(SyncResult.supported);
  EXPECT_NE(std::string(SyncResult.reason).find("Async"), std::string::npos);

  Config NoPgo = C;
  NoPgo.enablePgo = false;
  NoPgo.enableProfileAudit = false;
  auto NoPgoResult = checkSharedSpecializationSupport(NoPgo);
  EXPECT_FALSE(NoPgoResult.supported);
  EXPECT_NE(std::string(NoPgoResult.reason).find("PGO"), std::string::npos);

  Config AuditOnly = C;
  AuditOnly.enablePgo = false;
  AuditOnly.enableProfileAudit = true;
  auto AuditResult = checkSharedSpecializationSupport(AuditOnly);
  EXPECT_FALSE(AuditResult.supported);
  EXPECT_NE(std::string(AuditResult.reason).find("audit"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// §3.1 — the non-shared pipeline is unchanged for the same input.
//===----------------------------------------------------------------------===//

TEST_F(SharedSpecializationTest, NonSharedModeStillRewritesArguments) {
  buildArrayStorage();
  M = parse(kArrayIR);
  ASSERT_TRUE(M);

  PeriodArrayRegistry Registry;
  Registry.registerArray("cell", "g_cfg", Storage.data(), kArrayBytes);

  EJitOptimizerTestAccess Opt(Registry);
  SpecializationContext CtxSpec = makeCtx("f", 2, 1);
  CtxSpec.sharedSpecialization = false;
  Opt.preReplacePeriodIndices(*M, CtxSpec);

  Function *F = M->getFunction("f");
  ASSERT_NE(F, nullptr);
  EXPECT_FALSE(argumentIsUsed(*F, 0))
      << "the pre-existing mode still specializes the whole argument";
}

//===----------------------------------------------------------------------===//
// Stage 1 + stage 2 composition: what the shared pipeline emits is what the
// reuse table compares. Two instances whose folded configuration agrees must
// canonicalize to the same emission and share one physical record; a changed
// may_const value must produce a different emission and compile independently.
//===----------------------------------------------------------------------===//

TEST_F(SharedSpecializationTest, IdenticalEmissionsShareOneRecord) {
  const char *IR = R"(
    target datalayout = "e-p:64:64-i64:64-n8:16:32:64-S128"
    %S = type { i32, i32 }
    @g_cfg = external global [4 x %S], !ejit.metadata !4
    define i32 @f(i32 %cell, i32 %trp, i32 %x) !ejit.metadata !0 {
    entry:
      %p = getelementptr [4 x %S], ptr @g_cfg, i32 0, i32 %cell, i32 0
      %gain = load i32, ptr %p, !ejit.may_const !9
      %lp = getelementptr [4 x %S], ptr @g_cfg, i32 0, i32 %cell, i32 1
      %live = load i32, ptr %lp
      %m = mul i32 %gain, %x
      %s = add i32 %m, %live
      %r = add i32 %s, %trp
      ret i32 %r
    }
    !0 = distinct !{!1, !2, !3}
    !1 = !{!"ejit_entry"}
    !2 = !{!"ejit_period_arr_ind", !"cell", i32 0}
    !3 = !{!"ejit_period_arr_ind", !"trp", i32 1}
    !4 = distinct !{!5, !6}
    !5 = !{!"ejit_period_arr", !"cell", i32 32}
    !6 = !{!"ejit_may_const_field", i32 0}
    !9 = !{!"ejit"}
  )";
  buildArrayStorage();
  // Instances 1 and 2 carry the same may_const gain, so their shared-mode
  // emissions must be identical; instance 3 differs.
  Storage[1].Gain = 4242;
  Storage[2].Gain = 4242;
  Storage[3].Gain = 99;

  PeriodArrayRegistry Registry;
  Registry.registerArray("cell", "g_cfg", Storage.data(), kArrayBytes);

  struct Emission {
    CanonicalModule Canon;
    LogicalKey Logical;
  };
  auto compileFor = [&](unsigned Cell) {
    LLVMContext LocalCtx;
    SMDiagnostic Err;
    auto Mod = parseAssemblyString(IR, Err, LocalCtx);
    EXPECT_TRUE(Mod) << Err.getMessage().str();
    EJitOptimizerTestAccess Opt(Registry);
    SpecializationContext CtxSpec = makeCtx("f", Cell, 1);
    CtxSpec.tier = CompileTier::Baseline;
    Opt.runPipeline(*Mod, CtxSpec);
    Emission Result;
    Result.Canon = canonicalizeModule(*Mod);
    // The physical identity deliberately excludes the instance value.
    Result.Logical = makeLogical("f", Cell, 1);
    return Result;
  };

  Emission E1 = compileFor(1);
  Emission E2 = compileFor(2);
  Emission E3 = compileFor(3);
  EXPECT_EQ(E1.Canon.bytes, E2.Canon.bytes)
      << "equal configuration must produce one canonical emission";
  EXPECT_NE(E1.Canon.bytes, E3.Canon.bytes)
      << "a differing may_const value must stay a different emission";

  CodeReuseTable Table;
  CodeKey Key;
  Key.sourceId = "src-rev-1";
  Key.entry = "f";
  Key.compilerPolicy = "async+pgo+shared-v1";
  Key.bindings = BindingEnvironment();
  Key.finalIrDigest = E1.Canon.digest;

  auto Add = Table.addCandidate(Key, E1.Canon);
  ASSERT_TRUE(Add.ok);
  ASSERT_TRUE(Table.markLinked(Add.codeId, 0x1000, {{0x1000, 64}}, {}, "near"));
  ASSERT_TRUE(Table.markPublished(Add.codeId));

  // Instance 2 reuses the same physical record ...
  ReuseLookup Hit = Table.lookup(Key, E2.Canon.bytes);
  EXPECT_EQ(Hit.decision, ReuseDecision::ReusedPublished);
  EXPECT_EQ(Hit.codeId, Add.codeId);
  // ... while remaining a distinct logical version.
  EXPECT_NE(E1.Logical.encode(), E2.Logical.encode());
  ASSERT_TRUE(Table.bindLogical(E1.Logical, Add.codeId));
  ASSERT_TRUE(Table.bindLogical(E2.Logical, Add.codeId));
  EXPECT_EQ(Table.logicalRefs(Add.codeId), 2u);
  EXPECT_EQ(Table.stats().execBytesUnique, 64u);
  EXPECT_EQ(Table.stats().reusedVersions, 2u);

  // Instance 3's changed configuration finds no match.
  CodeKey Key3 = Key;
  Key3.finalIrDigest = E3.Canon.digest;
  ReuseLookup Miss = Table.lookup(Key3, E3.Canon.bytes);
  EXPECT_EQ(Miss.decision, ReuseDecision::CompileIndependently);
  EXPECT_FALSE(Miss.digestCollision);
}

} // namespace
