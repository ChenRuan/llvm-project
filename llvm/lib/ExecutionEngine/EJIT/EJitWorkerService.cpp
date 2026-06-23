//===-- EJitWorkerService.cpp - Standalone worker service singleton -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  Implementation of the standalone EmbeddedJIT worker service (see
//  EJitWorkerService.h and jit_design_doc/EJIT_WORKER_SERVICE_DLIB.md).
//
//  The service is a plain in-library singleton (a Meyers function-local static,
//  the same pattern the existing EJitFuncRegistry uses). Its storage lives in
//  the dlib's data/bss. Under platform "model A" (one shared dlib data/bss
//  instance for all cores) this single object IS the one shared service: one
//  worker, one queue, one cache, one ORC. No custom shared section, no owner
//  election, no cross-core shared-state blob.
//
//  Two backends share ONE control layer:
//    * production: owns an llvm::ejit::EJit and drives ejit->taskPool().
//    * EJIT_WORKER_SERVICE_TESTING: owns a standalone EJitTaskPool with a mock
//      compiler (deterministic, no ORC, no real codegen) so the control logic
//      can be unit-tested without LLVM/threads.
//
//===----------------------------------------------------------------------===//

#ifdef EJIT_WORKER_SERVICE

#ifndef EJIT_SRE_TASKPOOL
#error "EJIT_WORKER_SERVICE requires EJIT_SRE_TASKPOOL (one queue/worker/cache)"
#endif

#include "llvm/ExecutionEngine/EJIT/EJitWorkerService.h"

#include "llvm/ExecutionEngine/EJIT/EJitAtomic.h"
#include "llvm/ExecutionEngine/EJIT/EJitFuncRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitLifecycleRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitTaskPool.h"

#ifndef EJIT_WORKER_SERVICE_TESTING
#include "llvm/ExecutionEngine/EJIT/EJit.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptions.h"
#include "llvm/ExecutionEngine/EJIT/EJitRegistrationStore.h"
#endif

#include <cstring>
#include <memory>
#include <string>
#include <vector>

//===----------------------------------------------------------------------===//
// Diagnostics — zero-cost unless built with -DEJIT_SERVICE_DIAG_ENABLE. Args
// are not evaluated when disabled. Redirect SRE_printf to the device log on
// target.
//===----------------------------------------------------------------------===//
#ifdef EJIT_SERVICE_DIAG_ENABLE
extern "C" int SRE_printf(const char *fmt, ...);
#define EJIT_SVC_DIAG(...)                                                     \
  do {                                                                         \
    SRE_printf("[EJIT-SVC] %s:%d ", __func__, __LINE__);                       \
    SRE_printf(__VA_ARGS__);                                                   \
    SRE_printf("\n");                                                          \
  } while (0)
#else
#define EJIT_SVC_DIAG(...) ((void)0)
#endif

/// Worker stack size the service reports. The actual SRE task stack is sized by
/// EJitSreTask_sre.cpp with the same macro (1 MiB default, platform-tunable);
/// the host build uses std::thread's default stack. Reported for observability.
#ifndef EJIT_SRE_TASKPOOL_WORKER_STACK_SIZE
#define EJIT_SRE_TASKPOOL_WORKER_STACK_SIZE (1024u * 1024u)
#endif

using namespace llvm::ejit;

namespace {

//===----------------------------------------------------------------------===//
// Fingerprint helpers (FNV-1a) for idempotent / conflicting registration.
//===----------------------------------------------------------------------===//
constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

uint64_t fnvBytes(uint64_t h, const void *data, size_t n) {
  const auto *p = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= kFnvPrime;
  }
  return h;
}

uint64_t fnvScalar(uint64_t h, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    h ^= (v & 0xFFu);
    h *= kFnvPrime;
    v >>= 8;
  }
  return h;
}

uint64_t fnvStr(uint64_t h, const char *s) {
  if (!s)
    return fnvScalar(h, 0);
  return fnvBytes(h, s, std::strlen(s));
}

