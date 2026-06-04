#===-- EmbeddedJIT light AArch64 backend (optional component) -------------===#
#
# 本文件不是一个独立的 add_subdirectory 目标，而是被上级
# llvm/lib/ExecutionEngine/EJIT/CMakeLists.txt 在
# EJIT_ENABLE_LIGHT_BACKEND=ON 时 include() 进来，向 LLVMEJIT 追加
# light 后端的源文件。
#
# 这样做的原因：LLVMEJIT 是单个 add_llvm_component_library，light 后端
# 只是它的一组可选 TU，没必要再拆成独立 LLVM component（避免引入额外的
# component 依赖图，符合 EJIT_LIBRARY_TRIMMING.md 的最小依赖原则）。
#
# light 后端只依赖 LLVM Core / Support（IR 遍历 + Error），不拉入任何
# AArch64 CodeGen / MC 组件——它自己手写指令编码。
#
set(EJIT_LIGHT_BACKEND_SOURCES
  ${CMAKE_CURRENT_LIST_DIR}/EJitLightAArch64.cpp
  ${CMAKE_CURRENT_LIST_DIR}/EJitLightCodeAllocator.cpp
  ${CMAKE_CURRENT_LIST_DIR}/EJitLightBackend.cpp
)
