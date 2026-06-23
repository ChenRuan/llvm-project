//===-- EJitWorkerServiceTest.cpp - Worker service unit tests ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  Host-runnable, deterministic tests for the standalone EmbeddedJIT worker
//  service. The service source is compiled directly into this executable with
//  -DEJIT_WORKER_SERVICE_TESTING, so the service uses a MOCK compiler (no ORC,
//  no real codegen) and the SYNC / ASYNC_MANUAL paths run with NO real threads.
//  A single ASYNC test exercises the real host worker thread (std::thread, the
//  host adapter) to prove the single-worker contract.
//
//  These tests answer the design question directly with code: a plain
//  in-library singleton can hold one worker / one queue / one cache / one
//  registry; identity + diagnostics make the single-instance assumption
//  verifiable rather than assumed.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitWorkerService.h"
#include "gtest/gtest.h"

#include <cstring>
#include <string>
#include <thread>
#include <vector>

// Test-only hooks exported by EJitWorkerService.cpp under
// EJIT_WORKER_SERVICE_TESTING.
extern "C" {
void ejit_service_test_set_compile_fail(int fail);
void ejit_service_test_set_worker_start_fail(int fail);
uint64_t ejit_service_test_compile_calls(void);
void ejit_service_test_reset_registries(void);
}