/// Payload fingerprint used to decide idempotent vs conflicting
/// re-registration. FUNC_INDEX / LIFECYCLE carry no payload (the assignment is
/// by name and always idempotent), so they fingerprint to a constant and never
/// "conflict".
uint64_t entryFingerprint(const ejit_service_reg_entry_t &e) {
  uint64_t h = kFnvOffset;
  h = fnvScalar(h, e.kind);
  switch (e.kind) {
  case EJIT_SERVICE_REG_BITCODE:
    h = fnvScalar(h, e.size);
    if (e.address && e.size)
      h = fnvBytes(h, e.address, static_cast<size_t>(e.size));
    break;
  case EJIT_SERVICE_REG_PERIOD_ARRAY:
    h = fnvScalar(h, reinterpret_cast<uintptr_t>(e.address));
    h = fnvScalar(h, e.size);
    h = fnvStr(h, e.name2);
    break;
  case EJIT_SERVICE_REG_STATIC_VAR:
  case EJIT_SERVICE_REG_SYMBOL:
    h = fnvScalar(h, reinterpret_cast<uintptr_t>(e.address));
    break;
  default:
    break;
  }
  return h;
}

bool isPayloadKind(uint32_t kind) {
  return kind == EJIT_SERVICE_REG_BITCODE ||
         kind == EJIT_SERVICE_REG_PERIOD_ARRAY ||
         kind == EJIT_SERVICE_REG_STATIC_VAR || kind == EJIT_SERVICE_REG_SYMBOL;
}

#ifdef EJIT_WORKER_SERVICE_TESTING
//===----------------------------------------------------------------------===//
// Test-only mock compiler: deterministic non-null pointer keyed by the request.
// No ORC, no codegen, no real bitcode needed.
//===----------------------------------------------------------------------===//
EJitAtomicU32 gMockCompileFail{0};
EJitAtomicU32 gMockWorkerStartFail{0};
EJitAtomicU64 gMockCompileCalls{0};

bool mockCompile(void * /*ctx*/, const EJitCompileRequest &req, void **outFn) {
  gMockCompileCalls.fetchAdd(1);
  if (gMockCompileFail.loadAcquire()) {
    *outFn = nullptr;
    return false;
  }
  uintptr_t key = 0x4A170000ull + (static_cast<uintptr_t>(req.funcIndex) << 8);
  if (req.numDims > 0)
    key += (req.dims[0].instanceId & 0xFFu);
  *outFn = reinterpret_cast<void *>(key);
  return true;
}
#endif

//===----------------------------------------------------------------------===//
// The service singleton.
//===----------------------------------------------------------------------===//
class WorkerService {
public:
  static WorkerService &instance() {
    static WorkerService S;
    return S;
  }

  ejit_service_state_t state() const {
    return static_cast<ejit_service_state_t>(state_.loadAcquire());
  }

  ejit_service_status_t init(const ejit_service_config_t *cfg);
  ejit_service_status_t shutdown();
  ejit_service_status_t registerModule(ejit_service_module_reg_t *m);
  ejit_service_status_t compileOrGet(uint32_t funcIndex,
                                     const ejit_service_dim_t *dims,
                                     uint32_t numDims, void *fallback,
                                     void **outFn, uint32_t *outBucket);
  ejit_service_status_t releaseRead(uint32_t bucket);
  uint32_t pollOne();
  uint32_t pollBudget(uint32_t maxItems);
  ejit_service_status_t setActive(const char *lifecycle, uint32_t instanceId,
                                  bool on);
  ejit_service_status_t freeCode(uint32_t funcIndex,
                                 const ejit_service_dim_t *dims,
                                 uint32_t numDims);
  void identity(ejit_service_identity_t *out) const;
  void diagnostics(ejit_service_diagnostics_t *out) const;

private:
  WorkerService() = default;
  WorkerService(const WorkerService &) = delete;
  WorkerService &operator=(const WorkerService &) = delete;

  bool casState(ejit_service_state_t expect, ejit_service_state_t next) {
    uint32_t e = static_cast<uint32_t>(expect);
    return state_.compareExchange(e, static_cast<uint32_t>(next));
  }
  void setState(ejit_service_state_t s) {
    state_.storeRelease(static_cast<uint32_t>(s));
  }

  bool buildBackend(const ejit_service_config_t &cfg);
  void destroyBackend();
  bool commissionWorker();

#ifdef EJIT_WORKER_SERVICE_TESTING
public:
  /// Test-only: return the process-global singleton to a pristine state so each
  /// gtest case starts from EJIT_SERVICE_STATE_UNINITIALIZED with zeroed
  /// counters (the singleton itself is never destroyed).
  void testFullReset() {
    destroyBackend();
    generation_ = 0;
    initCount_ = 0;
    workerStartCount_.storeRelease(0);
    workerTaskId_ = 0;
    regTable_.clear();
    moduleCount_ = 0;
    mode_ = EJIT_SERVICE_MODE_ASYNC;
    shareCodePointers_ = 0;
    setState(EJIT_SERVICE_STATE_UNINITIALIZED);
  }

private:
#endif

