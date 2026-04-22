add_llvm_component_library(LLVMAsmPrinter
  AccelTable.cpp
  AddressPool.cpp
  AIXException.cpp
  ARMException.cpp
  AsmPrinter.cpp
  AsmPrinterDwarf.cpp
  AsmPrinterInlineAsm.cpp
  DbgEntityHistoryCalculator.cpp
  DebugHandlerBase.cpp
  DebugLocStream.cpp
  DIE.cpp
  DIEHash.cpp
  DwarfCFIException.cpp
  DwarfCompileUnit.cpp
  DwarfDebug.cpp
  DwarfExpression.cpp
  DwarfFile.cpp
  DwarfStringPool.cpp
  DwarfUnit.cpp
  EHStreamer.cpp
  ErlangGCPrinter.cpp
  OcamlGCPrinter.cpp
  PseudoProbePrinter.cpp
  WinCFGuard.cpp
  WinException.cpp
  CodeViewDebug.cpp
  WasmException.cpp

  DEPENDS
  intrinsics_gen

  LINK_COMPONENTS
  Analysis
  BinaryFormat
  CodeGen
  Core
  DebugInfoCodeView
  DebugInfoDWARF
  DebugInfoMSF
  MC
  MCParser
  Remarks
  Support
  Target
  )

# Experimental embedded/JIT option: replace the MCParser-based inline-asm
# emission path with a raw-text fallback. Breaks the force-reference from
# libLLVMAsmPrinter.a(AsmPrinterInlineAsm.cpp.o) into libLLVMMCParser.a.
# Only affects JIT/embedded builds that never feed inline asm to the
# AsmPrinter.
option(LLVM_DISABLE_INLINE_ASM_PARSER
       "Disable the MCParser-based inline asm path in AsmPrinter" OFF)
if(LLVM_DISABLE_INLINE_ASM_PARSER)
  target_compile_definitions(LLVMAsmPrinter PRIVATE LLVM_DISABLE_INLINE_ASM_PARSER=1)
endif()

# Experimental embedded/JIT option: skip the DwarfDebug / CodeViewDebug
# handler construction in AsmPrinter::doInitialization. When the JIT
# input module carries no debuginfo metadata, this prevents pulling
# DwarfDebug.cpp.o, DwarfCompileUnit.cpp.o, DwarfUnit.cpp.o,
# CodeViewDebug.cpp.o and friends (and transitively
# libLLVMDebugInfoDWARF.a / libLLVMDebugInfoCodeView.a) from
# libLLVMAsmPrinter.a. Binaries built with this option will silently
# drop any DI metadata that does reach codegen.
option(LLVM_DISABLE_DEBUG_INFO_EMISSION
       "Disable DWARF/CodeView debug-info emission in AsmPrinter" OFF)
if(LLVM_DISABLE_DEBUG_INFO_EMISSION)
  target_compile_definitions(LLVMAsmPrinter PRIVATE LLVM_DISABLE_DEBUG_INFO_EMISSION=1)
endif()
