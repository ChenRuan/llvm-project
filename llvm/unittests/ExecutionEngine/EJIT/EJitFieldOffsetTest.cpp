//===-- EJitFieldOffsetTest.cpp - ejitMayConstFieldOffset unit tests ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// ejitMayConstFieldOffset decides *which field* a load names, and that answer is
// the only gate on whether the load is treated as may_const. Getting it wrong is
// silent: the generated code stays correct-looking while either losing a
// specialization (offset not matched) or folding a field that is free to change
// (offset matched for the wrong field). Neither shows up as a test failure
// anywhere else, so the accepted and rejected GEP shapes are pinned here.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitCommon.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"

#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::ejit;

namespace {

/// %S is 7 x i32 = 28 bytes, so field offsets are 0,4,8,12,16,20,24 and the
/// period array @g has an element stride of 28. Field 4 (offset 16) is the
/// one every accepted shape below must resolve to.
constexpr uint64_t kElemSize = 28;
constexpr uint64_t kField4Offset = 16;

class FieldOffsetTest : public testing::Test {
protected:
  LLVMContext Ctx;
  std::unique_ptr<Module> M;

  /// Parse \p Body as the entire body of `@f(i64 %ci, i64 %j)` and return the
  /// pointer operand of its single load.
  const Value *ptrOfLoadIn(StringRef Body) {
    std::string Src = R"(
      target datalayout = "e-m:e-i64:64-i128:128-n32:64-S128"
      %S = type { i32, i32, i32, i32, i32, i32, i32 }
      @g = external global [16 x %S]
      @scalar = external global %S
      define void @f(i64 %ci, i64 %j) {
      )" + Body.str() + R"(
        ret void
      }
    )";
    SMDiagnostic Err;
    M = parseAssemblyString(Src, Err, Ctx);
    if (!M) {
      Err.print("FieldOffsetTest", errs());
      return nullptr;
    }
    for (Instruction &I : instructions(M->getFunction("f")))
      if (auto *LI = dyn_cast<LoadInst>(&I))
        return LI->getPointerOperand();
    return nullptr;
  }

  std::optional<uint64_t> offsetOf(StringRef Body,
                                   const GlobalVariable **OutGV = nullptr) {
    const Value *Ptr = ptrOfLoadIn(Body);
    EXPECT_NE(Ptr, nullptr);
    if (!Ptr)
      return std::nullopt;
    const GlobalVariable *GV = nullptr;
    auto Off = ejitMayConstFieldOffset(Ptr, M->getDataLayout(), GV);
    if (OutGV)
      *OutGV = GV;
    return Off;
  }
};

//===----------------------------------------------------------------------===//
// Accepted shapes: every one names field 4 and must yield its element-relative
// offset, independent of which element is selected.
//===----------------------------------------------------------------------===//

TEST_F(FieldOffsetTest, DynamicElementIndex) {
  // g[ci].f4 -- the AOT shape, before the JIT substitutes ci.
  const GlobalVariable *GV = nullptr;
  auto Off = offsetOf(R"(
    %p = getelementptr [16 x %S], ptr @g, i64 0, i64 %ci, i32 4
    %v = load i32, ptr %p
  )", &GV);
  ASSERT_TRUE(Off.has_value());
  EXPECT_EQ(*Off, kField4Offset);
  ASSERT_NE(GV, nullptr);
  EXPECT_EQ(GV->getName(), "g");
}

TEST_F(FieldOffsetTest, ConstantElementIndexReducesModuloStride) {
  // g[3].f4 -- total offset 3*28 + 16 = 100, which must reduce to 16.
  auto Off = offsetOf(R"(
    %p = getelementptr [16 x %S], ptr @g, i64 0, i64 3, i32 4
    %v = load i32, ptr %p
  )");
  ASSERT_TRUE(Off.has_value());
  EXPECT_EQ(*Off, kField4Offset);
  EXPECT_EQ(3 * kElemSize + kField4Offset, 100u); // the offset that used to miss
}

TEST_F(FieldOffsetTest, FlatByteGEP) {
  // What InstCombine leaves behind once the index is a constant: @g + 156.
  auto Off = offsetOf(R"(
    %p = getelementptr i8, ptr @g, i64 156
    %v = load i32, ptr %p
  )");
  ASSERT_TRUE(Off.has_value());
  EXPECT_EQ(*Off, kField4Offset); // 156 % 28
}

TEST_F(FieldOffsetTest, DecayedArrayToPointer) {
  // &g[ci] with the array decayed: the element selector is the only index.
  auto Off = offsetOf(R"(
    %p = getelementptr %S, ptr @g, i64 %ci
    %v = load i32, ptr %p
  )");
  ASSERT_TRUE(Off.has_value());
  EXPECT_EQ(*Off, 0u); // names field 0
}