  // Lifecycle / identity.
  EJitAtomicU32 state_{EJIT_SERVICE_STATE_UNINITIALIZED};
  uint64_t generation_ = 0;
  uint64_t initCount_ = 0;
  EJitAtomicU64 workerStartCount_{0};
  uint64_t workerTaskId_ = 0;

  // Config snapshot.
  uint32_t mode_ = EJIT_SERVICE_MODE_ASYNC;
  uint32_t shareCodePointers_ = 0;

  // Registration table (touched only during the single-threaded setup phase,
  // before the worker is commissioned — no lock needed).
  struct RegRec {
    uint32_t kind;
    std::string name;
    uint64_t fp;
  };
  std::vector<RegRec> regTable_;
  uint32_t moduleCount_ = 0;

  // Backend: the one queue/cache/worker. pool_ points at the active taskpool.
  EJitTaskPool *pool_ = nullptr;
#ifdef EJIT_WORKER_SERVICE_TESTING
  std::unique_ptr<EJitTaskPool> testPool_;
#else
  std::unique_ptr<EJit> ejit_;
#endif
};

//===----------------------------------------------------------------------===//
// Backend construction
//===----------------------------------------------------------------------===//

bool WorkerService::buildBackend(const ejit_service_config_t &cfg) {
#ifdef EJIT_WORKER_SERVICE_TESTING
  // Standalone taskpool with a mock compiler — deterministic, no ORC.
  testPool_ = std::make_unique<EJitTaskPool>(EJIT_SRE_TASKPOOL_QUEUE_CAPACITY,
                                             /*autoStartWorker=*/false);
  if (!testPool_)
    return false;
  testPool_->setCompiler(&mockCompile, nullptr);
  testPool_->switchController().setMode(EJitCompileMode::Async);
  pool_ = testPool_.get();
  return true;
#else
  // Production: an Async EJit auto-starts the worker; a Sync EJit keeps the
  // worker stopped (we then turn the taskpool mode to Async ourselves so the
  // queue accepts requests that the caller / poll pump drains inline). Either
  // way the taskpool's compiler is wired to compileNow inside the driver, so
  // there is exactly one optimizer pipeline / one compileCold.
  Config c;
  c.compileMode = (cfg.mode == EJIT_SERVICE_MODE_ASYNC) ? CompileMode::Async
                                                        : CompileMode::Sync;
  if (cfg.optLevel >= 1 && cfg.optLevel <= 3)
    c.optLevel = static_cast<OptimizationLevel>(cfg.optLevel);
  if (cfg.maxCodeMemory)
    c.maxCodeMemory = static_cast<size_t>(cfg.maxCodeMemory);
  if (cfg.maxDataMemory)
    c.maxDataMemory = static_cast<size_t>(cfg.maxDataMemory);
  if (cfg.maxCacheEntries)
    c.maxCacheEntries = static_cast<size_t>(cfg.maxCacheEntries);
  if (cfg.maxCacheSize)
    c.maxCacheSize = static_cast<size_t>(cfg.maxCacheSize);
  c.enableLogger = cfg.enableLogger != 0;

  ejit_ = std::make_unique<EJit>(c);
  if (!ejit_)
    return false;
  if (ejit_->initFailed()) {
    EJIT_SVC_DIAG("backend init failed: code=%d", ejit_->initError().code);
    return false;
  }
  EJitTaskPool *tp = ejit_->taskPool();
  if (!tp)
    return false;
  // For SYNC / ASYNC_MANUAL we built a Sync EJit (worker stopped, taskpool
  // Off); flip the taskpool to Async so compileOrGet enqueues and the caller
  // drains.
  if (cfg.mode != EJIT_SERVICE_MODE_ASYNC)
    tp->switchController().setMode(EJitCompileMode::Async);
  pool_ = tp;
  return true;
#endif
}

void WorkerService::destroyBackend() {
  if (pool_)
    pool_->stopWorker(); // join the worker BEFORE tearing down ORC / cache.
  pool_ = nullptr;
#ifdef EJIT_WORKER_SERVICE_TESTING
  testPool_.reset();
#else
  ejit_.reset();
#endif
}

