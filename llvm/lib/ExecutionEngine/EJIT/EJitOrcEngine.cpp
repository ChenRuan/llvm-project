//===-- EJitOrcEngine.cpp - OrcJIT Engine Wrapper -------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitAtomic.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/ExecutionEngine/EJIT/EJitLibcallStubs.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptimizer.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorSymbolDef.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <map>
#include <string>

#ifdef EJIT_FREESTANDING
#include "llvm/ExecutionEngine/EJIT/EJitBareMetal.h"
#else
#include <mutex>
#endif

#ifdef EJIT_SRE_CODE_POOL
#include "llvm/ExecutionEngine/EJIT/EJitCodePoolMemoryManager.h"
#include "llvm/ExecutionEngine/EJIT/EJitSrePlatform.h"
#include "llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h"
#endif
#ifdef EJIT_SRE_SHARED_TASKPOOL
#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPoolState.h"
#endif

using namespace llvm;
using namespace llvm::ejit;

#define DEBUG_TYPE "ejit-orc-engine"

struct EJitOrcEngine::Impl {
#ifdef EJIT_SRE_CODE_POOL
  /// Dedicated 2MiB code pools backing all JIT machine code. Declared before
  /// J so it outlives the LLJIT (and the memory manager the object linking
  /// layer owns, which references it).
  std::unique_ptr<EJitCodePoolManager> codePool;
#endif
  std::unique_ptr<orc::LLJIT> J;
  PeriodArrayRegistry *periodReg = nullptr;
  EJitRuntimeState *runtimeState = nullptr;
  const SpecializationContext *activeCtx = nullptr;
  /// Per-specialization JITDylib pointers so each specialization is
  /// independently compiled and symbols from different specializations
  /// never conflict.
  std::map<uint64_t, orc::JITDylib *> specDylibs;
  /// User-registered symbols (functions + globals) for bare-metal.
  /// Populated via ejit_register_symbol() / addUserSymbol().
  std::map<std::string, void *> userSymbols;
  /// If non-empty, dump JIT-optimized IR to this directory.
  std::string dumpJITDir;
  /// Persistent optimizer — analysis managers are registered once and reused.
  std::unique_ptr<EJitOptimizer> optimizer;
  /// TargetMachine used for the name-filtered ASM diagnostic dump (created
  /// once from the same JITTargetMachineBuilder the JIT compiles with, so the
  /// emitted assembly matches the real JIT output). Null if creation failed.
  std::unique_ptr<TargetMachine> dumpTM;
};

