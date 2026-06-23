//===-- EJitWorkerService.h - Standalone EmbeddedJIT worker service -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  Stable C ABI for the EmbeddedJIT "worker service" — a standalone dynamic
//  library (dlib) that holds ONE JIT worker, ONE compile queue, ONE cache, ONE
//  ORC/JITLink engine and ONE registration registry as a plain in-library
//  singleton.
//
//  Design intent (see jit_design_doc/EJIT_WORKER_SERVICE_DLIB.md):
//
//    * The service object is an ordinary process-local singleton living in the
//      dlib's data/bss. If the platform maps a SINGLE dlib data/bss instance
//      that every business core shares ("model A"), that ordinary singleton IS
//      the one shared service — no custom shared section, no owner election, no
//      cross-core shared-state blob. ejit_service_get_identity() lets the
//      platform VERIFY model A at runtime instead of assuming it.
//
//    * If instead each core gets its own dlib data/bss copy ("model B"), each
//      core sees an independent singleton; identity addresses differ. The
//      single-worker goal is then NOT achievable through this mechanism and the
//      caller must fall back (documented, not silently broken).
//
//  This header is the ONLY surface business modules ever touch. They never see
//  a C++ class. Every struct carries {abiVersion, structSize} and uses only
//  fixed-width scalars / opaque pointers so the ABI is append-only and
//  big-endian safe (each field is a naturally aligned scalar, never parsed from
//  raw bytes). No exceptions, no RTTI, no C++ STL types cross this boundary.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITWORKERSERVICE_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITWORKERSERVICE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// ABI version of every struct in this header. Bump only on a breaking change;
/// fields are otherwise append-only (new fields go at the end and old callers
/// keep working because every struct also carries its own structSize).
#define EJIT_SERVICE_ABI_VERSION 1u

/// Export marker for the dlib's C ABI. The service library is built with
/// -fvisibility=hidden so that no LLVM internal symbol leaks; the C ABI entry
/// points carry default visibility so they remain exported. (Harmless when the
/// service source is compiled directly into a host unit-test executable.)
#if defined(_WIN32)
#define EJIT_SERVICE_API __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define EJIT_SERVICE_API __attribute__((visibility("default")))
#else
#define EJIT_SERVICE_API
#endif

//===----------------------------------------------------------------------===//
// Status codes
//===----------------------------------------------------------------------===//

/// Explicit status codes. Non-negative values are "not an error" (OK /
/// PENDING); negative values are hard failures. No status is ever signalled by
/// an exception — every entry point returns a code.
typedef enum {
  EJIT_SERVICE_OK = 0,
  EJIT_SERVICE_PENDING = 1, ///< async: request accepted, result not ready yet

  EJIT_SERVICE_ERR_INVALID_PARAM = -1,
  EJIT_SERVICE_ERR_NOT_READY = -2,   ///< service not in Ready state
  EJIT_SERVICE_ERR_INIT_FAILED = -3, ///< backend init failed
  EJIT_SERVICE_ERR_FROZEN = -4,      ///< registration closed after init
  EJIT_SERVICE_ERR_CONFLICT = -5,    ///< same name, different payload
  EJIT_SERVICE_ERR_CAPACITY = -6,    ///< funcIndex/dimType/table exhausted
  EJIT_SERVICE_ERR_QUEUE_FULL = -7,  ///< compile queue full (fallback used)
  EJIT_SERVICE_ERR_COMPILE_FAILED = -8,
  EJIT_SERVICE_ERR_INSTANCE_DISABLED = -9, ///< lifecycle/instance not active
  EJIT_SERVICE_ERR_ABI_MISMATCH = -10,     ///< abiVersion/structSize rejected
  EJIT_SERVICE_ERR_MEMORY = -11,
  EJIT_SERVICE_ERR_UNSUPPORTED = -12, ///< capability not built/allowed
} ejit_service_status_t;