bool WorkerService::commissionWorker() {
  // SYNC and ASYNC_MANUAL run with NO background worker; the caller (or the
  // service's inline drain) pumps the queue. Only ASYNC commissions the single
  // background worker task.
  if (mode_ != EJIT_SERVICE_MODE_ASYNC)
    return true;

#ifdef EJIT_WORKER_SERVICE_TESTING
  if (gMockWorkerStartFail.loadAcquire())
    return false;
  if (!pool_->startWorker())
    return false;
#else
  // The Async EJit already started the worker during construction; confirm it.
  if (!pool_->isWorkerRunning())
    return false;
#endif
  workerStartCount_.fetchAdd(1);
  workerTaskId_ = static_cast<uint64_t>(workerStartCount_.loadRelaxed());
  return true;
}

//===----------------------------------------------------------------------===//
// init / shutdown
//===----------------------------------------------------------------------===//

ejit_service_status_t WorkerService::init(const ejit_service_config_t *cfg) {
  // Validate the (optional) config before touching any state.
  ejit_service_config_t local;
  std::memset(&local, 0, sizeof(local));
  local.abiVersion = EJIT_SERVICE_ABI_VERSION;
  local.structSize = sizeof(ejit_service_config_t);
  local.mode = EJIT_SERVICE_MODE_ASYNC;
  local.optLevel = 2;
  local.enableLogger = 1;
  if (cfg) {
    if (cfg->abiVersion != EJIT_SERVICE_ABI_VERSION)
      return EJIT_SERVICE_ERR_ABI_MISMATCH;
    if (cfg->structSize != sizeof(ejit_service_config_t))
      return EJIT_SERVICE_ERR_ABI_MISMATCH;
    if (cfg->mode > EJIT_SERVICE_MODE_ASYNC_MANUAL)
      return EJIT_SERVICE_ERR_INVALID_PARAM;
    local = *cfg;
  }

  // Idempotent re-affirmation: a service that is already Ready stays Ready.
  ejit_service_state_t cur = state();
  if (cur == EJIT_SERVICE_STATE_READY) {
    ++initCount_;
    EJIT_SVC_DIAG("init: already Ready (idempotent), count=%llu",
                  (unsigned long long)initCount_);
    return EJIT_SERVICE_OK;
  }

  // Claim the Initializing transition. UNINITIALIZED or STOPPED may (re)init.
  if (!casState(EJIT_SERVICE_STATE_UNINITIALIZED,
                EJIT_SERVICE_STATE_INITIALIZING) &&
      !casState(EJIT_SERVICE_STATE_STOPPED, EJIT_SERVICE_STATE_INITIALIZING)) {
    // Either another core is mid-init, or we are in FAILED.
    return (state() == EJIT_SERVICE_STATE_FAILED) ? EJIT_SERVICE_ERR_INIT_FAILED
                                                  : EJIT_SERVICE_ERR_NOT_READY;
  }

  mode_ = local.mode;
  shareCodePointers_ = local.shareCodePointers;

  if (!buildBackend(local)) {
    destroyBackend();
    setState(EJIT_SERVICE_STATE_FAILED);
    EJIT_SVC_DIAG("init: backend build failed -> FAILED");
    return EJIT_SERVICE_ERR_INIT_FAILED;
  }

  // The worker is commissioned only AFTER the backend (ORC + registration +
  // taskpool) is fully built. A worker-start failure is a hard init failure —
  // we never accept requests that can never be consumed.
  if (!commissionWorker()) {
    destroyBackend();
    setState(EJIT_SERVICE_STATE_FAILED);
    EJIT_SVC_DIAG("init: worker commission failed -> FAILED");
    return EJIT_SERVICE_ERR_INIT_FAILED;
  }

  ++generation_;
  ++initCount_;
  setState(EJIT_SERVICE_STATE_READY); // registration is now frozen.
  EJIT_SVC_DIAG("init: READY gen=%llu mode=%u workerStarts=%llu",
                (unsigned long long)generation_, mode_,
                (unsigned long long)workerStartCount_.loadRelaxed());
  return EJIT_SERVICE_OK;
}

ejit_service_status_t WorkerService::shutdown() {
  ejit_service_state_t cur = state();
  if (cur == EJIT_SERVICE_STATE_UNINITIALIZED ||
      cur == EJIT_SERVICE_STATE_STOPPED)
    return EJIT_SERVICE_OK;

  setState(EJIT_SERVICE_STATE_STOPPING);
  destroyBackend(); // stops/join the worker, then destroys ORC + cache.
  workerStartCount_.storeRelease(0);
  workerTaskId_ = 0;
  regTable_.clear();
  moduleCount_ = 0;
  setState(EJIT_SERVICE_STATE_STOPPED);
  EJIT_SVC_DIAG("shutdown complete -> STOPPED");
  return EJIT_SERVICE_OK;
}

