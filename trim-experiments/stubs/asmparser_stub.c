// Compile: gcc -c asmparser_stub.c -o asmparser_stub.o
// Purpose: Provide a no-op definition of LLVMInitializeAArch64AsmParser so
// that the reference from runtime/InitNativeTarget.cpp no longer drags
// libLLVMAArch64AsmParser.a into a static link.
//
// Safe for pure JIT use: AsmParser is only needed when LLVM is asked to
// parse textual assembly (inline asm from IR, llvm-as, Clang integrated
// asm parser). EasyJIT's JIT path only consumes bitcode and goes through
// SelectionDAG/MC bytes-emission, which does not need AsmParser.
//
// Same idea applies to LLVMInitializeAArch64Disassembler (not in baseline)
// and a few other MC helpers the user might choose to stub.

void LLVMInitializeAArch64AsmParser(void) {}
