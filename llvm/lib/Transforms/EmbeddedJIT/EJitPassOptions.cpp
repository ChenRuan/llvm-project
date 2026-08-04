//===-- EJitPassOptions.cpp - Shared Command-Line Options -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/CommandLine.h"
using namespace llvm;

cl::opt<bool> EnableEJitGlobalCtors(
    "enable-ejit-global-ctors", cl::init(true), cl::Hidden,
    cl::desc("Generate llvm.global_ctors for auto-registration "
             "(disable for bare-metal / testing)"));

cl::opt<std::string> EJitDumpBitcodeDir(
    "ejit-dump-bitcode-dir", cl::init(""), cl::Hidden,
    cl::desc("Directory to dump extracted EmbeddedJIT bitcode (.bc and .ll) "
             "at AOT compile time, for debugging symbol extraction. Each TU "
             "writes a PID + module-named file so parallel -j builds do not "
             "collide or serialize."));

// AOT specialization diagnostics (PASS1). Both default on; each guards its own
// warning so users can silence them independently via -mllvm.
cl::opt<bool> EJitWarnNoSpecialization(
    "ejit-warn-no-specialization", cl::init(true), cl::Hidden,
    cl::desc("Warn when an ejit_entry function reads no ejit_may_const field "
             "in its specialization closure (no JIT specialization value)."));

cl::opt<bool> EJitWarnUnusedDim(
    "ejit-warn-unused-dim", cl::init(true), cl::Hidden,
    cl::desc("Warn when an ejit_entry declares ejit_period_arr_ind(P) but its "
             "specialization closure never indexes an ejit_period_arr(P) "
             "(unused specialization dimension)."));

// Optional info-level report (not a warning): per ejit_entry, count the
// ejit_may_const reads in its specialization closure and how many sit inside
// loops. A raw count is a poor specialization-value metric on its own (a
// single read in a hot loop or feeding a branch is high value), so this only
// reports the numbers for the user to judge - it does not gate or warn.
cl::opt<bool> EJitReportMayConst(
    "ejit-report-mayconst", cl::init(false), cl::Hidden,
    cl::desc("Report per-ejit_entry ejit_may_const read counts (total and "
             "in-loop) in the specialization closure, for manual "
             "specialization-value assessment."));