//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

ejit_service_status_t
WorkerService::registerModule(ejit_service_module_reg_t *m) {
  if (!m)
    return EJIT_SERVICE_ERR_INVALID_PARAM;
  if (m->abiVersion != EJIT_SERVICE_ABI_VERSION ||
      m->structSize != sizeof(ejit_service_module_reg_t))
    return EJIT_SERVICE_ERR_ABI_MISMATCH;
  if (m->entryCount && !m->entries)
    return EJIT_SERVICE_ERR_INVALID_PARAM;

  // Registration is frozen once the service is Ready (the worker reads the
  // registries lock-free; we must not mutate them underneath it).
  ejit_service_state_t cur = state();
  if (cur == EJIT_SERVICE_STATE_READY || cur == EJIT_SERVICE_STATE_STOPPING ||
      cur == EJIT_SERVICE_STATE_FAILED)
    return EJIT_SERVICE_ERR_FROZEN;

  // Pass 1: validate every entry WITHOUT mutating any state, so a conflicting
  // entry leaves prior registrations untouched.
  for (uint32_t i = 0; i < m->entryCount; ++i) {
    const ejit_service_reg_entry_t &e = m->entries[i];
    if (e.abiVersion != EJIT_SERVICE_ABI_VERSION)
      return EJIT_SERVICE_ERR_ABI_MISMATCH;
    if (e.kind < EJIT_SERVICE_REG_BITCODE || e.kind > EJIT_SERVICE_REG_SYMBOL)
      return EJIT_SERVICE_ERR_INVALID_PARAM;
    if (!e.name1 || !e.name1[0])
      return EJIT_SERVICE_ERR_INVALID_PARAM;
    if (isPayloadKind(e.kind)) {
      uint64_t fp = entryFingerprint(e);
      for (const RegRec &r : regTable_)
        if (r.kind == e.kind && r.name == e.name1 && r.fp != fp)
          return EJIT_SERVICE_ERR_CONFLICT; // same name, different payload.
    }
  }

  // Pass 2: commit. Idempotent re-registration (same name + same fingerprint)
  // is skipped; new names are assigned/forwarded.
  for (uint32_t i = 0; i < m->entryCount; ++i) {
    ejit_service_reg_entry_t &e = m->entries[i];
    uint64_t fp = entryFingerprint(e);

    bool seen = false;
    for (const RegRec &r : regTable_)
      if (r.kind == e.kind && r.name == e.name1) {
        seen = true;
        break;
      }

    switch (e.kind) {
    case EJIT_SERVICE_REG_FUNC_INDEX: {
      // Central, name-keyed, dense funcIndex. Idempotent across modules.
      uint32_t idx = EJitFuncRegistry::instance().resolveAssign(e.name1);
      if (idx == kEJitInvalidFuncIndex)
        return EJIT_SERVICE_ERR_CAPACITY;
      e.value0 = idx; // backfill the assigned global slot for the wrapper.
      break;
    }
    case EJIT_SERVICE_REG_LIFECYCLE: {
      uint32_t dt = EJitLifecycleRegistry::instance().resolveAssign(e.name1);
      if (dt == kEJitInvalidDimType)
        return EJIT_SERVICE_ERR_CAPACITY;
      e.value0 = dt;
      break;
    }
    case EJIT_SERVICE_REG_BITCODE:
      if (!seen) {
#ifndef EJIT_WORKER_SERVICE_TESTING
        EJitRegistrationStore::instance().registerBitcode(
            e.name1, static_cast<const uint8_t *>(e.address),
            static_cast<size_t>(e.size));
#endif
      }
      break;
    case EJIT_SERVICE_REG_PERIOD_ARRAY:
      if (!seen) {
#ifndef EJIT_WORKER_SERVICE_TESTING
        EJitRegistrationStore::instance().registerPeriodArray(
            e.name1, e.name2 ? e.name2 : "", const_cast<void *>(e.address),
            e.size);
#endif
      }
      break;
    case EJIT_SERVICE_REG_STATIC_VAR:
      if (!seen) {
#ifndef EJIT_WORKER_SERVICE_TESTING
        EJitRegistrationStore::instance().registerStaticVar(
            e.name1, const_cast<void *>(e.address));
#endif
      }
      break;
    case EJIT_SERVICE_REG_SYMBOL:
      if (!seen) {
#ifndef EJIT_WORKER_SERVICE_TESTING
        EJitRegistrationStore::instance().registerSymbol(
            e.name1, const_cast<void *>(e.address));
#endif
      }
      break;
    default:
      break;
    }

    if (!seen)
      regTable_.push_back({e.kind, std::string(e.name1), fp});
  }

  ++moduleCount_;
  EJIT_SVC_DIAG("register_module: entries=%u total=%zu modules=%u",
                m->entryCount, regTable_.size(), moduleCount_);
  return EJIT_SERVICE_OK;
}