namespace llvm {
namespace ejit {

// Mutex type for the dump store. On SRE/freestanding std::mutex is
// unavailable and BareMetalMutex is a no-op, so use a real CAS spinlock (built
// on the __atomic wrappers in EJitAtomic.h). gDumpStore is per-core (each core
// has its own process image, so there is no cross-core race on it), but a
// same-core overlap between the worker capture and a producer print must still
// be guarded. Hosted builds keep std::mutex. The spinlock has a trivial
// default constructor, so a static instance is zero-initialized (unlocked)
// with no dynamic initializer — important for freestanding.
#ifdef EJIT_FREESTANDING
namespace {
class DumpSpinLock {
public:
  void lock() {
    uint32_t expected = 0;
    while (!flag_.compareExchange(expected, 1u))
      expected = 0;
  }
  void unlock() { flag_.storeRelease(0u); }

private:
  EJitAtomicU32 flag_;
};
} // namespace
using DumpMutexType = DumpSpinLock;
#else
using DumpMutexType = std::mutex;
#endif

// Process-wide function-name filter for the IR+ASM diagnostic dump. Set via
// ejit_dump_func() / setDumpFuncFilter(). Read in the IR transform layer.
// A diagnostic: set before triggering the compile of interest; concurrent
// set/read is benign (worst case a missed or extra dump).
#ifdef EJIT_SRE_SHARED_TASKPOOL
EJitSharedTaskPoolState *gDumpSharedState = nullptr;
#endif
static std::string gDumpFuncFilter;

void setDumpFuncFilter(const std::string &name) {
  gDumpFuncFilter = name;
  EJIT_DIAG_DEBUG("set_dump_filter value=%s &filter=%p",
            gDumpFuncFilter.empty() ? "(off)" : gDumpFuncFilter.c_str(),
            (void *)&gDumpFuncFilter);
#ifdef EJIT_SRE_SHARED_TASKPOOL
  if (gDumpSharedState) {
    EJitSharedDumpState &D = gDumpSharedState->dump;
    uint32_t expected = 0;
    while (!D.lock.compareExchange(expected, 1))
      expected = 0;
    uint32_t len = 0;
    if (!name.empty()) {
      while (len + 1 < kEJitSharedDumpNameBytes && len < name.size()) {
        D.filterName[len] = name[len];
        ++len;
      }
    }
    D.filterName[len] = 0;
    D.filterLen = len;
    D.resultValid.storeRelease(0);
    D.resultNameLen = 0;
    D.irSize = 0;
    D.asmSize = 0;
    D.truncated.storeRelease(0);
    D.filterEnabled.storeRelease(len ? 1u : 0u);
    D.lock.storeRelease(0);
    EJIT_DIAG_DEBUG("set_dump_filter shared enabled=%u len=%u &shared=%p",
              len ? 1u : 0u, len, (void *)gDumpSharedState);
  }
#endif
}

#ifdef EJIT_SRE_SHARED_TASKPOOL
void setDumpSharedState(EJitSharedTaskPoolState *state) {
  gDumpSharedState = state;
  EJIT_DIAG_DEBUG("set_dump_shared_state state=%p", (void *)state);
}

static void sharedDumpLock(EJitSharedDumpState &D) {
  uint32_t expected = 0;
  while (!D.lock.compareExchange(expected, 1))
    expected = 0;
}

static void sharedDumpUnlock(EJitSharedDumpState &D) {
  D.lock.storeRelease(0);
}

static bool getSharedDumpFilter(std::string &out) {
  if (!gDumpSharedState)
    return false;
  EJitSharedDumpState &D = gDumpSharedState->dump;
  sharedDumpLock(D);
  bool enabled = D.filterEnabled.loadAcquire() != 0;
  if (enabled) {
    uint32_t len = D.filterLen;
    if (len >= kEJitSharedDumpNameBytes)
      len = kEJitSharedDumpNameBytes - 1;
    out.assign(D.filterName, D.filterName + len);
  }
  sharedDumpUnlock(D);
  return enabled;
}
#else
void setDumpSharedState(EJitSharedTaskPoolState * /*state*/) {}
#endif

static bool getActiveDumpFilter(std::string &out) {
#ifdef EJIT_SRE_SHARED_TASKPOOL
  if (getSharedDumpFilter(out))
    return true;
#endif
  if (gDumpFuncFilter.empty())
    return false;
  out = gDumpFuncFilter;
  return true;
}

/// Saved IR+ASM for a captured specialization (latest per function name).
struct DumpEntry {
  uint64_t cacheKey = 0;
  std::string IR;
  std::string ASM;
};

// Process-wide store of captured IR+ASM, filled by the IR transform layer
// (worker thread) when the filter matches, read by ejit_print_dumped() (user
// thread). Guarded by gDumpMutex. These are ordinary process statics, not part
// of the shared taskpool state; cross-core visibility depends on the worker
// running in the same process image (addresses are logged to diagnose this).
static DumpMutexType gDumpMutex;
static std::map<std::string, DumpEntry> gDumpStore;

static void dumpBytesSafe(const char *label, const char *data, size_t n) {
  EJIT_DIAG("=== %s begin size=%u ===", label, (unsigned)n);
  size_t i = 0;
  unsigned lineNo = 0;
  while (i < n) {
    size_t lineEnd = i;
    while (lineEnd < n && data[lineEnd] != '\n')
      ++lineEnd;
    size_t pos = i;
    if (pos == lineEnd) {
      EJIT_DIAG("%s:%u: ", label, lineNo);
    } else {
      while (pos < lineEnd) {
        size_t chunk = lineEnd - pos;
        if (chunk > 180)
          chunk = 180;
        EJIT_DIAG("%s:%u: %.*s", label, lineNo, (int)chunk, data + pos);
        pos += chunk;
      }
    }
    ++lineNo;
    i = lineEnd + 1;
  }
  (void)lineNo;
  EJIT_DIAG("=== %s end lines=%u ===", label, lineNo);
}

/// Emit a multi-line blob (IR or ASM) through EJIT_DIAG. SRE-safe: no lambda,
/// no Twine, no raw_fd_ostream. Splits on '\n' and chunks each line to a small
/// fixed width so a single EJIT_DIAG/SRE_printf call stays bounded.
static void dumpLinesSafe(const char *label, const std::string &s) {
  dumpBytesSafe(label, s.data(), s.size());
}

#ifdef EJIT_SRE_SHARED_TASKPOOL
static uint32_t copyDumpBytes(char *dst, uint32_t cap, const char *src,
                              size_t size, bool &truncated) {
  if (cap == 0)
    return 0;
  uint32_t n = 0;
  while (n + 1 < cap && n < size) {
    dst[n] = src[n];
    ++n;
  }
  dst[n] = 0;
  truncated = size >= cap;
  return n;
}

static void captureSharedDump(const std::string &fnName, uint64_t cacheKey,
                              const std::string &IR, const std::string &ASM) {
  if (!gDumpSharedState)
    return;
  EJitSharedDumpState &D = gDumpSharedState->dump;
  sharedDumpLock(D);
  bool nameTrunc = false;
  bool irTrunc = false;
  bool asmTrunc = false;
  D.resultValid.storeRelease(0);
  D.resultNameLen =
      copyDumpBytes(D.resultName, kEJitSharedDumpNameBytes, fnName.data(),
                    fnName.size(), nameTrunc);
  D.irSize =
      copyDumpBytes(D.ir, kEJitSharedDumpTextBytes, IR.data(), IR.size(),
                    irTrunc);
  D.asmSize = copyDumpBytes(D.asmText, kEJitSharedDumpTextBytes, ASM.data(),
                            ASM.size(), asmTrunc);
  D.keyHi = (uint32_t)(cacheKey >> 32);
  D.keyLo = (uint32_t)(cacheKey & 0xffffffffu);
  D.truncated.storeRelease((nameTrunc ? 4u : 0u) | (irTrunc ? 1u : 0u) |
                           (asmTrunc ? 2u : 0u));
  D.resultValid.storeRelease(1);
  sharedDumpUnlock(D);
  EJIT_DIAG_DEBUG("capture shared func=%s ir=%u asm=%u trunc=0x%x &shared=%p",
            fnName.c_str(), (unsigned)IR.size(), (unsigned)ASM.size(),
            (nameTrunc ? 4u : 0u) | (irTrunc ? 1u : 0u) |
                (asmTrunc ? 2u : 0u),
            (void *)gDumpSharedState);
}

static bool printSharedDumped(const char *name) {
  if (!gDumpSharedState)
    return false;
  EJitSharedDumpState &D = gDumpSharedState->dump;
  sharedDumpLock(D);
  bool valid = D.resultValid.loadAcquire() != 0;
  if (!valid) {
    EJIT_DIAG_DEBUG("print_dumped shared: nothing saved &shared=%p",
              (void *)gDumpSharedState);
    sharedDumpUnlock(D);
    return false;
  }
  bool hasName = name && name[0];
  bool match = true;
  if (hasName) {
    uint32_t i = 0;
    while (i < D.resultNameLen && name[i] && name[i] == D.resultName[i])
      ++i;
    match = (i == D.resultNameLen && name[i] == 0);
  }
  if (!match) {
    EJIT_DIAG_DEBUG("print_dumped shared miss name=%s stored=%s ir=%u asm=%u",
              name ? name : "(null)", D.resultName, D.irSize, D.asmSize);
    sharedDumpUnlock(D);
    return false;
  }
  uint32_t trunc = D.truncated.loadAcquire();
  (void)trunc;
  EJIT_DIAG("print_dumped shared hit requested=%s stored=%s key_hi=0x%08x "
            "key_lo=0x%08x ir_size=%u asm_size=%u trunc=0x%x",
            hasName ? name : "(all)", D.resultName, D.keyHi, D.keyLo, D.irSize,
            D.asmSize, trunc);
  if (D.irSize)
    dumpBytesSafe("dump IR", D.ir, D.irSize);
  if (D.asmSize)
    dumpBytesSafe("dump ASM", D.asmText, D.asmSize);
  sharedDumpUnlock(D);
  return true;
}
#endif

/// Called from the IR transform layer when the filter matches: save the
/// post-optimization IR and emitted ASM for later selective printing.
static void captureDump(const std::string &fnName, uint64_t cacheKey,
                        std::string IR, std::string ASM) {
  EJIT_DIAG_DEBUG("capture enter func=%s ir_size=%u asm_size=%u &store=%p",
            fnName.c_str(), (unsigned)IR.size(), (unsigned)ASM.size(),
            (void *)&gDumpStore);
  std::lock_guard<DumpMutexType> lock(gDumpMutex);
  EJIT_DIAG_DEBUG("capture store_size before=%u", (unsigned)gDumpStore.size());
  gDumpStore[fnName] = DumpEntry{cacheKey, std::move(IR), std::move(ASM)};
  EJIT_DIAG_DEBUG("capture store_size after=%u", (unsigned)gDumpStore.size());
#ifdef EJIT_SRE_SHARED_TASKPOOL
  const DumpEntry &E = gDumpStore[fnName];
  captureSharedDump(fnName, cacheKey, E.IR, E.ASM);
#endif
}

/// Print one stored entry: header (name, key hi/lo, IR/ASM sizes) followed by
/// the IR and ASM bodies. SRE-safe: no Twine, no lambda, no temporary label.
static void printOneDumpSafe(const char *requestedName,
                             const std::string &storedName,
                             const DumpEntry &e) {
  uint32_t keyHi = (uint32_t)(e.cacheKey >> 32);
  uint32_t keyLo = (uint32_t)(e.cacheKey & 0xffffffffu);
  (void)keyHi;
  (void)keyLo;
  EJIT_DIAG("print_dumped hit requested=%s stored=%s key_hi=0x%08x "
            "key_lo=0x%08x ir_size=%u asm_size=%u",
            requestedName ? requestedName : "(all)", storedName.c_str(), keyHi,
            keyLo, (unsigned)e.IR.size(), (unsigned)e.ASM.size());
  if (!e.IR.empty())
    dumpLinesSafe("dump IR", e.IR);
  if (!e.ASM.empty())
    dumpLinesSafe("dump ASM", e.ASM);
}

/// Print the saved IR+ASM for \p name (or all saved entries when \p name is
/// null/empty) through EJIT_DIAG, one line per IR/ASM line. Names with no
/// saved capture are reported as missing.
void printDumped(const char *name) {
  EJIT_DIAG_DEBUG("print_dumped enter name=%s &filter=%p &store=%p",
            (name && name[0]) ? name : "(all)", (void *)&gDumpFuncFilter,
            (void *)&gDumpStore);
  bool hasName = name && name[0];
#ifdef EJIT_SRE_SHARED_TASKPOOL
  // A specific name may have been captured on another core: try the shared
  // single-result first. For "print all" (name NULL/empty) skip the shared
  // path — it holds only the latest capture — and iterate the local store,
  // which keeps one entry per function name (every dump-all capture).
  if (hasName && printSharedDumped(name))
    return;
#endif
  std::lock_guard<DumpMutexType> lock(gDumpMutex);
  EJIT_DIAG_DEBUG("print_dumped store_size=%u", (unsigned)gDumpStore.size());
  if (hasName) {
    auto it = gDumpStore.find(name);
    if (it != gDumpStore.end()) {
      printOneDumpSafe(name, it->first, it->second);
    } else {
      EJIT_DIAG_DEBUG("print_dumped miss name=%s store_size=%u", name,
                (unsigned)gDumpStore.size());
      unsigned idx = 0;
      for (auto &kv : gDumpStore) {
        (void)kv;
        EJIT_DIAG_DEBUG("stored[%u]=%s ir=%u asm=%u", idx, kv.first.c_str(),
                  (unsigned)kv.second.IR.size(),
                  (unsigned)kv.second.ASM.size());
        ++idx;
      }
      (void)idx;
    }
  } else {
    if (gDumpStore.empty())
      EJIT_DIAG("print_dumped: nothing saved");
    for (auto &kv : gDumpStore)
      printOneDumpSafe(nullptr, kv.first, kv.second);
  }
}

} // namespace ejit
} // namespace llvm

