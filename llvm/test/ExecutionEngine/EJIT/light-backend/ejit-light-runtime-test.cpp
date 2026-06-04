//===- ejit-light-runtime-test.cpp - light backend runtime e2e ------------===//
//
// EmbeddedJIT 轻量 AArch64 后端的 *运行时端到端* 测试（standalone executable，
// 不依赖 lit / gtest）。由 CMake 自定义目标 check-ejit-light-runtime 运行。
//
// 与 ejit-light-backend-test.cpp（直接测发射器）不同，本测试走完整 runtime
// 路径（任务 F / G 验证）：
//
//   IR -> bitcode -> ejit_register_bitcode -> ejit_set_backend_mode(LIGHT/AUTO)
//        -> ejit_compile_or_get -> EJitCompileDriver::tryCompileLight
//        -> EJitOptimizer(SPEC4/PASS7) -> light::compileAArch64Light
//        -> HostMmapCodeAllocator -> 函数指针
//
// 关键点：light 路径不依赖 ORC 引擎是否创建成功，也不依赖宿主是否注册了
// AArch64 LLVM CodeGen target。即便本构建仅包含 X86 target，只要宿主是
// aarch64，light 路径仍可发射并执行机器码——这正是“极小依赖”后端的价值。
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
#include "llvm/ExecutionEngine/EJIT/EJitRegistration.h"

#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace llvm;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    ++g_checks;                                                                \
    if (!(cond)) {                                                             \
      std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__);          \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

static const char *kAArch64LE_DL =
    "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128";

// Build:  int compute(int x) { return g_threshold + x; }
// where @g_threshold is an external global the runtime binds to a host int.
// Exercises Task G (global symbol mapping) end to end.
std::string buildComputeBitcode() {
  LLVMContext C;
  auto M = std::make_unique<Module>("ejit_light_rt", C);
  M->setTargetTriple(llvm::Triple("aarch64-unknown-linux-gnu"));
  M->setDataLayout(kAArch64LE_DL);

  Type *I32 = Type::getInt32Ty(C);

  // External global @g_threshold (declaration → resolved by registry address).
  auto *G = new GlobalVariable(*M, I32, /*isConstant=*/false,
                               GlobalValue::ExternalLinkage,
                               /*Initializer=*/nullptr, "g_threshold");
  G->setDSOLocal(false);

  FunctionType *FT = FunctionType::get(I32, {I32}, false);
  Function *F =
      Function::Create(FT, GlobalValue::ExternalLinkage, "compute", M.get());
  F->arg_begin()->setName("x");

  BasicBlock *BB = BasicBlock::Create(C, "entry", F);
  IRBuilder<> B(BB);
  Value *Thr = B.CreateLoad(I32, G, "thr");
  Value *Sum = B.CreateAdd(Thr, F->arg_begin(), "sum");
  B.CreateRet(Sum);

  std::string out;
  raw_string_ostream OS(out);
  WriteBitcodeToFile(*M, OS);
  OS.flush();
  return out;
}

} // namespace

int main() {
  std::printf("=== EmbeddedJIT light backend runtime e2e test ===\n");

  CHECK(ejit_light_backend_available(),
        "runtime built with light backend available");

  std::string bc = buildComputeBitcode();
  CHECK(!bc.empty(), "bitcode generated");

  ejit_config_t cfg = {};
  cfg.compileMode = EJIT_COMPILE_SYNC;
  cfg.optLevel = EJIT_OPT_L2;
  cfg.enableLogger = true;
  CHECK(ejit_init(&cfg) == EJIT_OK, "ejit_init");

  // Bind the external global to a host integer (Task G).
  int32_t host_threshold = 100;
  ejit_register_static_var("g_threshold", &host_threshold);

  // Register the function bitcode.
  ejit_register_bitcode("compute",
                        reinterpret_cast<const uint8_t *>(bc.data()),
                        bc.size());

  // Default backend is ORC; switch to forced LIGHT.
  CHECK(ejit_get_backend_mode() == EJIT_BACKEND_ORC,
        "default backend is ORC");
  ejit_set_backend_mode(EJIT_BACKEND_LIGHT);
  CHECK(ejit_get_backend_mode() == EJIT_BACKEND_LIGHT,
        "backend mode set to LIGHT");

  void *pfn = nullptr;
  void *res = ejit_compile_or_get("compute", nullptr, 0, &pfn);
  if (!res) {
    const ejit_error_t *e = ejit_get_last_error();
    if (e)
      std::printf("  last error: code=%d func=%s msg=%s\n", e->code,
                  e->funcName, e->message);
  }
  CHECK(res != nullptr, "light compile produced a function pointer");
  CHECK(pfn == res, "out_pfn matches return");

#if defined(__aarch64__)
  if (res) {
    using Fn = int (*)(int);
    Fn fn = reinterpret_cast<Fn>(res);
    int r1 = fn(5);
    CHECK(r1 == 105, "compute(5) == g_threshold(100) + 5");
    int r2 = fn(-30);
    CHECK(r2 == 70, "compute(-30) == 70");

    // Mutating the host global is reflected (absolute-address binding).
    host_threshold = 1;
    int r3 = fn(9);
    CHECK(r3 == 10, "after host_threshold=1, compute(9) == 10");

    // Cached: second call returns the same pointer.
    void *res2 = ejit_compile_or_get("compute", nullptr, 0, nullptr);
    CHECK(res2 == res, "cache returns same light function pointer");
  }
#else
  std::printf("  (non-aarch64 host: skipping execution of emitted code)\n");
#endif

  // AUTO mode should also succeed for this supported function.
  ejit_clear_cache();
  ejit_set_backend_mode(EJIT_BACKEND_AUTO);
  CHECK(ejit_get_backend_mode() == EJIT_BACKEND_AUTO, "backend mode AUTO");
  void *resAuto = ejit_compile_or_get("compute", nullptr, 0, nullptr);
  CHECK(resAuto != nullptr, "AUTO mode compiled (light) the function");
#if defined(__aarch64__)
  if (resAuto) {
    using Fn = int (*)(int);
    int r = reinterpret_cast<Fn>(resAuto)(2);
    CHECK(r == 3, "AUTO-compiled compute(2) == host_threshold(1) + 2");
  }
#endif

  ejit_shutdown();

  std::printf("\n=== %d checks, %d failures ===\n", g_checks, g_failures);
  std::printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");
  return g_failures == 0 ? 0 : 1;
}
