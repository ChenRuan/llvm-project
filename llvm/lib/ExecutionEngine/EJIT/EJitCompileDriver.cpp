//===-- EJitCompileDriver.cpp - Compilation Scheduler ---------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitCompileDriver.h"
#ifndef EJIT_FREESTANDING
#include "llvm/ExecutionEngine/EJIT/EJitLogger.h"
#endif
#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#ifndef EJIT_FREESTANDING
#include <chrono>
#endif

#ifdef EJIT_LIGHT_BACKEND
#include "llvm/ExecutionEngine/EJIT/EJitOptimizer.h"
#include "llvm/ExecutionEngine/EJIT/LightBackend/EJitLightBackend.h"
#include "llvm/ExecutionEngine/EJIT/LightBackend/EJitLightCodeAllocator.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/MemoryBuffer.h"
#include <vector>
#endif

using namespace llvm;
using namespace llvm::ejit;

EJitCompileDriver::EJitCompileDriver(const Config &config,
                                     EJitCache &cache,
                                     PeriodArrayRegistry &periodReg,
                                     EJitRuntimeState &runtimeState,
                                     EJitModuleLoader &loader,
                                     EJitLogger *logger)
    : config_(config), cache_(cache), periodReg_(periodReg),
  runtimeState_(runtimeState), loader_(loader)
#ifndef EJIT_FREESTANDING
  , logger_(logger)
#endif
{}

EJitCompileDriver::~EJitCompileDriver() = default;

void EJitCompileDriver::setSyncEngine(std::unique_ptr<EJitOrcEngine> engine) {
  syncEngine_ = std::move(engine);
}

void EJitCompileDriver::registerSymbol(const std::string &name, void *addr) {
  if (syncEngine_)
    syncEngine_->addUserSymbol(name, addr);
#ifdef EJIT_LIGHT_BACKEND
  lightUserSymbols_[name] = addr;
#endif
}