EJitOrcEngine::EJitOrcEngine() : P(std::make_unique<Impl>()) {}
EJitOrcEngine::~EJitOrcEngine() = default;

Expected<std::unique_ptr<EJitOrcEngine>>
EJitOrcEngine::Create(const Config &config,
                      PeriodArrayRegistry &periodReg,
                      EJitRuntimeState &runtimeState) {
  EJIT_DIAG_VERBOSE("create: opt=%d dump=%s",
                    static_cast<int>(config.optLevel),
                    config.dumpJITDir.empty() ? "(off)" : config.dumpJITDir.c_str());
  auto engine = std::unique_ptr<EJitOrcEngine>(new EJitOrcEngine());
  engine->P->periodReg = &periodReg;
  engine->P->runtimeState = &runtimeState;
  engine->P->dumpJITDir = config.dumpJITDir;

  // Bare-metal / cross-compiled: use compile-time target triple.
  // Native host: auto-detect via detectHost().
#if defined(EJIT_DEFAULT_TRIPLE) || defined(EJIT_FREESTANDING)
  #ifdef EJIT_DEFAULT_TRIPLE
    Expected<orc::JITTargetMachineBuilder> JTMBOrErr(
        orc::JITTargetMachineBuilder(Triple(EJIT_DEFAULT_TRIPLE)));
  #else
    #error EJIT_FREESTANDING requires EJIT_DEFAULT_TRIPLE to be set
  #endif
#else
  auto JTMBOrErr = orc::JITTargetMachineBuilder::detectHost();
#endif
  if (!JTMBOrErr) {
    EJIT_DIAG("create FAIL: target machine builder error");
    return JTMBOrErr.takeError();
  }

  // Use Large code model so JITLink can generate 64-bit absolute relocations.
  JTMBOrErr->setCodeModel(CodeModel::Large);

  // Build a TargetMachine (same options the JIT compiles with) for the
  // name-filtered ASM diagnostic dump. Failure is non-fatal — the dump is
  // simply unavailable.
  if (auto TMOrErr = JTMBOrErr->createTargetMachine())
    engine->P->dumpTM = std::move(*TMOrErr);
  else
    consumeError(TMOrErr.takeError());

  orc::LLJITBuilder Builder;
  Builder.setJITTargetMachineBuilder(*JTMBOrErr);
  Builder.setNumCompileThreads(0);
// Bare-metal: skip host process symbol search (avoids dlopen/dlsym),
// and skip ORC runtime injection / EH frames / atexit / global ctors.
#ifdef EJIT_FREESTANDING
  Builder.setLinkProcessSymbolsByDefault(false);
  Builder.setPlatformSetUp(orc::setUpInactivePlatform);
#endif

#ifdef EJIT_SRE_CODE_POOL
  // Route JIT machine-code memory through EmbeddedJIT's own 2MiB pools instead
  // of the default JITLink mmap/mprotect path. The pool manager is owned by the
  // engine (so it outlives the LLJIT); the object linking layer owns a memory
  // manager that references it. Pages are kept RW here and sealed to RX later,
  // at lookup time, by the pool manager's enable_ex sealing.
  engine->P->codePool = makeSreCodePoolManager();
  {
    EJitCodePoolManager *Pool = engine->P->codePool.get();
    Builder.setObjectLinkingLayerCreator(
        [Pool](orc::ExecutionSession &ES)
            -> Expected<std::unique_ptr<orc::ObjectLayer>> {
          // Page size only affects per-segment layout padding; we never apply
          // per-segment protections (sealing is done per 2MiB pool), so a
          // conservative 4KiB is sufficient and portable.
          constexpr size_t JitPageSize = 4096;
          return std::make_unique<orc::ObjectLinkingLayer>(
              ES, std::make_unique<EJitCodePoolMemoryManager>(*Pool,
                                                              JitPageSize));
        });
  }
#endif

  auto J = Builder.create();
  if (!J) {
    EJIT_DIAG("create FAIL: LLJIT builder error");
    return J.takeError();
  }

  engine->P->J = std::move(*J);

  // Override the default error reporter (logErrorsToStdErr → errs() →
  // raw_fd_ostream) with a bare-metal-safe version using EJIT_DIAG.  On
  // SRE / bare-metal the default reporter crashes because raw_fd_ostream
  // internally calls POSIX I/O (open / write / isatty) whose GOT/PLT
  // entries may be unmapped.  EJIT_DIAG uses SRE_printf / std::printf
  // which are always available on the target.
  engine->P->J->getExecutionSession().setErrorReporter(
      [](Error Err) {
        EJIT_DIAG("JIT error: %s", toString(std::move(Err)).c_str());
      });

  // Create persistent optimizer — analysis managers are registered once here
  // and reused across compilations (cleared between runs).
  engine->P->optimizer = std::make_unique<EJitOptimizer>(periodReg);

  // Register all known global variable addresses from the PeriodArrayRegistry
  // so that external global references in any loaded bitcode module resolve
  // to the AOT process's memory. Deduplicate: the constructor may run twice
  // (PASS1 + PASS2 both add to global_ctors), causing duplicate entries.
  {
    auto &JD = engine->P->J->getMainJITDylib();
    orc::SymbolMap symMap;
    for (auto &kv : periodReg.getStaticVars())
      symMap[engine->P->J->mangleAndIntern(kv.varName)] =
          orc::ExecutorSymbolDef(orc::ExecutorAddr::fromPtr(kv.varAddr),
                                 JITSymbolFlags::Exported);
    if (!symMap.empty()) {
      size_t n = symMap.size();
      (void)n;
      if (auto Err = JD.define(orc::absoluteSymbols(std::move(symMap))))
        EJIT_DIAG("create: define %zu static var(s) FAILED: %s", n,
                  toString(std::move(Err)).c_str());
    }
  }
  EJIT_DIAG_VERBOSE("create: static vars registered=%zu",
                    periodReg.getStaticVars().size());

  // Set up IR transform layer: runs the specialization pipeline during
  // JIT compilation (parameter substitution → InstCombine → StructFieldPass
  // → core optimization pipeline).
  engine->P->J->getIRTransformLayer().setTransform(
      [engine = engine.get()](
          orc::ThreadSafeModule TSM,
          const orc::MaterializationResponsibility &R)
          -> Expected<orc::ThreadSafeModule> {
        TSM.withModuleDo([engine](Module &M) {
          LLVM_DEBUG(dbgs() << "ejit-orc-engine: JIT transform on "
                            << M.getName() << "\n");
          const SpecializationContext *ctx = engine->P->activeCtx;
          if (!ctx)
            return;

          // Clear stale analysis results from previous compilations
          // (each compilation uses a fresh Module with new IR unit pointers).
          engine->P->optimizer->clearAnalyses();

          // Dump pre-optimization IR (before the JIT pipeline runs).
          if (!engine->P->dumpJITDir.empty()) {
            std::string prePath = engine->P->dumpJITDir + "/" +
                                  ctx->fnName + "_" +
                                  std::to_string(ctx->cacheKey) + "_pre.ll";
            std::error_code EC;
            llvm::raw_fd_ostream preOS(prePath, EC);
            if (!EC)
              M.print(preOS, nullptr);
          }

          engine->P->optimizer->runPipeline(M, *ctx);

          // Dump post-optimization IR.
          if (!engine->P->dumpJITDir.empty()) {
            std::string path = engine->P->dumpJITDir + "/" +
                               ctx->fnName + "_" +
                               std::to_string(ctx->cacheKey) + "_opt.ll";
            std::error_code EC;
            llvm::raw_fd_ostream OS(path, EC);
            if (!EC)
              M.print(OS, nullptr);
          }

          // Name-filtered IR+ASM capture for later selective printing. Filter
          // set via ejit_dump_func(); captured entries printed on demand via
          // ejit_print_dumped(). Bare-metal-safe (strings only, no
          // raw_fd_ostream). Captures the post-optimization IR and the emitted
          // assembly (from the same TargetMachine the JIT compiles with).
          // The filter value "*" is a wildcard: every specialization is captured
          // (ejit_dump_all(true) sets it). The local gDumpStore keeps one entry
          // per function name (overwritten on re-compile), so dump-all is
          // bounded by the number of distinct entry functions; the cross-core
          // shared result keeps only the latest capture (see ejit_print_dumped).
          {
            std::string DumpFilter;
            bool hasFilter = getActiveDumpFilter(DumpFilter);
            bool wildcard = hasFilter && DumpFilter == "*";
            bool match = wildcard || (hasFilter && ctx->fnName == DumpFilter);
            EJIT_DIAG_DEBUG("dump check filter=%s fn=%s key_hi=0x%08x "
                            "key_lo=0x%08x match=%d wildcard=%d &filter=%p",
                            hasFilter ? DumpFilter.c_str() : "(off)",
                            ctx->fnName.c_str(), (uint32_t)(ctx->cacheKey >> 32),
                            (uint32_t)(ctx->cacheKey & 0xffffffffu), match ? 1 : 0,
                            wildcard ? 1 : 0, (void *)&gDumpFuncFilter);
            if (match) {
              // IR capture always runs first so it succeeds even if the ASM
              // diagnostic path is disabled or fails.
              std::string IR;
              raw_string_ostream IOS(IR);
              M.print(IOS, nullptr);
              IOS.flush();

              std::string Asm;
              // ASM dump drives the LLVM assembly emitter, whose InstPrinter
              // formats fields into AsmBuf via C-library snprintf/vsnprintf.
              // On SRE those must be functional or the emit crashes in
              // raw_ostream::operator<<(format_object_base). Link
              // ejit_test/stubs/ejit_sre_format_stubs.cpp into the SRE/lipo
              // image, or provide an equivalent platform vsnprintf.
              if (engine->P->dumpTM) {
                EJIT_DIAG_DEBUG("dump asm begin fn=%s", ctx->fnName.c_str());
                SmallVector<char, 0> AsmBuf;
                raw_svector_ostream AOS(AsmBuf);
                legacy::PassManager PM;
                if (!engine->P->dumpTM->addPassesToEmitFile(
                        PM, AOS, /*DwoOut=*/nullptr,
                        CodeGenFileType::AssemblyFile)) {
                  EJIT_DIAG_DEBUG("dump asm PM.run begin fn=%s", ctx->fnName.c_str());
                  // Clone M before running codegen so this diagnostic ASM emit
                  // path cannot perturb the live module handed back to the JIT
                  // for real compilation (codegen is IR-read-only in theory,
                  // but target passes are not guaranteed to never touch IR).
                  // The clone is local to this diagnostic path and discarded.
                  std::unique_ptr<Module> MClone = CloneModule(M);
                  PM.run(*MClone);
                  EJIT_DIAG_DEBUG("dump asm PM.run end fn=%s", ctx->fnName.c_str());
                  Asm.assign(AsmBuf.begin(), AsmBuf.end());
                  EJIT_DIAG_DEBUG("dump asm size=%u fn=%s", (unsigned)Asm.size(),
                            ctx->fnName.c_str());
                } else {
                  EJIT_DIAG_DEBUG("dump asm addPassesToEmitFile failed fn=%s",
                            ctx->fnName.c_str());
                }
              }
              captureDump(ctx->fnName, ctx->cacheKey, std::move(IR),
                          std::move(Asm));
            }
          }
        });
        return std::move(TSM);
      });

  EJIT_DIAG("create OK: LLJIT ready");
  return engine;
}

