//===-- EJitSreConfig.h - SRE build-time configuration ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITSRECONFIG_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITSRECONFIG_H

#include <cstddef>
#include <cstdint>

#ifndef EJIT_SRE_CODE_POOL_SIZE
#define EJIT_SRE_CODE_POOL_SIZE                                                \
  (static_cast<unsigned long long>(2) * 1024 * 1024)
#endif

#ifndef EJIT_SRE_CODE_POOL_PTNO
#define EJIT_SRE_CODE_POOL_PTNO 8
#endif

#ifndef EJIT_SRE_CODE_POOL_MID
#define EJIT_SRE_CODE_POOL_MID 0
#endif

#ifndef EJIT_SRE_TASK_PRIORITY
#define EJIT_SRE_TASK_PRIORITY 20u
#endif

#ifndef EJIT_SRE_TASKPOOL_WORKER_STACK_SIZE
#define EJIT_SRE_TASKPOOL_WORKER_STACK_SIZE (1024u * 1024u)
#endif

namespace llvm {
namespace ejit {
namespace sre_config {

constexpr std::size_t k2MiB = static_cast<std::size_t>(2) * 1024 * 1024;
constexpr std::size_t k4KiB = static_cast<std::size_t>(4) * 1024;

constexpr unsigned long long kCodePoolSize = EJIT_SRE_CODE_POOL_SIZE;
constexpr unsigned char kCodePoolPtNo =
    static_cast<unsigned char>(EJIT_SRE_CODE_POOL_PTNO);
constexpr unsigned kCodePoolMid = static_cast<unsigned>(EJIT_SRE_CODE_POOL_MID);

constexpr unsigned kTaskPriority = static_cast<unsigned>(EJIT_SRE_TASK_PRIORITY);
constexpr std::uint32_t kWorkerStackSize =
    static_cast<std::uint32_t>(EJIT_SRE_TASKPOOL_WORKER_STACK_SIZE);

static_assert(EJIT_SRE_TASKPOOL_WORKER_STACK_SIZE > 0u,
              "worker stack size must be non-zero");
static_assert(
    EJIT_SRE_TASKPOOL_WORKER_STACK_SIZE % 16u == 0u,
    "worker stack size must be 16-byte aligned (AArch64 SP alignment)");
static_assert(
    static_cast<unsigned long long>(EJIT_SRE_TASKPOOL_WORKER_STACK_SIZE) <=
        0xFFFFFFFFull,
    "worker stack size must fit the 32-bit TSK_INIT_PARAM_S.uwStackSize");

} // namespace sre_config
} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITSRECONFIG_H