//===----------------------------------------------------------------------===//
// Lifecycle state machine (explicit enum, never a bool)
//===----------------------------------------------------------------------===//

typedef enum {
  EJIT_SERVICE_STATE_UNINITIALIZED = 0,
  EJIT_SERVICE_STATE_INITIALIZING = 1,
  EJIT_SERVICE_STATE_READY = 2,
  EJIT_SERVICE_STATE_FAILED = 3,
  EJIT_SERVICE_STATE_STOPPING = 4,
  EJIT_SERVICE_STATE_STOPPED = 5,
} ejit_service_state_t;

//===----------------------------------------------------------------------===//
// Worker / compile mode
//===----------------------------------------------------------------------===//

typedef enum {
  /// Synchronous: the caller compiles inline (drains the queue on its own
  /// task), shares the one service cache, and NO background worker runs.
  EJIT_SERVICE_MODE_SYNC = 0,
  /// Asynchronous: a single background worker task drains the queue. A cache
  /// miss enqueues and returns PENDING with the fallback pointer.
  EJIT_SERVICE_MODE_ASYNC = 1,
  /// Asynchronous semantics, but with NO background task. The caller (or
  /// platform pump) drains the queue via ejit_service_worker_poll_*. Useful for
  /// deterministic tests and for platforms that pump compiles from their own
  /// scheduler tick.
  EJIT_SERVICE_MODE_ASYNC_MANUAL = 2,
} ejit_service_mode_t;

//===----------------------------------------------------------------------===//
// Registration protocol (POD)
//===----------------------------------------------------------------------===//

typedef enum {
  EJIT_SERVICE_REG_BITCODE = 1, ///< name1=funcName, address=bitcode, size=bytes
  EJIT_SERVICE_REG_FUNC_INDEX =
      2, ///< name1=funcName; value0<-assigned funcIndex
  EJIT_SERVICE_REG_LIFECYCLE =
      3, ///< name1=lifecycleName; value0<-assigned dimType
  EJIT_SERVICE_REG_PERIOD_ARRAY =
      4, ///< name1=periodName, name2=varName, address=base, size=elems
  EJIT_SERVICE_REG_STATIC_VAR = 5, ///< name1=varName, address=var
  EJIT_SERVICE_REG_SYMBOL = 6,     ///< name1=symbol, address=addr
} ejit_service_reg_kind_t;

/// One registration entry. The service ASSIGNS funcIndex/dimType centrally and
/// writes the result back into value0 (so the wrapper reads back the assigned
/// global slot rather than recomputing it). All scalar fields are naturally
/// aligned — no byte-by-byte parsing, big-endian safe.
typedef struct {
  uint32_t kind;       ///< ejit_service_reg_kind_t
  uint32_t abiVersion; ///< EJIT_SERVICE_ABI_VERSION
  const char *name1;   ///< primary name (UTF-8, NUL-terminated)
  const char *name2;   ///< secondary name or NULL
  const void *address; ///< payload pointer or NULL
  uint64_t size;       ///< payload size / array element count
  uint64_t value0;     ///< OUT: assigned funcIndex/dimType (else 0)
  uint64_t value1;     ///< reserved (must be 0)
} ejit_service_reg_entry_t;

/// A batch of registration entries from one business module. Registration is
/// idempotent: re-registering the same {kind,name,payload} succeeds; a name
/// re-registered with a DIFFERENT payload is rejected
/// (EJIT_SERVICE_ERR_CONFLICT) and leaves prior state unchanged.
typedef struct {
  uint32_t abiVersion;               ///< EJIT_SERVICE_ABI_VERSION
  uint32_t structSize;               ///< sizeof(ejit_service_module_reg_t)
  const char *moduleName;            ///< for diagnostics, may be NULL
  ejit_service_reg_entry_t *entries; ///< array of entryCount entries
  uint32_t entryCount;
  uint32_t reserved; ///< must be 0
} ejit_service_module_reg_t;

//===----------------------------------------------------------------------===//
// Configuration
//===----------------------------------------------------------------------===//