Error EJitOrcEngine::loadBitcodeModule(StringRef bitcodeData,
                                       uint64_t cacheKey,
                                       const std::string &origFnName) {
  EJIT_DIAG_VERBOSE("loadBitcode key=0x%016lx func=%s size=%zu", cacheKey,
                    origFnName.c_str(), bitcodeData.size());
  auto Ctx = std::make_unique<LLVMContext>();
  auto Buf = MemoryBuffer::getMemBuffer(
      bitcodeData, ("spec_" + std::to_string(cacheKey) + ".bc"));
  auto ModuleOrErr = parseBitcodeFile(Buf->getMemBufferRef(), *Ctx);
  if (!ModuleOrErr) {
    EJIT_DIAG("loadBitcode FAIL key=0x%016lx: parse bitcode error", cacheKey);
    return ModuleOrErr.takeError();
  }

  Triple TT((*ModuleOrErr)->getTargetTriple());
  if (TT.isAArch64() && TT.isOSBinFormatELF()) {
    // These declarations resolve to process addresses, not co-located JIT
    // storage. Clearing dso_local forces AArch64 PIC codegen to use GOT/PLT
    // style indirection instead of near-page ADRP relocations.
    for (Function &F : (*ModuleOrErr)->functions()) {
      if (F.isDeclaration() && !F.isIntrinsic())
        F.setDSOLocal(false);
    }
    for (GlobalVariable &GV : (*ModuleOrErr)->globals()) {
      if (GV.isDeclaration())
        GV.setDSOLocal(false);
    }
  }

  // ejit_entry functions may have internal linkage (e.g. declared `static` in
  // source). ORC's IR layer excludes local-linkage symbols from the JITDylib
  // symbol table (Layer.cpp: hasLocalLinkage() skip), so a static entry is
  // invisible to lookup ("symbol not found", no materialization). The function
  // being compiled (origFnName) is the JIT lookup target — force it to
  // external linkage so ORC registers and can materialize it. Spec JITDylibs
  // are isolated, so this cannot collide with other specializations.
  if (Function *EntryF = (*ModuleOrErr)->getFunction(origFnName))
    if (!EntryF->isDeclaration() && EntryF->hasLocalLinkage())
      EntryF->setLinkage(GlobalValue::ExternalLinkage);

  // Collect global variable addresses from the registry for symbols
  // that appear as external declarations in the bitcode module.
  orc::SymbolMap globalSymbols;
  for (GlobalVariable &GV : (*ModuleOrErr)->globals()) {
    if (!GV.isDeclaration() || GV.getName().empty())
      continue;
    void *addr = nullptr;
    if (const auto *info = P->periodReg->getArrayInfo(GV.getName().str()))
      addr = info->baseAddr;
    else
      addr = P->periodReg->getStaticVarAddr(GV.getName().str());
    if (!addr)
      continue;
    globalSymbols[P->J->mangleAndIntern(GV.getName())] =
        orc::ExecutorSymbolDef(orc::ExecutorAddr::fromPtr(addr),
                               JITSymbolFlags::Exported);
  }

  // Each specialization gets its own JITDylib so that symbols from
  // different specializations (same TU bitcode loaded multiple times)
  // never conflict. Remove any stale JD from a previous compilation
  // of the same cacheKey (e.g., after ejit_clear_cache).
  auto it = P->specDylibs.find(cacheKey);
  if (it != P->specDylibs.end()) {
    if (auto Err = P->J->getExecutionSession().removeJITDylib(*it->second))
      EJIT_DIAG("loadBitcode key=0x%016lx: remove stale JD FAILED: %s",
                cacheKey, toString(std::move(Err)).c_str());
    P->specDylibs.erase(it);
  }

  auto JDOrErr = P->J->createJITDylib("spec_" + std::to_string(cacheKey));
  if (!JDOrErr) {
    EJIT_DIAG("loadBitcode FAIL key=0x%016lx: create JITDylib error", cacheKey);
    return JDOrErr.takeError();
  }

  // Resolve undefined function symbols from user-registered table.
  // Required for bare-metal where dynamic lookup (dlsym) is unavailable.
  // Throttle diagnostics: tally unresolved externals and emit a single
  // summary line per load (below) instead of one line per symbol; the
  // individual names are listed at DEBUG for regression triage.
  size_t unresolvedFuncs = 0;
  size_t unresolvedGlobals = 0;
  SmallVector<std::string, 16> unresolvedNames;
  static constexpr size_t kMaxUnresolvedNames = 32;
  for (Function &F : (*ModuleOrErr)->functions()) {
    if (!F.isDeclaration() || F.getName().empty())
      continue;
    std::string name = F.getName().str();
    if (globalSymbols.count(P->J->mangleAndIntern(name)))
      continue;
    auto it = P->userSymbols.find(name);
    if (it == P->userSymbols.end()) {
      if (!F.isIntrinsic()) {
        ++unresolvedFuncs;
        if (unresolvedNames.size() < kMaxUnresolvedNames)
          unresolvedNames.push_back("f:" + name);
      }
      continue;
    }
    globalSymbols[P->J->mangleAndIntern(name)] =
        orc::ExecutorSymbolDef(orc::ExecutorAddr::fromPtr(it->second),
                               JITSymbolFlags::Exported);
  }
  for (GlobalVariable &GV : (*ModuleOrErr)->globals()) {
    if (!GV.isDeclaration() || GV.getName().empty())
      continue;
    std::string name = GV.getName().str();
    if (globalSymbols.count(P->J->mangleAndIntern(name)))
      continue;
    auto it = P->userSymbols.find(name);
    if (it == P->userSymbols.end()) {
      ++unresolvedGlobals;
      if (unresolvedNames.size() < kMaxUnresolvedNames)
        unresolvedNames.push_back("g:" + name);
      continue;
    }
    globalSymbols[P->J->mangleAndIntern(name)] =
        orc::ExecutorSymbolDef(orc::ExecutorAddr::fromPtr(it->second),
                               JITSymbolFlags::Exported);
  }
  if (unresolvedFuncs || unresolvedGlobals)
    EJIT_DIAG("loadBitcode: %zu unresolved external(s) not registered "
              "(%zu funcs, %zu globals)",
              unresolvedFuncs + unresolvedGlobals, unresolvedFuncs,
              unresolvedGlobals);
  EJIT_DIAG_DEBUG("loadBitcode: %zu unresolved name(s) listed (of %zu total):",
                  unresolvedNames.size(),
                  unresolvedFuncs + unresolvedGlobals);
  for (const std::string &n : unresolvedNames)
    EJIT_DIAG_DEBUG("  %s", n.c_str());

  // Provide codegen-synthesized runtime symbols (memset/memcpy/memmove/memcmp
  // and the stack-protector guard/fail) that the AOT symbol collector cannot
  // register because they are never present as IR declarations — the JIT
  // back-end lowers the llvm.mem* intrinsics and -fstack-protector attributes
  // into references to them. On freestanding targets process-symbol lookup is
  // disabled, so without these every JIT compilation fails at link time.
  // They go into the spec JITDylib (which is isolated: it does not link back
  // to the main JITDylib) so each specialization resolves them locally.
  for (const LibcallSymbol &LCS : getLibcallSymbols())
    globalSymbols[P->J->mangleAndIntern(LCS.name)] =
        orc::ExecutorSymbolDef(orc::ExecutorAddr::fromPtr(LCS.addr),
                               JITSymbolFlags::Exported);

  // Define all collected symbols in the spec JITDylib before loading the
  // IR module so the JIT linker can resolve external references.
  if (!globalSymbols.empty()) {
    size_t nGlobals = globalSymbols.size();
    (void)nGlobals;
    if (auto Err = JDOrErr->define(
            orc::absoluteSymbols(std::move(globalSymbols))))
      EJIT_DIAG("loadBitcode key=0x%016lx: define %zu global(s) FAILED: %s",
                cacheKey, nGlobals, toString(std::move(Err)).c_str());
  }

  if (auto Err = P->J->addIRModule(*JDOrErr,
      orc::ThreadSafeModule(std::move(*ModuleOrErr), std::move(Ctx)))) {
    EJIT_DIAG("loadBitcode FAIL key=0x%016lx: add IR module error", cacheKey);
    return Err;
  }

  P->specDylibs[cacheKey] = &*JDOrErr;
  EJIT_DIAG_VERBOSE("loadBitcode OK key=0x%016lx func=%s", cacheKey,
                    origFnName.c_str());
  return Error::success();
}

