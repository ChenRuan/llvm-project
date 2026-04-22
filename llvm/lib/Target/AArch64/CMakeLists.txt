add_llvm_component_group(AArch64 HAS_JIT)

set(LLVM_TARGET_DEFINITIONS AArch64.td)

# Experimental: trim GlobalISel for embedded/static builds. When enabled,
# the AArch64 backend falls back to SelectionDAG-only codegen and the
# libLLVMGlobalISel.a archive (and AArch64's GISel/* TUs) are no longer
# referenced by the final link. The GISel tablegen outputs are still
# generated (cheap) because other .td fragments may depend on them.
option(LLVM_AARCH64_DISABLE_GISEL
       "Disable GlobalISel in the AArch64 backend (SelectionDAG only)" OFF)

tablegen(LLVM AArch64GenAsmMatcher.inc -gen-asm-matcher)
tablegen(LLVM AArch64GenAsmWriter.inc -gen-asm-writer)
tablegen(LLVM AArch64GenAsmWriter1.inc -gen-asm-writer -asmwriternum=1)
tablegen(LLVM AArch64GenCallingConv.inc -gen-callingconv)
tablegen(LLVM AArch64GenDAGISel.inc -gen-dag-isel)
tablegen(LLVM AArch64GenDisassemblerTables.inc -gen-disassembler)
tablegen(LLVM AArch64GenFastISel.inc -gen-fast-isel)
tablegen(LLVM AArch64GenGlobalISel.inc -gen-global-isel)
tablegen(LLVM AArch64GenO0PreLegalizeGICombiner.inc -gen-global-isel-combiner
              -combiners="AArch64O0PreLegalizerCombinerHelper")
tablegen(LLVM AArch64GenPreLegalizeGICombiner.inc -gen-global-isel-combiner
              -combiners="AArch64PreLegalizerCombinerHelper")
tablegen(LLVM AArch64GenPostLegalizeGICombiner.inc -gen-global-isel-combiner
              -combiners="AArch64PostLegalizerCombinerHelper")
tablegen(LLVM AArch64GenPostLegalizeGILowering.inc -gen-global-isel-combiner
              -combiners="AArch64PostLegalizerLoweringHelper")
tablegen(LLVM AArch64GenInstrInfo.inc -gen-instr-info)
tablegen(LLVM AArch64GenMCCodeEmitter.inc -gen-emitter)
tablegen(LLVM AArch64GenMCPseudoLowering.inc -gen-pseudo-lowering)
tablegen(LLVM AArch64GenRegisterBank.inc -gen-register-bank)
tablegen(LLVM AArch64GenRegisterInfo.inc -gen-register-info)
tablegen(LLVM AArch64GenSubtargetInfo.inc -gen-subtarget)
tablegen(LLVM AArch64GenSystemOperands.inc -gen-searchable-tables)
tablegen(LLVM AArch64GenExegesis.inc -gen-exegesis)

add_public_tablegen_target(AArch64CommonTableGen)

# When LLVM_AARCH64_DISABLE_GISEL is ON, we still compile the GISel/* TUs
# (so subtarget/target-machine code can refer to the classes if needed),
# but we cut the force-reference chain (pass initialisers + subtarget
# GISel member construction) so the entire libLLVMGlobalISel.a archive
# and all AArch64 GISel/* .o members are pruned at final static link.
if(LLVM_AARCH64_DISABLE_GISEL)
  add_compile_definitions(LLVM_AARCH64_DISABLE_GISEL=1)
endif()

# Experimental: drop the -O0 FastISel path. The JIT compiler runs at -O2/-O3
# where SelectionDAG is used anyway, so the entire AArch64FastISel.cpp is
# dead code in the embedded static-link scenario.
option(LLVM_AARCH64_DISABLE_FASTISEL
       "Disable the AArch64 FastISel path" OFF)
if(LLVM_AARCH64_DISABLE_FASTISEL)
  add_compile_definitions(LLVM_AARCH64_DISABLE_FASTISEL=1)
endif()

add_llvm_target(AArch64CodeGen
  GISel/AArch64CallLowering.cpp
  GISel/AArch64GlobalISelUtils.cpp
  GISel/AArch64InstructionSelector.cpp
  GISel/AArch64LegalizerInfo.cpp
  GISel/AArch64O0PreLegalizerCombiner.cpp
  GISel/AArch64PreLegalizerCombiner.cpp
  GISel/AArch64PostLegalizerCombiner.cpp
  GISel/AArch64PostLegalizerLowering.cpp
  GISel/AArch64PostSelectOptimize.cpp
  GISel/AArch64RegisterBankInfo.cpp
  AArch64A57FPLoadBalancing.cpp
  AArch64AdvSIMDScalarPass.cpp
  AArch64AsmPrinter.cpp
  AArch64BranchTargets.cpp
  AArch64CallingConvention.cpp
  AArch64CleanupLocalDynamicTLSPass.cpp
  AArch64CollectLOH.cpp
  AArch64CondBrTuning.cpp
  AArch64ConditionalCompares.cpp
  AArch64DeadRegisterDefinitionsPass.cpp
  AArch64ExpandImm.cpp
  AArch64ExpandPseudoInsts.cpp
  AArch64FalkorHWPFFix.cpp
  AArch64FastISel.cpp
  AArch64A53Fix835769.cpp
  AArch64FrameLowering.cpp
  AArch64CompressJumpTables.cpp
  AArch64ConditionOptimizer.cpp
  AArch64RedundantCopyElimination.cpp
  AArch64ISelDAGToDAG.cpp
  AArch64ISelLowering.cpp
  AArch64InstrInfo.cpp
  AArch64LoadStoreOptimizer.cpp
  AArch64LowerHomogeneousPrologEpilog.cpp
  AArch64MachineFunctionInfo.cpp
  AArch64MachineScheduler.cpp
  AArch64MacroFusion.cpp
  AArch64MIPeepholeOpt.cpp
  AArch64MCInstLower.cpp
  AArch64PromoteConstant.cpp
  AArch64PBQPRegAlloc.cpp
  AArch64RegisterInfo.cpp
  AArch64SLSHardening.cpp
  AArch64SelectionDAGInfo.cpp
  AArch64SpeculationHardening.cpp
  AArch64StackTagging.cpp
  AArch64StackTaggingPreRA.cpp
  AArch64StorePairSuppress.cpp
  AArch64Subtarget.cpp
  AArch64TargetMachine.cpp
  AArch64TargetObjectFile.cpp
  AArch64TargetTransformInfo.cpp
  SVEIntrinsicOpts.cpp
  AArch64SIMDInstrOpt.cpp

  DEPENDS
  intrinsics_gen

  LINK_COMPONENTS
  AArch64Desc
  AArch64Info
  AArch64Utils
  Analysis
  AsmPrinter
  CodeGen
  Core
  MC
  Scalar
  SelectionDAG
  Support
  Target
  TransformUtils
  GlobalISel
  CFGuard

  ADD_TO_COMPONENT
  AArch64
)

add_subdirectory(AsmParser)
add_subdirectory(Disassembler)
add_subdirectory(MCTargetDesc)
add_subdirectory(TargetInfo)
add_subdirectory(Utils)