typedef struct {
  uint32_t abiVersion;      ///< EJIT_SERVICE_ABI_VERSION
  uint32_t structSize;      ///< sizeof(ejit_service_config_t)
  uint32_t mode;            ///< ejit_service_mode_t
  uint32_t optLevel;        ///< 1..3 (clamped)
  uint64_t maxCodeMemory;   ///< 0 = backend default
  uint64_t maxDataMemory;   ///< 0 = backend default
  uint64_t maxCacheEntries; ///< 0 = backend default
  uint64_t maxCacheSize;    ///< 0 = backend default
  uint32_t enableLogger; ///< nonzero enables host logger (ignored freestanding)
  /// Capability gate for returning compiled code pointers that are intended to
  /// be executed on a core OTHER than the one that requested the compile. This
  /// is OFF by default. It only makes sense, and is only safe, when the
  /// platform has confirmed: one address space, code pool mapped at the same VA
  /// on all cores, relocations applied, RW->RX done, I/D cache synchronised,
  /// and code-pool lifetime covering all callers. The service NEVER
  /// auto-detects this — the integrator must assert it.
  uint32_t shareCodePointers;
  uint32_t reserved0; ///< must be 0
  uint32_t reserved1; ///< must be 0
} ejit_service_config_t;

//===----------------------------------------------------------------------===//
// Compile request dimension pair
//===----------------------------------------------------------------------===//

typedef struct {
  uint32_t dimType;    ///< lifecycle slot in [0, 8)
  uint32_t instanceId; ///< instance in [0, 256)
} ejit_service_dim_t;

//===----------------------------------------------------------------------===//
// Identity — for verifying the single-shared-instance (model A) assumption
//===----------------------------------------------------------------------===//

typedef struct {
  uint32_t abiVersion;       ///< EJIT_SERVICE_ABI_VERSION
  uint32_t structSize;       ///< sizeof(ejit_service_identity_t)
  uint64_t instanceAddress;  ///< address of the service singleton in this dlib
  uint64_t queueAddress;     ///< address of the one compile queue/taskpool
  uint64_t cacheAddress;     ///< address of the one code cache
  uint64_t generation;       ///< bumped on every successful init
  uint64_t workerStartCount; ///< total worker task starts (==1 for one worker)
  uint64_t workerTaskId; ///< opaque platform task id of the worker (0 = none)
} ejit_service_identity_t;

//===----------------------------------------------------------------------===//
// Diagnostics snapshot
//===----------------------------------------------------------------------===//

typedef struct {
  uint32_t abiVersion; ///< EJIT_SERVICE_ABI_VERSION
  uint32_t structSize; ///< sizeof(ejit_service_diagnostics_t)
  uint32_t state;      ///< ejit_service_state_t
  uint32_t mode;       ///< ejit_service_mode_t
  uint64_t instanceAddress;
  uint64_t queueAddress;
  uint64_t cacheAddress;
  uint64_t generation;
  uint64_t initCount; ///< number of accepted init() calls
  uint64_t workerStartCount;
  uint64_t workerTaskId;
  uint64_t workerStackSize;   ///< configured worker stack size (bytes)
  uint32_t shareCodePointers; ///< effective capability flag
  uint32_t registrationCount; ///< accepted registration entries
  uint32_t moduleCount;       ///< accepted module batches
  uint32_t funcIndexCount;    ///< distinct funcIndex assigned
  uint32_t dimTypeCount;      ///< distinct dimType assigned
  uint32_t queueDepth;        ///< approximate in-flight queue depth
  uint32_t pendingCount;      ///< in-flight compile requests
  uint32_t cacheReadyEntries; ///< published cache entries
  uint64_t cacheHits;
  uint64_t enqueueCount;
  uint64_t compileCount; ///< worker-processed compiles
  uint64_t publishFailCount;
  uint64_t queueFullCount;
} ejit_service_diagnostics_t;

