//===- ejit-light-backend-test.cpp - standalone light backend tests -------===//
//
// EmbeddedJIT 轻量 AArch64 后端的独立测试驱动（standalone executable，
// 不依赖 lit / gtest）。由 CMake 自定义目标 check-ejit-light-backend 运行。
//
// 覆盖（任务 E）：
//   1. endian parity / triple gate / MOVZ-MOVK-MOVN 字节模式
//   2. integer smoke
//   3. FP smoke
//   4. stack args
//   5. dynamic GEP
//   6. pressure / spill
//   7. vector reject
//   8. CodeAllocator（HostMmap / StaticSlab）行为
//
// 设计：
//   * 非 AArch64 host 上也能跑 emit / reject / 字节模式（raw::emit 纯发射）。
//   * AArch64 host 上额外真正执行机器码（HostMmapCodeAllocator +
//     compileAArch64Light，把返回的函数指针 cast 后调用）。
//   * 不依赖任何 SRE 接口。
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/LightBackend/EJitLightBackend.h"
#include "llvm/ExecutionEngine/EJIT/LightBackend/EJitLightAArch64.h"
#include "llvm/ExecutionEngine/EJIT/LightBackend/EJitLightCodeAllocator.h"

#include "llvm/IR/Argument.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/Error.h"
#include "llvm/TargetParser/Triple.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace llvm;
namespace light = llvm::ejit::light;