namespace {

ejit_service_config_t makeConfig(ejit_service_mode_t mode) {
  ejit_service_config_t c;
  std::memset(&c, 0, sizeof(c));
  c.abiVersion = EJIT_SERVICE_ABI_VERSION;
  c.structSize = sizeof(ejit_service_config_t);
  c.mode = mode;
  c.optLevel = 2;
  c.enableLogger = 0;
  return c;
}

ejit_service_reg_entry_t makeEntry(ejit_service_reg_kind_t kind,
                                   const char *name1, const char *name2,
                                   const void *address, uint64_t size) {
  ejit_service_reg_entry_t e;
  std::memset(&e, 0, sizeof(e));
  e.kind = kind;
  e.abiVersion = EJIT_SERVICE_ABI_VERSION;
  e.name1 = name1;
  e.name2 = name2;
  e.address = address;
  e.size = size;
  return e;
}

ejit_service_status_t registerOne(ejit_service_reg_entry_t *entries,
                                  uint32_t count, const char *moduleName) {
  ejit_service_module_reg_t m;
  std::memset(&m, 0, sizeof(m));
  m.abiVersion = EJIT_SERVICE_ABI_VERSION;
  m.structSize = sizeof(ejit_service_module_reg_t);
  m.moduleName = moduleName;
  m.entries = entries;
  m.entryCount = count;
  return ejit_service_register_module(&m);
}

// Clean global state between tests: the service and the funcIndex/lifecycle
// registries are process-global singletons.
class WorkerServiceTest : public ::testing::Test {
protected:
  void SetUp() override {
    ejit_service_shutdown();
    ejit_service_test_reset_registries();
    ejit_service_test_set_compile_fail(0);
    ejit_service_test_set_worker_start_fail(0);
  }
  void TearDown() override { ejit_service_shutdown(); }
};

//===----------------------------------------------------------------------===//
// ABI / version
//===----------------------------------------------------------------------===//

TEST_F(WorkerServiceTest, AbiVersionMatchesHeader) {
  EXPECT_EQ(ejit_service_get_abi_version(), EJIT_SERVICE_ABI_VERSION);
}

TEST_F(WorkerServiceTest, InitRejectsBadAbiVersion) {
  ejit_service_config_t c = makeConfig(EJIT_SERVICE_MODE_SYNC);
  c.abiVersion = 999u;
  EXPECT_EQ(ejit_service_init(&c), EJIT_SERVICE_ERR_ABI_MISMATCH);
  c.abiVersion = EJIT_SERVICE_ABI_VERSION;
  c.structSize = 7u;
  EXPECT_EQ(ejit_service_init(&c), EJIT_SERVICE_ERR_ABI_MISMATCH);
}

TEST_F(WorkerServiceTest, RegisterRejectsBadAbiVersion) {
  ejit_service_reg_entry_t e =
      makeEntry(EJIT_SERVICE_REG_FUNC_INDEX, "f", nullptr, nullptr, 0);
  ejit_service_module_reg_t m;
  std::memset(&m, 0, sizeof(m));
  m.abiVersion = 2u; // wrong
  m.structSize = sizeof(m);
  m.entries = &e;
  m.entryCount = 1;
  EXPECT_EQ(ejit_service_register_module(&m), EJIT_SERVICE_ERR_ABI_MISMATCH);
}

TEST_F(WorkerServiceTest, StructsCarrySizeAndVersion) {
  ASSERT_EQ(ejit_service_init(nullptr), EJIT_SERVICE_OK);
  ejit_service_identity_t id;
  std::memset(&id, 0, sizeof(id));
  ASSERT_EQ(ejit_service_get_identity(&id), EJIT_SERVICE_OK);
  EXPECT_EQ(id.abiVersion, EJIT_SERVICE_ABI_VERSION);
  EXPECT_EQ(id.structSize, sizeof(ejit_service_identity_t));
  ejit_service_diagnostics_t d;
  std::memset(&d, 0, sizeof(d));
  ASSERT_EQ(ejit_service_get_diagnostics(&d), EJIT_SERVICE_OK);
  EXPECT_EQ(d.abiVersion, EJIT_SERVICE_ABI_VERSION);
  EXPECT_EQ(d.structSize, sizeof(ejit_service_diagnostics_t));
}

//===----------------------------------------------------------------------===//
// Singleton / one-worker contract
//===----------------------------------------------------------------------===//

TEST_F(WorkerServiceTest, RepeatedInitYieldsOneService) {
  ejit_service_config_t c = makeConfig(EJIT_SERVICE_MODE_SYNC);
  ASSERT_EQ(ejit_service_init(&c), EJIT_SERVICE_OK);
  ejit_service_identity_t a;
  std::memset(&a, 0, sizeof(a));
  ASSERT_EQ(ejit_service_get_identity(&a), EJIT_SERVICE_OK);
  // Second init is idempotent: same instance, same generation (no rebuild).
  ASSERT_EQ(ejit_service_init(&c), EJIT_SERVICE_OK);
  ejit_service_identity_t b;
  std::memset(&b, 0, sizeof(b));
  ASSERT_EQ(ejit_service_get_identity(&b), EJIT_SERVICE_OK);
  EXPECT_NE(a.instanceAddress, 0ull);
  EXPECT_EQ(a.instanceAddress, b.instanceAddress);
  EXPECT_EQ(a.queueAddress, b.queueAddress);
  EXPECT_EQ(a.cacheAddress, b.cacheAddress);
  EXPECT_EQ(a.generation, b.generation);
  EXPECT_EQ(a.generation, 1ull);
}

TEST_F(WorkerServiceTest, RepeatedInitYieldsOneWorker) {
  // ASYNC commissions one real host worker; re-init must not start a second.
  ejit_service_config_t c = makeConfig(EJIT_SERVICE_MODE_ASYNC);
  ASSERT_EQ(ejit_service_init(&c), EJIT_SERVICE_OK);
  ASSERT_EQ(ejit_service_init(&c), EJIT_SERVICE_OK);
  ASSERT_EQ(ejit_service_init(&c), EJIT_SERVICE_OK);
  ejit_service_identity_t id;
  std::memset(&id, 0, sizeof(id));
  ASSERT_EQ(ejit_service_get_identity(&id), EJIT_SERVICE_OK);
  EXPECT_EQ(id.workerStartCount, 1ull);
  EXPECT_EQ(ejit_service_get_state(), EJIT_SERVICE_STATE_READY);
}

TEST_F(WorkerServiceTest, IdentityIsStableAcrossCalls) {
  ASSERT_EQ(ejit_service_init(nullptr), EJIT_SERVICE_OK);
  ejit_service_identity_t a, b;
  std::memset(&a, 0, sizeof(a));
  std::memset(&b, 0, sizeof(b));
  ASSERT_EQ(ejit_service_get_identity(&a), EJIT_SERVICE_OK);
  ASSERT_EQ(ejit_service_get_identity(&b), EJIT_SERVICE_OK);
  EXPECT_EQ(a.instanceAddress, b.instanceAddress);
  EXPECT_NE(a.queueAddress, 0ull);
  EXPECT_NE(a.cacheAddress, 0ull);
}

//===----------------------------------------------------------------------===//
// State machine
//===----------------------------------------------------------------------===//

TEST_F(WorkerServiceTest, StateProgression) {
  EXPECT_EQ(ejit_service_get_state(), EJIT_SERVICE_STATE_UNINITIALIZED);
  ejit_service_config_t c = makeConfig(EJIT_SERVICE_MODE_SYNC);
  ASSERT_EQ(ejit_service_init(&c), EJIT_SERVICE_OK);
  EXPECT_EQ(ejit_service_get_state(), EJIT_SERVICE_STATE_READY);
  ASSERT_EQ(ejit_service_shutdown(), EJIT_SERVICE_OK);
  EXPECT_EQ(ejit_service_get_state(), EJIT_SERVICE_STATE_STOPPED);
}

TEST_F(WorkerServiceTest, InitFailurePropagatesToFailedState) {
  ejit_service_test_set_worker_start_fail(1);
  ejit_service_config_t c = makeConfig(EJIT_SERVICE_MODE_ASYNC);
  EXPECT_EQ(ejit_service_init(&c), EJIT_SERVICE_ERR_INIT_FAILED);
  EXPECT_EQ(ejit_service_get_state(), EJIT_SERVICE_STATE_FAILED);
  // No fake success: compile is refused while not Ready.
  void *fn = nullptr;
  uint32_t bucket = 0;
  EXPECT_EQ(ejit_service_compile_or_get(0, nullptr, 0, nullptr, &fn, &bucket),
            EJIT_SERVICE_ERR_NOT_READY);
}

//===----------------------------------------------------------------------===//
// Registration protocol
//===----------------------------------------------------------------------===//

TEST_F(WorkerServiceTest, IdempotentReRegistration) {
  static const uint8_t bc[4] = {1, 2, 3, 4};
  ejit_service_reg_entry_t e =
      makeEntry(EJIT_SERVICE_REG_BITCODE, "f", nullptr, bc, sizeof(bc));
  EXPECT_EQ(registerOne(&e, 1, "m"), EJIT_SERVICE_OK);
  EXPECT_EQ(registerOne(&e, 1, "m"), EJIT_SERVICE_OK); // same payload again
  ASSERT_EQ(ejit_service_init(nullptr), EJIT_SERVICE_OK);
  ejit_service_diagnostics_t d;
  std::memset(&d, 0, sizeof(d));
  ASSERT_EQ(ejit_service_get_diagnostics(&d), EJIT_SERVICE_OK);
  EXPECT_EQ(d.registrationCount, 1u); // counted once despite two registrations
}

TEST_F(WorkerServiceTest, ConflictingReRegistrationRejected) {
  static const uint8_t a[4] = {1, 2, 3, 4};
  static const uint8_t b[4] = {9, 9, 9, 9};
  ejit_service_reg_entry_t ea =
      makeEntry(EJIT_SERVICE_REG_BITCODE, "f", nullptr, a, sizeof(a));
  ejit_service_reg_entry_t eb =
      makeEntry(EJIT_SERVICE_REG_BITCODE, "f", nullptr, b, sizeof(b));
  EXPECT_EQ(registerOne(&ea, 1, "m"), EJIT_SERVICE_OK);
  EXPECT_EQ(registerOne(&eb, 1, "m"), EJIT_SERVICE_ERR_CONFLICT);
  // Prior registration is unchanged.
  ASSERT_EQ(ejit_service_init(nullptr), EJIT_SERVICE_OK);
  ejit_service_diagnostics_t d;
  std::memset(&d, 0, sizeof(d));
  ASSERT_EQ(ejit_service_get_diagnostics(&d), EJIT_SERVICE_OK);
  EXPECT_EQ(d.registrationCount, 1u);
}

TEST_F(WorkerServiceTest, CrossModuleSameFuncSameIndex) {
  ejit_service_reg_entry_t e1 =
      makeEntry(EJIT_SERVICE_REG_FUNC_INDEX, "foo", nullptr, nullptr, 0);
  ejit_service_reg_entry_t e2 =
      makeEntry(EJIT_SERVICE_REG_FUNC_INDEX, "foo", nullptr, nullptr, 0);
  EXPECT_EQ(registerOne(&e1, 1, "modA"), EJIT_SERVICE_OK);
  EXPECT_EQ(registerOne(&e2, 1, "modB"), EJIT_SERVICE_OK);
  EXPECT_EQ(e1.value0, e2.value0); // same name -> same funcIndex across modules
}

TEST_F(WorkerServiceTest, DistinctFuncsGetDistinctIndices) {
  ejit_service_reg_entry_t ea =
      makeEntry(EJIT_SERVICE_REG_FUNC_INDEX, "a", nullptr, nullptr, 0);
  ejit_service_reg_entry_t eb =
      makeEntry(EJIT_SERVICE_REG_FUNC_INDEX, "b", nullptr, nullptr, 0);
  EXPECT_EQ(registerOne(&ea, 1, "m"), EJIT_SERVICE_OK);
  EXPECT_EQ(registerOne(&eb, 1, "m"), EJIT_SERVICE_OK);
  EXPECT_NE(ea.value0, eb.value0);
}

TEST_F(WorkerServiceTest, ConsistentLifecycleMapping) {
  ejit_service_reg_entry_t l1 =
      makeEntry(EJIT_SERVICE_REG_LIFECYCLE, "L1", nullptr, nullptr, 0);
  ejit_service_reg_entry_t l1b =
      makeEntry(EJIT_SERVICE_REG_LIFECYCLE, "L1", nullptr, nullptr, 0);
  ejit_service_reg_entry_t l2 =
      makeEntry(EJIT_SERVICE_REG_LIFECYCLE, "L2", nullptr, nullptr, 0);
  EXPECT_EQ(registerOne(&l1, 1, "m"), EJIT_SERVICE_OK);
  EXPECT_EQ(registerOne(&l1b, 1, "m"), EJIT_SERVICE_OK);
  EXPECT_EQ(registerOne(&l2, 1, "m"), EJIT_SERVICE_OK);
  EXPECT_EQ(l1.value0, l1b.value0);
  EXPECT_NE(l1.value0, l2.value0);
}

TEST_F(WorkerServiceTest, LifecycleCapacityExhaustionRejected) {
  std::vector<ejit_service_reg_entry_t> entries;
  std::vector<std::string> names;
  names.reserve(9);
  for (int i = 0; i < 9; ++i)
    names.push_back("L" + std::to_string(i));
  for (int i = 0; i < 9; ++i)
    entries.push_back(makeEntry(EJIT_SERVICE_REG_LIFECYCLE, names[i].c_str(),
                                nullptr, nullptr, 0));
  // 8 dimType slots exist; the 9th distinct lifecycle is a clean capacity
  // reject.
  EXPECT_EQ(registerOne(entries.data(), 9, "m"), EJIT_SERVICE_ERR_CAPACITY);
}

TEST_F(WorkerServiceTest, RegistrationFrozenAfterInit) {
  ASSERT_EQ(ejit_service_init(nullptr), EJIT_SERVICE_OK);
  ejit_service_reg_entry_t e =
      makeEntry(EJIT_SERVICE_REG_FUNC_INDEX, "late", nullptr, nullptr, 0);
  EXPECT_EQ(registerOne(&e, 1, "m"), EJIT_SERVICE_ERR_FROZEN);
}

TEST_F(WorkerServiceTest, TwoBusinessModulesSameService) {
  ejit_service_reg_entry_t a =
      makeEntry(EJIT_SERVICE_REG_FUNC_INDEX, "a", nullptr, nullptr, 0);
  ejit_service_reg_entry_t b =
      makeEntry(EJIT_SERVICE_REG_FUNC_INDEX, "b", nullptr, nullptr, 0);
  EXPECT_EQ(registerOne(&a, 1, "core0"), EJIT_SERVICE_OK);
  EXPECT_EQ(registerOne(&b, 1, "core1"), EJIT_SERVICE_OK);
  ASSERT_EQ(ejit_service_init(nullptr), EJIT_SERVICE_OK);
  ejit_service_diagnostics_t d;
  std::memset(&d, 0, sizeof(d));
  ASSERT_EQ(ejit_service_get_diagnostics(&d), EJIT_SERVICE_OK);
  EXPECT_EQ(d.moduleCount, 2u);
  EXPECT_EQ(d.funcIndexCount, 2u);
}

//===----------------------------------------------------------------------===//
// Sync compile path
//===----------------------------------------------------------------------===//

TEST_F(WorkerServiceTest, SyncCompileMissThenHit) {
  ejit_service_reg_entry_t fi =
      makeEntry(EJIT_SERVICE_REG_FUNC_INDEX, "f", nullptr, nullptr, 0);
  ASSERT_EQ(registerOne(&fi, 1, "m"), EJIT_SERVICE_OK);
  uint32_t idx = static_cast<uint32_t>(fi.value0);
  ejit_service_config_t c = makeConfig(EJIT_SERVICE_MODE_SYNC);
  ASSERT_EQ(ejit_service_init(&c), EJIT_SERVICE_OK);

  uint64_t before = ejit_service_test_compile_calls();
  void *fn = nullptr;
  uint32_t bucket = 0;
  // Miss -> inline compile -> OK with a real (mock) pointer.
  EXPECT_EQ(ejit_service_compile_or_get(idx, nullptr, 0, nullptr, &fn, &bucket),
            EJIT_SERVICE_OK);
  EXPECT_NE(fn, nullptr);
  EXPECT_EQ(ejit_service_test_compile_calls(), before + 1);
  ejit_service_release_read(bucket);

  // Second call -> cache hit -> no new compile.
  void *fn2 = nullptr;
  uint32_t bucket2 = 0;
  EXPECT_EQ(
      ejit_service_compile_or_get(idx, nullptr, 0, nullptr, &fn2, &bucket2),
      EJIT_SERVICE_OK);
  EXPECT_EQ(fn2, fn);
  EXPECT_EQ(ejit_service_test_compile_calls(), before + 1);
  ejit_service_release_read(bucket2);
}

TEST_F(WorkerServiceTest, SyncCompileFailurePropagates) {
  ejit_service_test_set_compile_fail(1);
  ejit_service_config_t c = makeConfig(EJIT_SERVICE_MODE_SYNC);
  ASSERT_EQ(ejit_service_init(&c), EJIT_SERVICE_OK);
  void *fn = nullptr;
  uint32_t bucket = 0;
  EXPECT_EQ(ejit_service_compile_or_get(3, nullptr, 0, nullptr, &fn, &bucket),
            EJIT_SERVICE_ERR_COMPILE_FAILED);
}

//===----------------------------------------------------------------------===//
// Async compile path (manual pump — no real thread)
//===----------------------------------------------------------------------===//

TEST_F(WorkerServiceTest, AsyncEnqueueReturnsPending) {
  ejit_service_config_t c = makeConfig(EJIT_SERVICE_MODE_ASYNC_MANUAL);
  ASSERT_EQ(ejit_service_init(&c), EJIT_SERVICE_OK);
  int sentinel = 0;
  void *fb = &sentinel;
  void *fn = nullptr;
  uint32_t bucket = 0;
  EXPECT_EQ(ejit_service_compile_or_get(5, nullptr, 0, fb, &fn, &bucket),
            EJIT_SERVICE_PENDING);
  EXPECT_EQ(fn, fb); // fallback returned while pending
}

TEST_F(WorkerServiceTest, AsyncWorkerCompileThenRetrieve) {
  ejit_service_config_t c = makeConfig(EJIT_SERVICE_MODE_ASYNC_MANUAL);
  ASSERT_EQ(ejit_service_init(&c), EJIT_SERVICE_OK);
  void *fn = nullptr;
  uint32_t bucket = 0;
  EXPECT_EQ(ejit_service_compile_or_get(5, nullptr, 0, nullptr, &fn, &bucket),
            EJIT_SERVICE_PENDING);
  EXPECT_EQ(ejit_service_worker_poll_one(), 1u); // worker compiles it
  // Now the result is retrievable.
  void *fn2 = nullptr;
  uint32_t bucket2 = 0;
  EXPECT_EQ(ejit_service_compile_or_get(5, nullptr, 0, nullptr, &fn2, &bucket2),
            EJIT_SERVICE_OK);
  EXPECT_NE(fn2, nullptr);
  ejit_service_release_read(bucket2);
}

TEST_F(WorkerServiceTest, SameKeyConcurrentCompilesOnce) {
  ejit_service_config_t c = makeConfig(EJIT_SERVICE_MODE_ASYNC_MANUAL);
  ASSERT_EQ(ejit_service_init(&c), EJIT_SERVICE_OK);
  uint64_t before = ejit_service_test_compile_calls();
  void *fn = nullptr;
  uint32_t bucket = 0;
  // Two producers, same key, before any drain -> dedup to one in-flight.
  EXPECT_EQ(ejit_service_compile_or_get(7, nullptr, 0, nullptr, &fn, &bucket),
            EJIT_SERVICE_PENDING);
  EXPECT_EQ(ejit_service_compile_or_get(7, nullptr, 0, nullptr, &fn, &bucket),
            EJIT_SERVICE_PENDING);
  ejit_service_worker_poll_budget(8);
  EXPECT_EQ(ejit_service_test_compile_calls(), before + 1); // compiled once
}

TEST_F(WorkerServiceTest, WorkerIdlePollDoesNoWork) {
  ejit_service_config_t c = makeConfig(EJIT_SERVICE_MODE_ASYNC_MANUAL);
  ASSERT_EQ(ejit_service_init(&c), EJIT_SERVICE_OK);
  EXPECT_EQ(ejit_service_worker_poll_one(), 0u);
  EXPECT_EQ(ejit_service_worker_poll_budget(16), 0u);
}

TEST_F(WorkerServiceTest, QueueFullFallsBackAndDedupRollsBack) {
  ejit_service_config_t c = makeConfig(EJIT_SERVICE_MODE_ASYNC_MANUAL);
  ASSERT_EQ(ejit_service_init(&c), EJIT_SERVICE_OK);
  // Flood distinct funcIndexes without draining; the bounded queue must
  // eventually report QUEUE_FULL rather than block or corrupt state.
  bool sawQueueFull = false;
  uint32_t fullIdx = 0;
  for (uint32_t i = 0; i < 4096; ++i) {
    void *fn = nullptr;
    uint32_t bucket = 0;
    ejit_service_status_t st =
        ejit_service_compile_or_get(i, nullptr, 0, nullptr, &fn, &bucket);
    if (st == EJIT_SERVICE_ERR_QUEUE_FULL) {
      sawQueueFull = true;
      fullIdx = i;
      break;
    }
    ASSERT_EQ(st, EJIT_SERVICE_PENDING);
  }
  ASSERT_TRUE(sawQueueFull);
  // Drain everything, then the rejected key can still be compiled (dedup for it
  // was rolled back — it is not stuck pending).
  while (ejit_service_worker_poll_budget(256) > 0) {
  }
  void *fn = nullptr;
  uint32_t bucket = 0;
  ejit_service_status_t st =
      ejit_service_compile_or_get(fullIdx, nullptr, 0, nullptr, &fn, &bucket);
  EXPECT_TRUE(st == EJIT_SERVICE_PENDING || st == EJIT_SERVICE_OK);
}

//===----------------------------------------------------------------------===//
// Lifecycle activate / deactivate
//===----------------------------------------------------------------------===//

TEST_F(WorkerServiceTest, ActivateDeactivateGatesCompile) {
  ejit_service_reg_entry_t lc =
      makeEntry(EJIT_SERVICE_REG_LIFECYCLE, "L", nullptr, nullptr, 0);
  ASSERT_EQ(registerOne(&lc, 1, "m"), EJIT_SERVICE_OK);
  uint32_t dt = static_cast<uint32_t>(lc.value0);
  ejit_service_config_t c = makeConfig(EJIT_SERVICE_MODE_ASYNC_MANUAL);
  ASSERT_EQ(ejit_service_init(&c), EJIT_SERVICE_OK);

  ejit_service_dim_t dim;
  dim.dimType = dt;
  dim.instanceId = 5;

  void *fn = nullptr;
  uint32_t bucket = 0;
  // Instances are enabled by default -> request accepted (pending).
  EXPECT_EQ(ejit_service_compile_or_get(2, &dim, 1, nullptr, &fn, &bucket),
            EJIT_SERVICE_PENDING);

  // Deactivate -> instance-disabled (also bumps the version so any in-flight
  // request for the now-stale version is dropped at the publish gate).
  ASSERT_EQ(ejit_service_deactivate("L", 5), EJIT_SERVICE_OK);
  EXPECT_EQ(ejit_service_compile_or_get(2, &dim, 1, nullptr, &fn, &bucket),
            EJIT_SERVICE_ERR_INSTANCE_DISABLED);

  // Re-activate -> requests accepted again.
  ASSERT_EQ(ejit_service_activate("L", 5), EJIT_SERVICE_OK);
  EXPECT_EQ(ejit_service_compile_or_get(2, &dim, 1, nullptr, &fn, &bucket),
            EJIT_SERVICE_PENDING);
}

TEST_F(WorkerServiceTest, ActivateUnknownLifecycleRejected) {
  ASSERT_EQ(ejit_service_init(nullptr), EJIT_SERVICE_OK);
  EXPECT_EQ(ejit_service_activate("nope", 0), EJIT_SERVICE_ERR_INVALID_PARAM);
}

//===----------------------------------------------------------------------===//
// free_code / release
//===----------------------------------------------------------------------===//

TEST_F(WorkerServiceTest, FreeCodeBestEffortKeepsServiceUsable) {
  ejit_service_config_t c = makeConfig(EJIT_SERVICE_MODE_SYNC);
  ASSERT_EQ(ejit_service_init(&c), EJIT_SERVICE_OK);
  void *fn = nullptr;
  uint32_t bucket = 0;
  ASSERT_EQ(ejit_service_compile_or_get(9, nullptr, 0, nullptr, &fn, &bucket),
            EJIT_SERVICE_OK);
  ejit_service_release_read(bucket);

  // free_code is best effort: it retires the code through the release callback
  // (a no-op when none is installed). The taskpool cache exposes no per-key
  // eviction, so it must not corrupt state or wedge the service.
  EXPECT_EQ(ejit_service_free_code(9, nullptr, 0), EJIT_SERVICE_OK);

  void *fn2 = nullptr;
  uint32_t bucket2 = 0;
  EXPECT_EQ(ejit_service_compile_or_get(9, nullptr, 0, nullptr, &fn2, &bucket2),
            EJIT_SERVICE_OK);
  ejit_service_release_read(bucket2);
}

//===----------------------------------------------------------------------===//
// Diagnostics / config round-trip (big-endian-safe field-by-field access)
//===----------------------------------------------------------------------===//

TEST_F(WorkerServiceTest, DiagnosticsReflectConfigAndState) {
  ejit_service_config_t c = makeConfig(EJIT_SERVICE_MODE_ASYNC_MANUAL);
  c.shareCodePointers = 1;
  ASSERT_EQ(ejit_service_init(&c), EJIT_SERVICE_OK);
  ejit_service_diagnostics_t d;
  std::memset(&d, 0, sizeof(d));
  ASSERT_EQ(ejit_service_get_diagnostics(&d), EJIT_SERVICE_OK);
  EXPECT_EQ(d.state, (uint32_t)EJIT_SERVICE_STATE_READY);
  EXPECT_EQ(d.mode, (uint32_t)EJIT_SERVICE_MODE_ASYNC_MANUAL);
  EXPECT_EQ(d.shareCodePointers, 1u);
  EXPECT_EQ(d.workerStackSize, 1048576ull); // each scalar field read directly
  EXPECT_NE(d.instanceAddress, 0ull);
  EXPECT_NE(d.queueAddress, 0ull);
  EXPECT_NE(d.cacheAddress, 0ull);
  EXPECT_EQ(d.generation, 1ull);
  EXPECT_GE(d.initCount, 1ull);
}

TEST_F(WorkerServiceTest, ShareCodePointersDefaultsOff) {
  ASSERT_EQ(ejit_service_init(nullptr), EJIT_SERVICE_OK);
  ejit_service_diagnostics_t d;
  std::memset(&d, 0, sizeof(d));
  ASSERT_EQ(ejit_service_get_diagnostics(&d), EJIT_SERVICE_OK);
  EXPECT_EQ(d.shareCodePointers, 0u);
}

//===----------------------------------------------------------------------===//
// Shutdown ordering (join worker before destroying the backend)
//===----------------------------------------------------------------------===//

TEST_F(WorkerServiceTest, ShutdownJoinsWorkerBeforeDestroy) {
  ejit_service_config_t c = makeConfig(EJIT_SERVICE_MODE_ASYNC);
  ASSERT_EQ(ejit_service_init(&c), EJIT_SERVICE_OK);
  // Queue a little work for the real worker, then shut down. The worker must be
  // joined before the backend is torn down (no crash / use-after-free).
  for (uint32_t i = 0; i < 8; ++i) {
    void *fn = nullptr;
    uint32_t bucket = 0;
    ejit_service_compile_or_get(i, nullptr, 0, nullptr, &fn, &bucket);
  }
  EXPECT_EQ(ejit_service_shutdown(), EJIT_SERVICE_OK);
  EXPECT_EQ(ejit_service_get_state(), EJIT_SERVICE_STATE_STOPPED);
  // After shutdown, compile is refused.
  void *fn = nullptr;
  uint32_t bucket = 0;
  EXPECT_EQ(ejit_service_compile_or_get(0, nullptr, 0, nullptr, &fn, &bucket),
            EJIT_SERVICE_ERR_NOT_READY);
}

TEST_F(WorkerServiceTest, ReinitAfterShutdown) {
  ejit_service_config_t c = makeConfig(EJIT_SERVICE_MODE_SYNC);
  ASSERT_EQ(ejit_service_init(&c), EJIT_SERVICE_OK);
  ejit_service_identity_t a;
  std::memset(&a, 0, sizeof(a));
  ASSERT_EQ(ejit_service_get_identity(&a), EJIT_SERVICE_OK);
  ASSERT_EQ(ejit_service_shutdown(), EJIT_SERVICE_OK);
  ASSERT_EQ(ejit_service_init(&c), EJIT_SERVICE_OK);
  ejit_service_identity_t b;
  std::memset(&b, 0, sizeof(b));
  ASSERT_EQ(ejit_service_get_identity(&b), EJIT_SERVICE_OK);
  // Same in-library singleton; a fresh generation.
  EXPECT_EQ(a.instanceAddress, b.instanceAddress);
  EXPECT_EQ(b.generation, 2ull);
}

//===----------------------------------------------------------------------===//
// Invalid-parameter guards
//===----------------------------------------------------------------------===//

TEST_F(WorkerServiceTest, CompileRejectsTooManyDims) {
  ASSERT_EQ(ejit_service_init(nullptr), EJIT_SERVICE_OK);
  ejit_service_dim_t dims[5];
  std::memset(dims, 0, sizeof(dims));
  void *fn = nullptr;
  uint32_t bucket = 0;
  EXPECT_EQ(ejit_service_compile_or_get(1, dims, 5, nullptr, &fn, &bucket),
            EJIT_SERVICE_ERR_INVALID_PARAM);
}

TEST_F(WorkerServiceTest, NullOutParamsRejected) {
  EXPECT_EQ(ejit_service_get_identity(nullptr), EJIT_SERVICE_ERR_INVALID_PARAM);
  EXPECT_EQ(ejit_service_get_diagnostics(nullptr),
            EJIT_SERVICE_ERR_INVALID_PARAM);
  EXPECT_EQ(ejit_service_register_module(nullptr),
            EJIT_SERVICE_ERR_INVALID_PARAM);
}

} // namespace
