add_llvm_component_library(LLVMProfileData
  GCOV.cpp
  InstrProf.cpp
  InstrProfCorrelator.cpp
  InstrProfReader.cpp
  InstrProfWriter.cpp
  MemProf.cpp
  ProfileSummaryBuilder.cpp
  SampleProf.cpp
  SampleProfReader.cpp
  SampleProfWriter.cpp
  RawMemProfReader.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/ProfileData

  DEPENDS
  intrinsics_gen

  LINK_COMPONENTS
  Core
  Object
  Support
  Demangle
  Symbolize
  DebugInfoDWARF
  )

option(LLVM_PROFILEDATA_DISABLE_CORRELATOR
  "Disable InstrProfCorrelator DWARF-based profile correlation (embedded JIT builds)" OFF)
if(LLVM_PROFILEDATA_DISABLE_CORRELATOR)
  target_compile_definitions(LLVMProfileData PRIVATE LLVM_PROFILEDATA_DISABLE_CORRELATOR=1)
endif()

add_subdirectory(Coverage)