namespace {

static const char *kAArch64LE_DL =
    "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128";
static const char *kAArch64BE_DL =
    "E-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128";

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    ++g_checks;                                                                \
    if (!(cond)) {                                                             \
      std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__);           \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

// ---- move-wide opcode scanning (MOVZ/MOVK/MOVN) ----
// Move-wide family: bits[28:23] == 0b100101. opc = bits[30:29]:
//   MOVN = 0b00, MOVZ = 0b10, MOVK = 0b11.
struct MoveWideCounts {
  int movz = 0, movk = 0, movn = 0;
};
MoveWideCounts scanMoveWide(const std::vector<uint8_t> &Code) {
  MoveWideCounts C;
  for (size_t i = 0; i + 4 <= Code.size(); i += 4) {
    uint32_t w = (uint32_t)Code[i] | ((uint32_t)Code[i + 1] << 8) |
                 ((uint32_t)Code[i + 2] << 16) | ((uint32_t)Code[i + 3] << 24);
    if ((w & 0x1F800000u) == 0x12800000u) {
      unsigned opc = (w >> 29) & 0x3u;
      if (opc == 0)
        ++C.movn;
      else if (opc == 2)
        ++C.movz;
      else if (opc == 3)
        ++C.movk;
    }
  }
  return C;
}

const char *rawStatusName(light::raw::Status s) {
  switch (s) {
  case light::raw::Status::Ok:
    return "Ok";
  case light::raw::Status::Unsupported:
    return "Unsupported";
  case light::raw::Status::TooLarge:
    return "TooLarge";
  case light::raw::Status::NotAarch64:
    return "NotAarch64";
  }
  return "?";
}

std::unique_ptr<Module> newModule(LLVMContext &C, const char *Triple,
                                  const char *DL) {
  auto M = std::make_unique<Module>("light_test", C);
  M->setTargetTriple(llvm::Triple(Triple));
  M->setDataLayout(DL);
  return M;
}

// Endian-sensitive sample: forces MOVZ + MOVK halfword chains via a wide
// positive constant and an i32 negative constant (bit-pattern path, no MOVN).
Function *buildEndianSample(Module &M, LLVMContext &C) {
  Type *I32 = Type::getInt32Ty(C);
  Type *I16 = Type::getInt16Ty(C);
  Type *Ptr = PointerType::get(C, 0);
  FunctionType *FT = FunctionType::get(I32, {Ptr}, false);
  Function *F =
      Function::Create(FT, GlobalValue::ExternalLinkage, "f", &M);
  BasicBlock *BB = BasicBlock::Create(C, "entry", F);
  IRBuilder<> B(BB);
  Argument *P = F->getArg(0);
  Value *a = B.CreateAlignedLoad(I32, P, MaybeAlign(4), "a");
  Value *qp = B.CreateGEP(I32, P, B.getInt64(1), "qp");
  Value *b = B.CreateAlignedLoad(I16, qp, MaybeAlign(2), "b");
  Value *bz = B.CreateZExt(b, I32, "bz");
  Value *sum = B.CreateAdd(a, bz, "sum");
  Value *k = B.CreateAdd(sum, B.getInt32(0x123), "k");
  Value *k2 = B.CreateAdd(k, B.getInt32(0x12345), "k2");
  Value *k3 = B.CreateAdd(k2, B.getInt32(-12345), "k3");
  B.CreateAlignedStore(k3, P, MaybeAlign(4));
  B.CreateRet(k3);
  return F;
}

//===----------------------------------------------------------------------===//
// 1. endian parity + triple gate + MOVZ/MOVK/MOVN
//===----------------------------------------------------------------------===//
void testEndianAndTripleGate() {
  std::printf("[test] endian parity + triple gate\n");
  struct Case {
    const char *triple;
    const char *dl;
    bool accept;
  };
  Case cases[] = {
      {"aarch64-unknown-linux-gnu", kAArch64LE_DL, true},
      {"aarch64_be-unknown-linux-gnu", kAArch64BE_DL, true},
      {"arm64-apple-darwin", kAArch64LE_DL, true},
      {"aarch64_32-unknown-linux-gnu", kAArch64LE_DL, false},
      {"x86_64-unknown-linux-gnu", kAArch64LE_DL, false},
  };

  std::vector<uint8_t> leBytes, beBytes;
  for (const Case &c : cases) {
    LLVMContext Ctx;
    auto M = newModule(Ctx, c.triple, c.dl);
    Function *F = buildEndianSample(*M, Ctx);
    std::vector<uint8_t> buf(8192);
    light::raw::Result r =
        light::raw::emit(*F, buf.data(), buf.size(), nullptr, 0);
    std::printf("  triple=%-32s status=%-12s bytes=%zu\n", c.triple,
                rawStatusName(r.status), r.codeBytes);
    if (c.accept) {
      CHECK(r.status == light::raw::Status::Ok, "expected accept");
      CHECK(r.codeBytes > 0 && r.codeBytes <= buf.size(), "codeBytes sane");
      std::vector<uint8_t> code(buf.begin(), buf.begin() + r.codeBytes);
      if (std::strncmp(c.triple, "aarch64_be", 10) == 0)
        beBytes = code;
      else if (std::strncmp(c.triple, "aarch64-", 8) == 0)
        leBytes = code;
    } else {
      CHECK(r.status == light::raw::Status::NotAarch64,
            "expected NotAarch64 reject");
    }
  }

  // endian parity: aarch64 LE and aarch64_be byte streams bit-identical.
  CHECK(!leBytes.empty() && !beBytes.empty(), "both LE/BE emitted");
  CHECK(leBytes == beBytes, "LE and BE instruction bytes bit-identical");

  // MOVZ >= 1, MOVK >= 1, MOVN == 0 (bit-pattern materialization design).
  MoveWideCounts mc = scanMoveWide(leBytes);
  std::printf("  move-wide: MOVZ=%d MOVK=%d MOVN=%d\n", mc.movz, mc.movk,
              mc.movn);
  CHECK(mc.movz >= 1, "MOVZ >= 1");
  CHECK(mc.movk >= 1, "MOVK >= 1");
  CHECK(mc.movn == 0, "MOVN == 0 (no MOVN-based materialization)");
}

//===----------------------------------------------------------------------===//
// Execution harness (AArch64 host only)
//===----------------------------------------------------------------------===//
#if defined(__aarch64__)
// Compile F via the light backend into executable host memory and return the
// function pointer, or nullptr (printing the reason) on failure.
void *jitCompile(Function &F, ArrayRef<light::GlobalSymbol> Globals,
                 light::HostMmapCodeAllocator &Alloc) {
  light::CompileResult CR;
  Expected<void *> P = light::compileAArch64Light(F, Globals, Alloc, &CR);
  if (!P) {
    std::string EM = toString(P.takeError());
    std::printf("  jit failed: status=%d reason=%s err=%s\n", (int)CR.status,
                CR.reason.c_str(), EM.c_str());
    return nullptr;
  }
  return *P;
}
#endif

//===----------------------------------------------------------------------===//
// 2. integer smoke
//===----------------------------------------------------------------------===//
void testIntegerSmoke() {
  std::printf("[test] integer smoke\n");
  LLVMContext Ctx;
  auto M = newModule(Ctx, "aarch64-unknown-linux-gnu", kAArch64LE_DL);
  Type *I32 = Type::getInt32Ty(Ctx);
  Type *I64 = Type::getInt64Ty(Ctx);

  // i32 poly(a,b,c) = (a + b - c) * a   ; exercises add/sub/mul.
  Function *Poly;
  {
    FunctionType *FT = FunctionType::get(I32, {I32, I32, I32}, false);
    Poly = Function::Create(FT, GlobalValue::ExternalLinkage, "poly", M.get());
    IRBuilder<> B(BasicBlock::Create(Ctx, "e", Poly));
    Value *s = B.CreateAdd(Poly->getArg(0), Poly->getArg(1));
    s = B.CreateSub(s, Poly->getArg(2));
    s = B.CreateMul(s, Poly->getArg(0));
    B.CreateRet(s);
  }
  // i64 addsub(a,b) = a - b + (-7)      ; negative constant + i64.
  Function *AddSub;
  {
    FunctionType *FT = FunctionType::get(I64, {I64, I64}, false);
    AddSub =
        Function::Create(FT, GlobalValue::ExternalLinkage, "addsub", M.get());
    IRBuilder<> B(BasicBlock::Create(Ctx, "e", AddSub));
    Value *s = B.CreateSub(AddSub->getArg(0), AddSub->getArg(1));
    s = B.CreateAdd(s, B.getInt64(-7));
    B.CreateRet(s);
  }
  // i32 sel(a,b) = (a < b) ? a : b      ; icmp + select + branch + phi.
  Function *Sel;
  {
    FunctionType *FT = FunctionType::get(I32, {I32, I32}, false);
    Sel = Function::Create(FT, GlobalValue::ExternalLinkage, "sel", M.get());
    BasicBlock *E = BasicBlock::Create(Ctx, "e", Sel);
    BasicBlock *T = BasicBlock::Create(Ctx, "t", Sel);
    BasicBlock *Fb = BasicBlock::Create(Ctx, "f", Sel);
    BasicBlock *J = BasicBlock::Create(Ctx, "j", Sel);
    IRBuilder<> B(E);
    Value *c = B.CreateICmpSLT(Sel->getArg(0), Sel->getArg(1));
    B.CreateCondBr(c, T, Fb);
    B.SetInsertPoint(T);
    B.CreateBr(J);
    B.SetInsertPoint(Fb);
    B.CreateBr(J);
    B.SetInsertPoint(J);
    PHINode *phi = B.CreatePHI(I32, 2);
    phi->addIncoming(Sel->getArg(0), T);
    phi->addIncoming(Sel->getArg(1), Fb);
    B.CreateRet(phi);
  }

  // emit-only validation everywhere.
  for (Function *F : {Poly, AddSub, Sel}) {
    std::vector<uint8_t> buf(8192);
    auto r = light::raw::emit(*F, buf.data(), buf.size(), nullptr, 0);
    std::printf("  %-8s status=%s bytes=%zu\n", F->getName().str().c_str(),
                rawStatusName(r.status), r.codeBytes);
    CHECK(r.status == light::raw::Status::Ok, "integer fn emits Ok");
  }

#if defined(__aarch64__)
  light::HostMmapCodeAllocator Alloc;
  if (void *p = jitCompile(*Poly, {}, Alloc)) {
    auto fn = reinterpret_cast<int32_t (*)(int32_t, int32_t, int32_t)>(p);
    int32_t got = fn(5, 3, 2); // (5+3-2)*5 = 30
    std::printf("  poly(5,3,2) = %d (want 30)\n", got);
    CHECK(got == 30, "poly executes correctly");
  }
  if (void *p = jitCompile(*AddSub, {}, Alloc)) {
    auto fn = reinterpret_cast<int64_t (*)(int64_t, int64_t)>(p);
    int64_t got = fn(100, 40); // 100-40-7 = 53
    std::printf("  addsub(100,40) = %lld (want 53)\n", (long long)got);
    CHECK(got == 53, "addsub executes correctly");
  }
  if (void *p = jitCompile(*Sel, {}, Alloc)) {
    auto fn = reinterpret_cast<int32_t (*)(int32_t, int32_t)>(p);
    std::printf("  sel(7,4)=%d sel(2,9)=%d (want 4,2)\n", fn(7, 4), fn(2, 9));
    CHECK(fn(7, 4) == 4 && fn(2, 9) == 2, "sel executes correctly");
  }
#endif
}

//===----------------------------------------------------------------------===//
// 3. FP smoke
//===----------------------------------------------------------------------===//
void testFpSmoke() {
  std::printf("[test] FP smoke\n");
  LLVMContext Ctx;
  auto M = newModule(Ctx, "aarch64-unknown-linux-gnu", kAArch64LE_DL);
  Type *F32 = Type::getFloatTy(Ctx);
  Type *F64 = Type::getDoubleTy(Ctx);

  // float fexpr(a,b,c) = (a + b) * c - a/b   ; fadd/fmul/fsub/fdiv f32.
  Function *FExpr;
  {
    FunctionType *FT = FunctionType::get(F32, {F32, F32, F32}, false);
    FExpr =
        Function::Create(FT, GlobalValue::ExternalLinkage, "fexpr", M.get());
    IRBuilder<> B(BasicBlock::Create(Ctx, "e", FExpr));
    Value *s = B.CreateFAdd(FExpr->getArg(0), FExpr->getArg(1));
    s = B.CreateFMul(s, FExpr->getArg(2));
    Value *d = B.CreateFDiv(FExpr->getArg(0), FExpr->getArg(1));
    s = B.CreateFSub(s, d);
    B.CreateRet(s);
  }
  // double dexpr(a,b) = -(a/b) + a*b          ; fneg/fdiv/fmul/fadd f64.
  Function *DExpr;
  {
    FunctionType *FT = FunctionType::get(F64, {F64, F64}, false);
    DExpr =
        Function::Create(FT, GlobalValue::ExternalLinkage, "dexpr", M.get());
    IRBuilder<> B(BasicBlock::Create(Ctx, "e", DExpr));
    Value *d = B.CreateFDiv(DExpr->getArg(0), DExpr->getArg(1));
    Value *n = B.CreateFNeg(d);
    Value *m = B.CreateFMul(DExpr->getArg(0), DExpr->getArg(1));
    B.CreateRet(B.CreateFAdd(n, m));
  }
  // float fmin2(a,b) = (a < b) ? a : b        ; ordered fcmp + select.
  Function *FMin;
  {
    FunctionType *FT = FunctionType::get(F32, {F32, F32}, false);
    FMin =
        Function::Create(FT, GlobalValue::ExternalLinkage, "fmin2", M.get());
    BasicBlock *E = BasicBlock::Create(Ctx, "e", FMin);
    BasicBlock *T = BasicBlock::Create(Ctx, "t", FMin);
    BasicBlock *Fb = BasicBlock::Create(Ctx, "f", FMin);
    BasicBlock *J = BasicBlock::Create(Ctx, "j", FMin);
    IRBuilder<> B(E);
    Value *c = B.CreateFCmpOLT(FMin->getArg(0), FMin->getArg(1));
    B.CreateCondBr(c, T, Fb);
    B.SetInsertPoint(T);
    B.CreateBr(J);
    B.SetInsertPoint(Fb);
    B.CreateBr(J);
    B.SetInsertPoint(J);
    PHINode *phi = B.CreatePHI(F32, 2);
    phi->addIncoming(FMin->getArg(0), T);
    phi->addIncoming(FMin->getArg(1), Fb);
    B.CreateRet(phi);
  }

  for (Function *F : {FExpr, DExpr, FMin}) {
    std::vector<uint8_t> buf(8192);
    auto r = light::raw::emit(*F, buf.data(), buf.size(), nullptr, 0);
    std::printf("  %-8s status=%s bytes=%zu reason=%s\n",
                F->getName().str().c_str(), rawStatusName(r.status),
                r.codeBytes, r.reason.c_str());
    CHECK(r.status == light::raw::Status::Ok, "fp fn emits Ok");
  }

#if defined(__aarch64__)
  light::HostMmapCodeAllocator Alloc;
  if (void *p = jitCompile(*FExpr, {}, Alloc)) {
    auto fn = reinterpret_cast<float (*)(float, float, float)>(p);
    float got = fn(2.0f, 4.0f, 3.0f); // (2+4)*3 - 2/4 = 18 - 0.5 = 17.5
    std::printf("  fexpr(2,4,3) = %f (want 17.5)\n", got);
    CHECK(got == 17.5f, "fexpr executes correctly");
  }
  if (void *p = jitCompile(*DExpr, {}, Alloc)) {
    auto fn = reinterpret_cast<double (*)(double, double)>(p);
    double got = fn(6.0, 2.0); // -(6/2) + 6*2 = -3 + 12 = 9
    std::printf("  dexpr(6,2) = %f (want 9)\n", got);
    CHECK(got == 9.0, "dexpr executes correctly");
  }
  if (void *p = jitCompile(*FMin, {}, Alloc)) {
    auto fn = reinterpret_cast<float (*)(float, float)>(p);
    std::printf("  fmin2(3,5)=%f fmin2(8,1)=%f (want 3,1)\n", fn(3, 5),
                fn(8, 1));
    CHECK(fn(3, 5) == 3.0f && fn(8, 1) == 1.0f, "fmin2 executes correctly");
  }
#endif
}

//===----------------------------------------------------------------------===//
// 4. stack args (9th arg spills to stack)
//===----------------------------------------------------------------------===//
void testStackArgs() {
  std::printf("[test] stack args\n");
  LLVMContext Ctx;
  auto M = newModule(Ctx, "aarch64-unknown-linux-gnu", kAArch64LE_DL);
  Type *I64 = Type::getInt64Ty(Ctx);

  // i64 ninth(9 x i64) = arg8 (the 9th arg, stack-passed) + arg0.
  std::vector<Type *> args(9, I64);
  FunctionType *FT = FunctionType::get(I64, args, false);
  Function *F =
      Function::Create(FT, GlobalValue::ExternalLinkage, "ninth", M.get());
  IRBuilder<> B(BasicBlock::Create(Ctx, "e", F));
  Value *s = B.CreateAdd(F->getArg(8), F->getArg(0));
  B.CreateRet(s);

  std::vector<uint8_t> buf(8192);
  auto r = light::raw::emit(*F, buf.data(), buf.size(), nullptr, 0);
  std::printf("  ninth status=%s bytes=%zu reason=%s\n",
              rawStatusName(r.status), r.codeBytes, r.reason.c_str());
  CHECK(r.status == light::raw::Status::Ok, "stack-arg fn emits Ok");

#if defined(__aarch64__)
  light::HostMmapCodeAllocator Alloc;
  if (void *p = jitCompile(*F, {}, Alloc)) {
    auto fn = reinterpret_cast<int64_t (*)(int64_t, int64_t, int64_t, int64_t,
                                           int64_t, int64_t, int64_t, int64_t,
                                           int64_t)>(p);
    int64_t got = fn(10, 0, 0, 0, 0, 0, 0, 0, 90); // 90 + 10 = 100
    std::printf("  ninth(10,...,90) = %lld (want 100)\n", (long long)got);
    CHECK(got == 100, "stack-arg fn executes correctly");
  }
#endif
}

//===----------------------------------------------------------------------===//
// 5. dynamic GEP (2D array load)
//===----------------------------------------------------------------------===//
void testDynamicGep() {
  std::printf("[test] dynamic GEP\n");
  LLVMContext Ctx;
  auto M = newModule(Ctx, "aarch64-unknown-linux-gnu", kAArch64LE_DL);
  Type *I32 = Type::getInt32Ty(Ctx);
  Type *I64 = Type::getInt64Ty(Ctx);
  Type *Ptr = PointerType::get(Ctx, 0);
  ArrayType *Row = ArrayType::get(I32, 4); // [4 x i32]

  // i32 get2d(ptr base, i64 i, i64 j) = base[i][j]  (row-major 4-wide).
  FunctionType *FT = FunctionType::get(I32, {Ptr, I64, I64}, false);
  Function *F =
      Function::Create(FT, GlobalValue::ExternalLinkage, "get2d", M.get());
  IRBuilder<> B(BasicBlock::Create(Ctx, "e", F));
  Value *gep = B.CreateGEP(Row, F->getArg(0),
                           {F->getArg(1), F->getArg(2)}, "elt");
  Value *v = B.CreateAlignedLoad(I32, gep, MaybeAlign(4));
  B.CreateRet(v);

  std::vector<uint8_t> buf(8192);
  auto r = light::raw::emit(*F, buf.data(), buf.size(), nullptr, 0);
  std::printf("  get2d status=%s bytes=%zu reason=%s\n",
              rawStatusName(r.status), r.codeBytes, r.reason.c_str());
  CHECK(r.status == light::raw::Status::Ok, "dynamic-GEP fn emits Ok");

#if defined(__aarch64__)
  light::HostMmapCodeAllocator Alloc;
  if (void *p = jitCompile(*F, {}, Alloc)) {
    auto fn = reinterpret_cast<int32_t (*)(int32_t *, int64_t, int64_t)>(p);
    int32_t grid[3][4] = {{0, 1, 2, 3}, {10, 11, 12, 13}, {20, 21, 22, 23}};
    int32_t got = fn(&grid[0][0], 2, 1); // grid[2][1] = 21
    std::printf("  get2d(grid,2,1) = %d (want 21)\n", got);
    CHECK(got == 21, "dynamic-GEP fn executes correctly");
  }
#endif
}

//===----------------------------------------------------------------------===//
// 6. pressure / spill (many live i32 values)
//===----------------------------------------------------------------------===//
void testPressureSpill() {
  std::printf("[test] pressure / spill\n");
  LLVMContext Ctx;
  auto M = newModule(Ctx, "aarch64-unknown-linux-gnu", kAArch64LE_DL);
  Type *I32 = Type::getInt32Ty(Ctx);

  // i32 sum16(16 x i32) — 16 simultaneously-live SSA adds, exceeds the
  // scratch GPR pool and forces minimal spill/reload.
  std::vector<Type *> args(16, I32);
  FunctionType *FT = FunctionType::get(I32, args, false);
  Function *F =
      Function::Create(FT, GlobalValue::ExternalLinkage, "sum16", M.get());
  IRBuilder<> B(BasicBlock::Create(Ctx, "e", F));
  // Build a tree so many values stay live at once.
  std::vector<Value *> v;
  for (unsigned i = 0; i < 16; ++i)
    v.push_back(F->getArg(i));
  Value *acc = v[0];
  for (unsigned i = 1; i < 16; ++i)
    acc = B.CreateAdd(acc, v[i]);
  // also multiply in a couple to widen live ranges
  acc = B.CreateAdd(acc, B.CreateMul(v[3], v[5]));
  B.CreateRet(acc);

  std::vector<uint8_t> buf(16384);
  auto r = light::raw::emit(*F, buf.data(), buf.size(), nullptr, 0);
  std::printf("  sum16 status=%s bytes=%zu reason=%s\n",
              rawStatusName(r.status), r.codeBytes, r.reason.c_str());
  CHECK(r.status == light::raw::Status::Ok, "pressure fn emits Ok");

#if defined(__aarch64__)
  light::HostMmapCodeAllocator Alloc;
  if (void *p = jitCompile(*F, {}, Alloc)) {
    auto fn = reinterpret_cast<int32_t (*)(
        int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
        int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
        int32_t)>(p);
    int32_t got = fn(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16);
    // sum(1..16)=136, plus v[3]*v[5]=4*6=24 -> 160
    std::printf("  sum16(1..16) = %d (want 160)\n", got);
    CHECK(got == 160, "pressure fn executes correctly");
  }
#endif
}

//===----------------------------------------------------------------------===//
// 7. vector reject
//===----------------------------------------------------------------------===//
void testVectorReject() {
  std::printf("[test] vector reject\n");
  LLVMContext Ctx;
  auto M = newModule(Ctx, "aarch64-unknown-linux-gnu", kAArch64LE_DL);
  Type *I32 = Type::getInt32Ty(Ctx);
  VectorType *V4I32 = VectorType::get(I32, 4, false);

  // <4 x i32> vadd(<4 x i32> a, <4 x i32> b) = a + b
  FunctionType *FT = FunctionType::get(V4I32, {V4I32, V4I32}, false);
  Function *F =
      Function::Create(FT, GlobalValue::ExternalLinkage, "vadd", M.get());
  IRBuilder<> B(BasicBlock::Create(Ctx, "e", F));
  B.CreateRet(B.CreateAdd(F->getArg(0), F->getArg(1)));

  // candidate checker should reject with a vector reason.
  std::string Reason;
  bool cand = light::isLightBackendCandidate(*F, &Reason);
  std::printf("  candidate=%d reason=%s\n", cand, Reason.c_str());
  CHECK(!cand, "vector fn rejected by candidate checker");
  CHECK(Reason.find("vector") != std::string::npos,
        "reject reason mentions vector");

  // emitter must also reject (two-layer validation).
  std::vector<uint8_t> buf(8192);
  auto r = light::raw::emit(*F, buf.data(), buf.size(), nullptr, 0);
  std::printf("  emit status=%s reason=%s\n", rawStatusName(r.status),
              r.reason.c_str());
  CHECK(r.status != light::raw::Status::Ok, "emitter rejects vector fn");
}

//===----------------------------------------------------------------------===//
// 8. CodeAllocator behaviour
//===----------------------------------------------------------------------===//
void testAllocators() {
  std::printf("[test] code allocators\n");
  // StaticSlabCodeAllocator: bump allocate + exhaustion.
  std::vector<uint8_t> slab(256, 0);
  light::StaticSlabCodeAllocator SA(slab.data(), slab.size());
  auto a1 = SA.allocate(64, 16);
  CHECK((bool)a1, "slab alloc 64 ok");
  if (a1)
    CHECK(a1->size() == 64, "slab alloc size");
  auto a2 = SA.allocate(64, 16);
  CHECK((bool)a2, "slab alloc 64 #2 ok");
  if (!a1)
    consumeError(a1.takeError());
  if (!a2)
    consumeError(a2.takeError());
  // finalize with no hooks = no-op success.
  if (a1)
    CHECK(!SA.finalize(*a1), "slab finalize no-op ok");
  // exhaust
  auto a3 = SA.allocate(1024, 16);
  CHECK(!a3, "slab exhaustion returns error");
  if (!a3)
    consumeError(a3.takeError());
  SA.reset();
  CHECK(SA.used() == 0, "slab reset rewinds");

#if !defined(_WIN32)
  // HostMmapCodeAllocator: allocate + finalize RX + release.
  light::HostMmapCodeAllocator HA;
  auto h1 = HA.allocate(128, 64);
  CHECK((bool)h1, "mmap alloc ok");
  if (h1) {
    // write a RET (0xD65F03C0) and finalize, just to exercise the path.
    uint32_t ret = 0xD65F03C0u;
    std::memcpy(h1->data(), &ret, 4);
    CHECK(!HA.finalize(*h1), "mmap finalize RX ok");
    HA.release(*h1);
  } else {
    consumeError(h1.takeError());
  }
#endif
}

} // namespace

int main() {
  std::printf("=== EmbeddedJIT light backend standalone tests ===\n");
#if defined(__aarch64__)
  std::printf("host: aarch64 (execution enabled)\n");
#else
  std::printf("host: non-aarch64 (emit/reject/bytes only, no execution)\n");
#endif

  testEndianAndTripleGate();
  testIntegerSmoke();
  testFpSmoke();
  testStackArgs();
  testDynamicGep();
  testPressureSpill();
  testVectorReject();
  testAllocators();

  std::printf("=== checks=%d failures=%d ===\n", g_checks, g_failures);
  if (g_failures) {
    std::printf("ejit-light-backend-test: FAIL\n");
    return 1;
  }
  std::printf("ejit-light-backend-test: PASS\n");
  return 0;
}