void *EJitCompileDriver::getOrCompile(
    const std::string &funcName,
    const std::pair<std::string, uint8_t> *dims,
    unsigned count) {

  // Build cache key: uint32_t = funcIdx(16b) | dim[0..3](4x4b)
  uint16_t funcIdx = loader_.getFuncIndex(funcName);
  uint32_t cacheKey = EJitCache::buildCacheKey(funcIdx, dims, count);

  // Check cache
  if (void *cached = cache_.getOrNull(cacheKey))
    return cached;

  // Verify time-window state
  for (unsigned i = 0; i < count; ++i) {
    if (!runtimeState_.isActive(dims[i].first, dims[i].second)) {
#ifndef EJIT_FREESTANDING
      if (logger_)
        logger_->log(ErrorCode::TimeWindowNotActive,
                     "Time window not active for " + dims[i].first,
                     funcName, std::to_string(cacheKey));
#endif
      return nullptr;
    }
  }

  // Get bitcode
  auto bitcodeOrErr = loader_.getBitcode(funcName);
  if (!bitcodeOrErr) {
#ifndef EJIT_FREESTANDING
    if (logger_)
      logger_->log(ErrorCode::BitcodeNotFound,
                   "No bitcode for function", funcName, std::to_string(cacheKey));
#endif
    return nullptr;
  }

  std::string bitcode = bitcodeOrErr->str();

  // Build specialization context
  SpecializationContext ctx;
  ctx.fnName = funcName;
  ctx.cacheKey = cacheKey;
  ctx.optLevel = config_.optLevel;
  for (unsigned i = 0; i < count; ++i)
    ctx.dimensions.push_back({dims[i].first, dims[i].second});

#ifdef EJIT_LIGHT_BACKEND
  // Optional light backend path. Taken when explicitly selected. It is fully
  // self-contained (no ORC engine dependency), so it runs *before* the
  // syncEngine_ availability check below.
  if (config_.backendMode == BackendMode::Light ||
      config_.backendMode == BackendMode::Auto) {
    bool unsupported = false;
    void *lightPtr = tryCompileLight(funcName, cacheKey, bitcode, ctx,
                                     unsupported);
    if (lightPtr) {
      std::set<std::string> periodDeps;
      for (unsigned i = 0; i < count; ++i)
        periodDeps.insert(dims[i].first + "=" + std::to_string(dims[i].second));
      cache_.put(cacheKey, lightPtr, bitcode.size(), periodDeps);
      return lightPtr;
    }
    // Light path did not produce code.
    if (config_.backendMode == BackendMode::Light) {
      // Forced light: do NOT silently fall back to ORC.
#ifndef EJIT_FREESTANDING
      if (logger_)
        logger_->log(ErrorCode::CompilationFailed,
                     unsupported ? "Light backend: IR unsupported (forced light)"
                                 : "Light backend: emit failed (forced light)",
                     funcName, std::to_string(cacheKey));
#endif
      return nullptr;
    }
    // Auto mode: fall through to ORC.
#ifndef EJIT_FREESTANDING
    if (logger_)
      logger_->log(ErrorCode::CompilationFailed,
                   unsupported ? "Light backend: unsupported, falling back to ORC"
                               : "Light backend: failed, falling back to ORC",
                   funcName, std::to_string(cacheKey));
#endif
  }
#endif // EJIT_LIGHT_BACKEND

  // Sync compile
  if (!syncEngine_) {
#ifndef EJIT_FREESTANDING
    if (logger_)
      logger_->log(ErrorCode::NotActive,
                   "Sync engine not initialized", funcName,
                   std::to_string(cacheKey));
#endif
    return nullptr;
  }

#ifndef EJIT_FREESTANDING
  auto start = std::chrono::steady_clock::now();
#endif

  syncEngine_->setActiveContext(&ctx);

  // Load module with cacheKey as module ID and original funcName for
  // symbol renaming (each specialization gets a unique symbol).
  if (auto Err = syncEngine_->loadBitcodeModule(bitcode, cacheKey, funcName)) {
    syncEngine_->setActiveContext(nullptr);
#ifndef EJIT_FREESTANDING
    if (logger_)
      logger_->log(ErrorCode::CompilationFailed,
                   "Failed to load bitcode module", funcName, std::to_string(cacheKey));
#else
    consumeError(std::move(Err));
#endif
    return nullptr;
  }

  auto addrOrErr = syncEngine_->lookup(cacheKey, funcName);
  syncEngine_->setActiveContext(nullptr);

  if (!addrOrErr) {
#ifndef EJIT_FREESTANDING
    if (logger_)
      logger_->log(ErrorCode::CompilationFailed,
                   "Failed to look up compiled function", funcName, std::to_string(cacheKey));
#else
    consumeError(addrOrErr.takeError());
#endif
    return nullptr;
  }

  void *funcPtr = *addrOrErr;

  // Cache the result.
  // NOTE: codeSize is the bitcode size, not the compiled machine code size.
  // Getting the actual machine code size from LLJIT/JITLink requires
  // instrumenting the memory manager. For now, bitcode size serves as an
  // approximation for cache eviction decisions.
  std::set<std::string> periodDeps;
  for (unsigned i = 0; i < count; ++i)
    periodDeps.insert(dims[i].first + "=" + std::to_string(dims[i].second));

  cache_.put(cacheKey, funcPtr, bitcode.size(), periodDeps);

  return funcPtr;
}

