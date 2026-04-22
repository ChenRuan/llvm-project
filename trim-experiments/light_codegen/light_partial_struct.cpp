// Round-6 light-codegen driver for partial_struct_binding.
//
// Reads the dumped specialized LLVM IR (produced by EASYJIT_DUMP_IR), locates
// the function, emits aarch64 machine code with the narrow emitter, and
// invokes the resulting pointer with the test's expected inputs / expected
// outputs. Does NOT link any LLVM backend libraries.

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

typedef struct {
  int dynamic_value;
  int fixed_flag;
} PartialConfig;

static void die(const char *m) {
  std::fprintf(stderr, "light_partial_struct: %s\n", m);
  std::exit(1);
}

int main(int argc, char **argv) {
  if (argc < 3) die("usage: light_partial_struct <ir-file> <func>");
  const char *path = argv[1];
  const char *fname = argv[2];

  llvm::LLVMContext Ctx;
  llvm::SMDiagnostic Err;
  auto M = llvm::parseIRFile(path, Err, Ctx);
  if (!M) { Err.print(argv[0], llvm::errs()); return 2; }

  llvm::Function *F = M->getFunction(fname);
  if (!F) die("function not found");

  light::Result R;
  void *code = light::compile(*F, R);
  if (!code) {
    std::fprintf(stderr, "light backend rejected: %s\n", R.reason.c_str());
    return 3;
  }
  std::fprintf(stderr, "[light] emitted %zu bytes of aarch64 machine code\n",
               R.codeBytes);
  const uint8_t *b = (const uint8_t *)code;
  std::fprintf(stderr, "[light] bytes:");
  for (size_t i = 0; i < R.codeBytes; ++i)
    std::fprintf(stderr, " %02x", b[i]);
  std::fprintf(stderr, "\n");

  using Fn = int (*)(const PartialConfig *, int);
  Fn f = (Fn)code;

  // Specialization fixes fixed_flag = 7 — so runtime_a's fixed_flag=0 gets
  // OVERWRITTEN to 7 inside the JIT code; expected result_a = 10 + 3 + 7 = 20.
  // Same for runtime_b: expected result_b = 10 + 9 + 7 = 26.
  PartialConfig runtime_a = {3, 0};
  PartialConfig runtime_b = {9, 1234};

  int ra = f(&runtime_a, 10);
  int rb = f(&runtime_b, 10);
  std::printf("light_partial_struct.result_a=%d (expect 20)\n", ra);
  std::printf("light_partial_struct.result_b=%d (expect 26)\n", rb);

  int pass = (ra == 20 && rb == 26);
  std::printf("light_partial_struct: %s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