//===----------------------------------------------------------------------===//
// Compile / poll
//===----------------------------------------------------------------------===//

static ejit_service_status_t mapStatus(EJitCompileOrGetStatus s) {
  switch (s) {
  case EJitCompileOrGetStatus::CacheHit:
    return EJIT_SERVICE_OK;
  case EJitCompileOrGetStatus::EnqueuedPending:
  case EJitCompileOrGetStatus::AlreadyPending:
    return EJIT_SERVICE_PENDING;
  case EJitCompileOrGetStatus::QueueFullFallback:
    return EJIT_SERVICE_ERR_QUEUE_FULL;
  case EJitCompileOrGetStatus::InstanceDisabled:
    return EJIT_SERVICE_ERR_INSTANCE_DISABLED;
  case EJitCompileOrGetStatus::OffMode:
    return EJIT_SERVICE_ERR_NOT_READY;
  case EJitCompileOrGetStatus::InvalidParam:
    return EJIT_SERVICE_ERR_INVALID_PARAM;
  case EJitCompileOrGetStatus::CompileFailed:
    return EJIT_SERVICE_ERR_COMPILE_FAILED;
  }
  return EJIT_SERVICE_ERR_COMPILE_FAILED;
}

ejit_service_status_t
WorkerService::compileOrGet(uint32_t funcIndex, const ejit_service_dim_t *dims,
                            uint32_t numDims, void *fallback, void **outFn,
                            uint32_t *outBucket) {
  if (outFn)
    *outFn = fallback;
  if (outBucket)
    *outBucket = 0;
  if (state() != EJIT_SERVICE_STATE_READY)
    return EJIT_SERVICE_ERR_NOT_READY;
  if (numDims > 4)
    return EJIT_SERVICE_ERR_INVALID_PARAM;
  if (numDims && !dims)
    return EJIT_SERVICE_ERR_INVALID_PARAM;

  EJitDimPair local[4];
  for (uint32_t i = 0; i < numDims; ++i) {
    if (dims[i].dimType >= EJitSwitchController::MAX_DIM_TYPES ||
        dims[i].instanceId >= EJitSwitchController::MAX_INSTANCES)
      return EJIT_SERVICE_ERR_INVALID_PARAM;
    local[i].dimType = dims[i].dimType;
    local[i].instanceId = dims[i].instanceId;
  }

  EJitTaskPool::CompileOrGetResult r = pool_->compileOrGet(
      funcIndex, numDims ? local : nullptr, numDims, fallback);

  // SYNC mode: the caller compiles inline. On a miss the request was enqueued;
  // drain the queue on this task, then do a CACHE-ONLY re-lookup (never
  // re-enqueue) so a successful compile returns the code and a failed compile
  // reports COMPILE_FAILED instead of spinning.
  if (mode_ == EJIT_SERVICE_MODE_SYNC &&
      (r.status == EJitCompileOrGetStatus::EnqueuedPending ||
       r.status == EJitCompileOrGetStatus::AlreadyPending)) {
    unsigned guard = EJIT_SRE_TASKPOOL_QUEUE_CAPACITY + 1u;
    while (guard-- && pool_->pendingCount() > 0)
      pool_->pollOne();
    EJitCacheLookupResult hit =
        pool_->cache().lookup(funcIndex, numDims ? local : nullptr, numDims);
    if (hit.fnPtr) {
      if (outFn)
        *outFn = hit.fnPtr;
      if (outBucket)
        *outBucket = hit.bucketIndex;
      return EJIT_SERVICE_OK;
    }
    return EJIT_SERVICE_ERR_COMPILE_FAILED;
  }

  if (r.status == EJitCompileOrGetStatus::CacheHit) {
    if (outFn)
      *outFn = r.fnPtr;
    if (outBucket)
      *outBucket = r.bucketIndex;
  }
  return mapStatus(r.status);
}