#ifdef EJIT_LIGHT_BACKEND
void *EJitCompileDriver::tryCompileLight(const std::string &funcName,
                                         uint32_t cacheKey,
                                         const std::string &bitcode,
                                         const SpecializationContext &ctx,
                                         bool &unsupported) {
  unsupported = false;

  // 1. Parse bitcode into a fresh module/context. The emitted machine code is
  //    position-independent and embeds global addresses as immediates, so the
  //    module/context can be dropped right after emission.
  LLVMContext localCtx;
  auto Buf = MemoryBuffer::getMemBuffer(
      bitcode, "light_" + std::to_string(cacheKey) + ".bc");
  auto ModuleOrErr = parseBitcodeFile(Buf->getMemBufferRef(), localCtx);
  if (!ModuleOrErr) {
#ifndef EJIT_FREESTANDING
    std::string msg;
    handleAllErrors(ModuleOrErr.takeError(),
                    [&](const ErrorInfoBase &E) { msg = E.message(); });
    if (logger_)
      logger_->log(ErrorCode::CompilationFailed,
                   "Light backend: bitcode parse failed: " + msg, funcName,
                   std::to_string(cacheKey));
#else
    consumeError(ModuleOrErr.takeError());
#endif
    return nullptr;
  }
  std::unique_ptr<Module> M = std::move(*ModuleOrErr);

  // 2. Run the same SPEC4/PASS7 specialization pipeline the ORC transform
  //    layer uses, so the light path sees identical specialized IR.
  {
    EJitOptimizer opt(periodReg_);
    opt.runPipeline(*M, ctx);
  }

  // 3. Locate the specialized entry function.
  Function *F = M->getFunction(funcName);
  if (!F || F->isDeclaration()) {
    unsupported = true;
#ifndef EJIT_FREESTANDING
    if (logger_)
      logger_->log(ErrorCode::CompilationFailed,
                   "Light backend: specialized function not found: " + funcName,
                   funcName, std::to_string(cacheKey));
#endif
    return nullptr;
  }

  // 4. Collect global symbols (Task G): period array base addresses, static
  //    vars, and user-registered symbols, bound as absolute host addresses.
  std::vector<std::string> nameStorage;
  std::vector<void *> addrStorage;
  for (const auto &sv : periodReg_.getStaticVars()) {
    nameStorage.push_back(sv.varName);
    addrStorage.push_back(sv.varAddr);
  }
  for (const auto &kv : periodReg_.getAllArraysByPeriod()) {
    for (const auto &info : kv.second) {
      nameStorage.push_back(info.varName);
      addrStorage.push_back(info.baseAddr);
    }
  }
  for (const auto &us : lightUserSymbols_) {
    nameStorage.push_back(us.first);
    addrStorage.push_back(us.second);
  }
  std::vector<light::GlobalSymbol> globals;
  globals.reserve(nameStorage.size());
  for (size_t i = 0; i < nameStorage.size(); ++i) {
    if (nameStorage[i].empty() || !addrStorage[i])
      continue;
    globals.push_back(
        {nameStorage[i].c_str(), reinterpret_cast<uint64_t>(addrStorage[i])});
  }

  // 5. Emit via the AArch64 light backend.
  if (!lightAlloc_)
    lightAlloc_ = std::make_unique<light::HostMmapCodeAllocator>();

  light::CompileResult lr;
  auto ptrOrErr =
      light::compileAArch64Light(*F, globals, *lightAlloc_, &lr);
  if (ptrOrErr && *ptrOrErr)
    return *ptrOrErr;

  // No code produced. Consume any error and classify via CompileResult so
  // Auto mode can decide whether to fall back to ORC.
  if (!ptrOrErr) {
#ifndef EJIT_FREESTANDING
    std::string msg;
    handleAllErrors(ptrOrErr.takeError(),
                    [&](const ErrorInfoBase &E) { msg = E.message(); });
    if (lr.reason.empty())
      lr.reason = msg;
#else
    consumeError(ptrOrErr.takeError());
#endif
  }
  unsupported = (lr.status == light::Status::Unsupported ||
                 lr.status == light::Status::NotAArch64);
#ifndef EJIT_FREESTANDING
  if (logger_)
    logger_->log(ErrorCode::CompilationFailed,
                 "Light backend rejected: " + lr.reason, funcName,
                 std::to_string(cacheKey));
#endif
  return nullptr;
}
#endif // EJIT_LIGHT_BACKEND
