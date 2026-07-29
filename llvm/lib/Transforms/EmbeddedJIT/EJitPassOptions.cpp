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