TEST_F(FieldOffsetTest, LoadDirectlyFromGlobal) {
  auto Off = offsetOf(R"(
    %v = load i32, ptr @g
  )");
  ASSERT_TRUE(Off.has_value());
  EXPECT_EQ(*Off, 0u);
}

TEST_F(FieldOffsetTest, ChainedConstantByteGEPsInsideElement) {
  // g[ci] then +16: the shape clang emits after canonicalizing field GEPs.
  auto Off = offsetOf(R"(
    %e = getelementptr [16 x %S], ptr @g, i64 0, i64 %ci
    %p = getelementptr i8, ptr %e, i64 16
    %v = load i32, ptr %p
  )");
  ASSERT_TRUE(Off.has_value());
  EXPECT_EQ(*Off, kField4Offset);
}

TEST_F(FieldOffsetTest, NonArrayGlobalKeepsStructOffset) {
  // A scalar `ejit_period` global: no array, so no modulo is applied.
  auto Off = offsetOf(R"(
    %p = getelementptr %S, ptr @scalar, i32 0, i32 4
    %v = load i32, ptr %p
  )");
  ASSERT_TRUE(Off.has_value());
  EXPECT_EQ(*Off, kField4Offset);
}

//===----------------------------------------------------------------------===//
// Rejected shapes. Each of these could otherwise resolve to a plausible field
// offset and cause a load the frontend never marked to be annotated may_const.
//===----------------------------------------------------------------------===//

TEST_F(FieldOffsetTest, RejectsTypedPointerDynamicIndex) {
  // ((int *)&g[ci])[j] -- %j walks *within* an element with stride 4, not 28.
  // Skipping it would resolve to offset 0 and wrongly claim field 0, letting the
  // JIT later fold a field that is free to change.
  auto Off = offsetOf(R"(
    %e = getelementptr [16 x %S], ptr @g, i64 0, i64 %ci
    %p = getelementptr i32, ptr %e, i64 %j
    %v = load i32, ptr %p
  )");
  EXPECT_FALSE(Off.has_value());
}

TEST_F(FieldOffsetTest, RejectsDynamicByteOffset) {
  auto Off = offsetOf(R"(
    %e = getelementptr [16 x %S], ptr @g, i64 0, i64 %ci
    %p = getelementptr i8, ptr %e, i64 %j
    %v = load i32, ptr %p
  )");
  EXPECT_FALSE(Off.has_value());
}

TEST_F(FieldOffsetTest, RejectsDynamicIndexNotRootedAtGlobal) {
  // Same stride as the element, but applied to a derived pointer rather than to
  // the global, so it is not the element selector.
  auto Off = offsetOf(R"(
    %e = getelementptr [16 x %S], ptr @g, i64 0, i64 3
    %p = getelementptr %S, ptr %e, i64 %j
    %v = load i32, ptr %p
  )");
  EXPECT_FALSE(Off.has_value());
}

TEST_F(FieldOffsetTest, RejectsTwoDynamicIndices) {
  auto Off = offsetOf(R"(
    %p = getelementptr [16 x %S], ptr @g, i64 %j, i64 %ci
    %v = load i32, ptr %p
  )");
  EXPECT_FALSE(Off.has_value());
}

TEST_F(FieldOffsetTest, RejectsNegativeConstantIndex) {
  // getZExtValue() would turn -4 into 2^64-4, wrap the accumulator, and leave a
  // plausible-looking field offset behind.
  auto Off = offsetOf(R"(
    %e = getelementptr [16 x %S], ptr @g, i64 0, i64 3
    %p = getelementptr i8, ptr %e, i64 -4
    %v = load i32, ptr %p
  )");
  EXPECT_FALSE(Off.has_value());
}

TEST_F(FieldOffsetTest, RejectsDynamicIndexOnNonArrayGlobal) {
  // No element stride exists, so nothing may be skipped.
  auto Off = offsetOf(R"(
    %p = getelementptr %S, ptr @scalar, i64 %j
    %v = load i32, ptr %p
  )");
  EXPECT_FALSE(Off.has_value());
}

TEST_F(FieldOffsetTest, RejectsPointerNotRootedAtGlobal) {
  auto Off = offsetOf(R"(
    %a = alloca %S
    %p = getelementptr %S, ptr %a, i32 0, i32 4
    %v = load i32, ptr %p
  )");
  EXPECT_FALSE(Off.has_value());
}

} // namespace