ejit_service_status_t WorkerService::releaseRead(uint32_t bucket) {
  if (!pool_)
    return EJIT_SERVICE_ERR_NOT_READY;
  pool_->releaseRead(bucket);
  return EJIT_SERVICE_OK;
}

uint32_t WorkerService::pollOne() {
  if (state() != EJIT_SERVICE_STATE_READY || !pool_)
    return 0;
  return pool_->pollOne() ? 1u : 0u;
}

uint32_t WorkerService::pollBudget(uint32_t maxItems) {
  if (state() != EJIT_SERVICE_STATE_READY || !pool_)
    return 0;
  return pool_->pollBudget(maxItems);
}

ejit_service_status_t WorkerService::setActive(const char *lifecycle,
                                               uint32_t instanceId, bool on) {
  if (!lifecycle || !lifecycle[0])
    return EJIT_SERVICE_ERR_INVALID_PARAM;
  if (state() != EJIT_SERVICE_STATE_READY || !pool_)
    return EJIT_SERVICE_ERR_NOT_READY;
  if (instanceId >= EJitSwitchController::MAX_INSTANCES)
    return EJIT_SERVICE_ERR_INVALID_PARAM;
  uint32_t dt = EJitLifecycleRegistry::instance().lookup(lifecycle);
  if (dt == kEJitInvalidDimType)
    return EJIT_SERVICE_ERR_INVALID_PARAM;
  pool_->switchController().setEnabled(dt, instanceId, on);
  return EJIT_SERVICE_OK;
}

ejit_service_status_t WorkerService::freeCode(uint32_t funcIndex,
                                              const ejit_service_dim_t *dims,
                                              uint32_t numDims) {
  if (state() != EJIT_SERVICE_STATE_READY || !pool_)
    return EJIT_SERVICE_ERR_NOT_READY;
  if (numDims > 4 || (numDims && !dims))
    return EJIT_SERVICE_ERR_INVALID_PARAM;
  EJitDimPair local[4];
  for (uint32_t i = 0; i < numDims; ++i) {
    local[i].dimType = dims[i].dimType;
    local[i].instanceId = dims[i].instanceId;
  }
  EJitCacheLookupResult r =
      pool_->cache().lookup(funcIndex, numDims ? local : nullptr, numDims);
  if (r.fnPtr) {
    pool_->cache().retireCode(r.fnPtr);
    if (r.hasReadToken)
      pool_->releaseRead(r.bucketIndex);
  }
  return EJIT_SERVICE_OK;
}

//===----------------------------------------------------------------------===//
// Identity / diagnostics
//===----------------------------------------------------------------------===//

void WorkerService::identity(ejit_service_identity_t *out) const {
  out->abiVersion = EJIT_SERVICE_ABI_VERSION;
  out->structSize = sizeof(ejit_service_identity_t);
  out->instanceAddress = reinterpret_cast<uintptr_t>(this);
  out->queueAddress = reinterpret_cast<uintptr_t>(pool_);
  out->cacheAddress =
      pool_ ? reinterpret_cast<uintptr_t>(&pool_->cache()) : 0ull;
  out->generation = generation_;
  out->workerStartCount = workerStartCount_.loadRelaxed();
  out->workerTaskId = workerTaskId_;
}

void WorkerService::diagnostics(ejit_service_diagnostics_t *out) const {
  out->abiVersion = EJIT_SERVICE_ABI_VERSION;
  out->structSize = sizeof(ejit_service_diagnostics_t);
  out->state = state_.loadAcquire();
  out->mode = mode_;
  out->instanceAddress = reinterpret_cast<uintptr_t>(this);
  out->queueAddress = reinterpret_cast<uintptr_t>(pool_);
  out->cacheAddress =
      pool_ ? reinterpret_cast<uintptr_t>(&pool_->cache()) : 0ull;
  out->generation = generation_;
  out->initCount = initCount_;
  out->workerStartCount = workerStartCount_.loadRelaxed();
  out->workerTaskId = workerTaskId_;
  out->workerStackSize = EJIT_SRE_TASKPOOL_WORKER_STACK_SIZE;
  out->shareCodePointers = shareCodePointers_;
  out->registrationCount = static_cast<uint32_t>(regTable_.size());
  out->moduleCount = moduleCount_;
  out->funcIndexCount = EJitFuncRegistry::instance().count();
  out->dimTypeCount = EJitLifecycleRegistry::instance().count();

  EJitTaskPoolStatsSnapshot s;
  std::memset(&s, 0, sizeof(s));
  if (pool_)
    pool_->getStats(s);
  out->queueDepth = s.queueApproxSize;
  out->pendingCount = pool_ ? pool_->pendingCount() : 0u;
  out->cacheReadyEntries = s.readyEntries;
  out->cacheHits = s.cacheHits;
  out->enqueueCount = s.asyncEnqueues;
  out->compileCount = s.asyncCompiles;
  out->publishFailCount = s.publishFailed;
  out->queueFullCount = s.queueFull;
}

} // namespace

