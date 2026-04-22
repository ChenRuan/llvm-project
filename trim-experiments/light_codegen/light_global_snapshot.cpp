// Round-7 light-codegen driver for global_snapshot / global_partial_snapshot.
//
// Demonstrates the new external-global resolution path: the JIT'd IR contains
// `@g_cfg = external dso_local global %struct.GlobalConfig` with NO
// initializer; the driver supplies a `light::GlobalSymbol` table that maps
// the IR symbol name ("g_cfg") to the live host address of the real C global.
// The emitter materializes that address into x17 via MOVZ/MOVK and issues
// LDRs at constant GEP offsets.
//
// Critically, this test calls the JIT'd function TWICE with the host global
// mutated in between, proving the emitter is NOT using a compile-time
// snapshot: it reads the live bytes at each invocation.

#include "light_aarch64.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// Matches struct GlobalConfig in easy-jit-llvm15/tests/c_api/global_snapshot.c.
typedef struct {
  int enabled;
  int bias;
} GlobalConfig;

// Real host globals (one per supported test). The IR symbol spelling is what
// we match against in the GlobalSymbol table.
extern "C" {
GlobalConfig g_cfg         = {1, 7};   // for global_snapshot.spec.ll
GlobalConfig g_partial_cfg = {1, 7};   // for global_partial_snapshot.spec.ll
}

static void die(const char *m) {
  std::fprintf(stderr, "light_global_snapshot: %s\n", m);
  std::exit(1);
}

int main(int argc, char **argv) {
  if (argc < 3) die("usage: light_global_snapshot <ir-file> <func> [gv_name]");
  const char *path  = argv[1];
  const char *fname = argv[2];
  // Optional third arg overrides the symbol name; default to "g_cfg".
  const char *gvname = (argc >= 4) ? argv[3] : "g_cfg";

  llvm::LLVMContext Ctx;
  llvm::SMDiagnostic Err;
  auto M = llvm::parseIRFile(path, Err, Ctx);
  if (!M) { Err.print(argv[0], llvm::errs()); return 2; }

  llvm::Function *F = M->getFunction(fname);
  if (!F) die("function not found");

  // Route the symbol name to the right host global.
  GlobalConfig *target = nullptr;
  if (!std::strcmp(gvname, "g_cfg"))              target = &g_cfg;
  else if (!std::strcmp(gvname, "g_partial_cfg")) target = &g_partial_cfg;
  else die("unknown gv name (expected g_cfg or g_partial_cfg)");

  light::GlobalSymbol syms[] = {
    {gvname, target},
  };

  light::Result R;
  void *code = light::compile(*F, R, syms, 1);
  if (!code) {
    std::fprintf(stderr, "light backend rejected: %s\n", R.reason.c_str());
    return 3;
  }
  std::fprintf(stderr, "[light] emitted %zu bytes of aarch64 machine code\n",
               R.codeBytes);
  std::fprintf(stderr, "[light] host addr of %s = %p\n", gvname, (void*)target);

  using Fn = int (*)(int);
  Fn f = (Fn)code;

  // Phase 1: enabled=1, bias=7 -> select picks (x + bias) = 5 + 7 = 12.
  target->enabled = 1;
  target->bias    = 7;
  int r1 = f(5);
  std::printf("phase1 (enabled=1,bias=7)    f(5) = %d (expect 12)\n", r1);

  // Phase 2: mutate host memory; re-invoke SAME code pointer.
  // enabled=0 -> select picks (x - 99) = 5 - 99 = -94. bias is irrelevant.
  target->enabled = 0;
  target->bias    = 1000;
  int r2 = f(5);
  std::printf("phase2 (enabled=0,bias=1000) f(5) = %d (expect -94)\n", r2);

  // Phase 3: enabled=1 again with different bias -> 5 + 42 = 47.
  target->enabled = 1;
  target->bias    = 42;
  int r3 = f(5);
  std::printf("phase3 (enabled=1,bias=42)   f(5) = %d (expect 47)\n", r3);

  int pass = (r1 == 12 && r2 == -94 && r3 == 47);
  std::printf("light_global_snapshot: %s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
