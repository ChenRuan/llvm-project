// Round-5 light-codegen PoC driver.
//
// Reads a pre-specialized LLVM bitcode file, finds a specified function,
// emits AArch64 machine code with the narrow emitter, and calls the result.
//
// Does NOT use LLVMCodeGen / LLVMSelectionDAG / LLVMAArch64CodeGen /
// LLVMOrcJIT / LLVMRuntimeDyld / LLVMMC*. Only LLVMCore + LLVMBitReader +
// LLVMSupport are linked (LLVMIRReader pulls LLVMBitReader).

#include "light_aarch64.h"

#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static void die(const char *m) { std::fprintf(stderr, "light_add_int: %s\n", m); std::exit(1); }

int main(int argc, char **argv) {
  if (argc < 3) die("usage: light_add_int <ir-file> <func> [arg]");
  const char *path = argv[1];
  const char *fname = argv[2];
  int inArg = (argc >= 4) ? std::atoi(argv[3]) : 0;

  llvm::LLVMContext Ctx;
  llvm::SMDiagnostic Err;
  auto M = llvm::parseIRFile(path, Err, Ctx);
  if (!M) { Err.print("light_add_int", llvm::errs()); return 2; }

  llvm::Function *F = M->getFunction(fname);
  if (!F) die("function not found");

  light::Result R;
  void *code = light::compile(*F, R);
  if (!code) {
    std::fprintf(stderr, "light backend rejected: %s\n", R.reason.c_str());
    return 3;
  }
  std::fprintf(stderr, "[light] emitted %zu bytes of aarch64 machine code\n", R.codeBytes);
  const uint8_t *b = (const uint8_t *)code;
  std::fprintf(stderr, "[light] bytes:");
  for (size_t i = 0; i < R.codeBytes; ++i) std::fprintf(stderr, " %02x", b[i]);
  std::fprintf(stderr, "\n");

  using Fn = int (*)(int);
  Fn f = (Fn)code;
  for (int a = inArg; a < inArg + 4; ++a) {
    int y = f(a);
    std::printf("light_inc(%d) is %d\n", a, y);
  }
  return 0;
}