//===----------------------------------------------------------------------===//
// C ABI — the entire dlib export surface. Every entry point validates the
// argument and returns an explicit status; none can throw.
//===----------------------------------------------------------------------===//

extern "C" {

uint32_t ejit_service_get_abi_version(void) { return EJIT_SERVICE_ABI_VERSION; }

ejit_service_status_t ejit_service_init(const ejit_service_config_t *config) {
  return WorkerService::instance().init(config);
}

ejit_service_status_t ejit_service_shutdown(void) {
  return WorkerService::instance().shutdown();
}

ejit_service_state_t ejit_service_get_state(void) {
  return WorkerService::instance().state();
}

ejit_service_status_t
ejit_service_get_diagnostics(ejit_service_diagnostics_t *out) {
  if (!out)
    return EJIT_SERVICE_ERR_INVALID_PARAM;
  WorkerService::instance().diagnostics(out);
  return EJIT_SERVICE_OK;
}

ejit_service_status_t ejit_service_get_identity(ejit_service_identity_t *out) {
  if (!out)
    return EJIT_SERVICE_ERR_INVALID_PARAM;
  WorkerService::instance().identity(out);
  return EJIT_SERVICE_OK;
}

ejit_service_status_t
ejit_service_register_module(ejit_service_module_reg_t *module) {
  return WorkerService::instance().registerModule(module);
}

ejit_service_status_t
ejit_service_compile_or_get(uint32_t funcIndex, const ejit_service_dim_t *dims,
                            uint32_t numDims, void *fallback, void **outFn,
                            uint32_t *outBucket) {
  return WorkerService::instance().compileOrGet(funcIndex, dims, numDims,
                                                fallback, outFn, outBucket);
}

ejit_service_status_t ejit_service_release_read(uint32_t bucket) {
  return WorkerService::instance().releaseRead(bucket);
}

uint32_t ejit_service_worker_poll_one(void) {
  return WorkerService::instance().pollOne();
}

uint32_t ejit_service_worker_poll_budget(uint32_t maxItems) {
  return WorkerService::instance().pollBudget(maxItems);
}

ejit_service_status_t ejit_service_activate(const char *lifecycle,
                                            uint32_t instanceId) {
  return WorkerService::instance().setActive(lifecycle, instanceId, true);
}

ejit_service_status_t ejit_service_deactivate(const char *lifecycle,
                                              uint32_t instanceId) {
  return WorkerService::instance().setActive(lifecycle, instanceId, false);
}

ejit_service_status_t ejit_service_free_code(uint32_t funcIndex,
                                             const ejit_service_dim_t *dims,
                                             uint32_t numDims) {
  return WorkerService::instance().freeCode(funcIndex, dims, numDims);
}

#ifdef EJIT_WORKER_SERVICE_TESTING
//===----------------------------------------------------------------------===//
// Test-only hooks (compiled only into the host unit test, never into the dlib).
//===----------------------------------------------------------------------===//
void ejit_service_test_set_compile_fail(int fail) {
  gMockCompileFail.storeRelease(fail ? 1u : 0u);
}
void ejit_service_test_set_worker_start_fail(int fail) {
  gMockWorkerStartFail.storeRelease(fail ? 1u : 0u);
}
uint64_t ejit_service_test_compile_calls(void) {
  return gMockCompileCalls.loadRelaxed();
}
void ejit_service_test_reset_registries(void) {
  WorkerService::instance().testFullReset();
  EJitFuncRegistry::instance().reset();
  EJitLifecycleRegistry::instance().reset();
}
#endif

} // extern "C"

#endif // EJIT_WORKER_SERVICE