//===----------------------------------------------------------------------===//
// C ABI entry points (the entire dlib export surface)
//===----------------------------------------------------------------------===//

/// Returns the ABI version this dlib was built with.
EJIT_SERVICE_API uint32_t ejit_service_get_abi_version(void);

/// Initialize (or idempotently re-affirm) the singleton service. The FIRST
/// successful call creates the one worker/queue/cache/ORC; later calls with a
/// compatible config return EJIT_SERVICE_OK without rebuilding. A backend or
/// worker-start failure transitions the service to FAILED and returns an
/// explicit error (never a fake success). \p config may be NULL for defaults.
EJIT_SERVICE_API ejit_service_status_t
ejit_service_init(const ejit_service_config_t *config);

/// Stop the worker (join before destroying ORC/registration) and destroy the
/// singleton. Safe to call when already stopped. No worker callback may run
/// after this returns.
EJIT_SERVICE_API ejit_service_status_t ejit_service_shutdown(void);

/// Current lifecycle state.
EJIT_SERVICE_API ejit_service_state_t ejit_service_get_state(void);

/// Copy a diagnostics snapshot into \p out (must set out->abiVersion and
/// out->structSize first, or pass a zeroed struct — the service fills them).
EJIT_SERVICE_API ejit_service_status_t
ejit_service_get_diagnostics(ejit_service_diagnostics_t *out);

/// Copy the service identity into \p out. Used by every core to compare
/// instanceAddress/generation/workerStartCount and decide whether model A
/// holds.
EJIT_SERVICE_API ejit_service_status_t
ejit_service_get_identity(ejit_service_identity_t *out);

/// Register a batch of module entries BEFORE init() (registration is frozen
/// once the service reaches Ready). Assigns funcIndex/dimType centrally and
/// writes them back into each entry's value0. Idempotent for identical payload;
/// rejects a name re-registered with a different payload.
EJIT_SERVICE_API ejit_service_status_t
ejit_service_register_module(ejit_service_module_reg_t *module);

/// Look up a compiled specialization, or schedule its compile.
///   * cache hit  -> *outFn = code, *outBucket = read token, returns OK
///                   (caller must call ejit_service_release_read(*outBucket))
///   * SYNC mode   -> compiles inline, behaves like a hit on success
///   * ASYNC modes -> enqueues, *outFn = fallback, returns PENDING
/// numDims must be <= 4. dims may be NULL when numDims == 0.
EJIT_SERVICE_API ejit_service_status_t ejit_service_compile_or_get(
    uint32_t funcIndex, const ejit_service_dim_t *dims, uint32_t numDims,
    void *fallback, void **outFn, uint32_t *outBucket);

/// Release a read token returned by a cache hit from compile_or_get.
EJIT_SERVICE_API ejit_service_status_t
ejit_service_release_read(uint32_t bucket);

/// Drain at most one queued compile on the calling task. Returns the number
/// processed (0 or 1). Meaningful in SYNC / ASYNC_MANUAL modes.
EJIT_SERVICE_API uint32_t ejit_service_worker_poll_one(void);

/// Drain up to \p maxItems queued compiles on the calling task. Returns the
/// number processed.
EJIT_SERVICE_API uint32_t ejit_service_worker_poll_budget(uint32_t maxItems);

/// Activate a lifecycle instance (enables compiles + bumps its version so any
/// in-flight request for a now-stale version is dropped at the publish gate).
EJIT_SERVICE_API ejit_service_status_t
ejit_service_activate(const char *lifecycle, uint32_t instanceId);

/// Deactivate a lifecycle instance.
EJIT_SERVICE_API ejit_service_status_t
ejit_service_deactivate(const char *lifecycle, uint32_t instanceId);

/// Retire any compiled code for the given specialization (best effort).
EJIT_SERVICE_API ejit_service_status_t ejit_service_free_code(
    uint32_t funcIndex, const ejit_service_dim_t *dims, uint32_t numDims);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITWORKERSERVICE_H