Expected<void *> EJitOrcEngine::lookup(uint64_t cacheKey,
                                       const std::string &name) {
  auto it = P->specDylibs.find(cacheKey);
  if (it == P->specDylibs.end()) {
    EJIT_DIAG("lookup FAIL key=0x%016lx name=%s: no spec JITDylib", cacheKey,
              name.c_str());
    return make_error<StringError>(
        "No specialization JITDylib found for: " + std::to_string(cacheKey),
        inconvertibleErrorCode());
  }

  auto addr = P->J->lookup(*it->second, name);
  if (!addr) {
    EJIT_DIAG("lookup FAIL key=0x%016lx name=%s: symbol not found", cacheKey,
              name.c_str());
    return addr.takeError();
  }
  void *Ptr = reinterpret_cast<void *>(addr->getValue());

#ifdef EJIT_SRE_CODE_POOL
  // Legacy whole-pool seal: flip the 2MiB pool that contains the resolved
  // function to RX before it is handed back. This is the only point a JIT pool
  // transitions RW->RX in whole-pool mode. Idempotent: a pool already sealed
  // (e.g. on allocation rollover) is not re-flipped, so repeated lookups of the
  // same function do not re-invoke enable_ex. Only pool-backed code is sealed;
  // an address resolved outside the pools (e.g. a process/absolute symbol) is
  // left untouched. If sealing fails we must not return a callable pointer.
  //
  // In 4K page-seal mode the seal already happened per-page at finalize (in the
  // code-pool memory manager), so nothing is done here.
  if (P->codePool && !P->codePool->usesPageSeal() &&
      P->codePool->contains(Ptr)) {
    if (auto Err = P->codePool->sealPoolContaining(Ptr)) {
      EJIT_DIAG("lookup FAIL key=0x%016lx ptr=%p: seal pool error", cacheKey,
                Ptr);
      return std::move(Err);
    }
  }
#endif

  EJIT_DIAG_VERBOSE("lookup OK key=0x%016lx name=%s ptr=%p", cacheKey,
                    name.c_str(), Ptr);
  return Ptr;
}

void EJitOrcEngine::setActiveContext(const SpecializationContext *ctx) {
  P->activeCtx = ctx;
}

const SpecializationContext *EJitOrcEngine::getActiveContext() const {
  return P->activeCtx;
}


void EJitOrcEngine::addUserSymbol(const std::string &name, void *addr) {
  P->userSymbols[name] = addr;
}

#ifdef EJIT_SRE_CODE_POOL
EJitCodePoolManager::Stats EJitOrcEngine::getCodePoolStats() const {
  if (P->codePool)
    return P->codePool->getStats();
  return EJitCodePoolManager::Stats{};
}

bool EJitOrcEngine::findCodeRange(const void *FnPtr,
                                  EJitCompiledCodeInfo &Out) const {
  if (!P->codePool) {
    EJIT_DIAG("findCodeRange FAIL: no code pool (fnPtr=%p)", FnPtr);
    return false;
  }
  return P->codePool->findRange(FnPtr, Out);
}
#endif
