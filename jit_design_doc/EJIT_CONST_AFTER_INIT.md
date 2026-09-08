# EJIT constants established after initialization

`EJIT_CONST_AFTER_INIT` marks a file-scope scalar whose final value is written
by application initialization and never changes before or during EJIT use.
Unlike a C `const` initializer, the final value is read from the live program
address when a JIT compilation runs.

```c
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"

EJIT_CONST_AFTER_INIT unsigned g_algorithm;

void application_init(void) {
  g_algorithm = discover_algorithm();
}

EJIT_ENTRY int process(int x) {
  return g_algorithm == 3 ? fast_path(x) : generic_path(x);
}
```

## Lifetime and ordering contract

The application owns synchronization. It must satisfy all of these conditions:

1. Finish every write to the object before the first call that can trigger JIT
   compilation of an entry which reads it.
2. Publish those writes to the worker and every other compilation core.
3. Keep the same storage address alive and mapped for the process lifetime.
4. Never write the object again, including through an alias, another
   translation unit, or another core.

`ejit_init()` may run before the final application write because it registers
the address, not the value. The hard boundary is the first JIT-triggering call.
Violating the contract can produce a stale specialized value; there is no hit
path check, cache dimension, version key, or automatic deoptimization.

Only external-linkage variables are supported. Annotate every translation
unit's visible `extern` declaration that is read by JIT code; the attribute is
inherited by later redeclarations within that translation unit. File-scope
`static`, anonymous-namespace, local-static, and thread-local objects are
rejected. The runtime registry is process-wide and keyed by external symbol
name, so internal-linkage objects cannot be identified safely across modules.

## Type matrix

| Type | Status |
| --- | --- |
| `_Bool`/`bool`, character and standard signed/unsigned integers | Supported |
| Typedefs of supported scalar types and enumerations | Supported |
| `__int128` and target-supported extended integers | Supported |
| `float`, `double`, `long double` | Supported when the target lowers the type |
| Other target-supported real floating types | Supported |
| Pointer, array, record, vector, complex | Compile-time error |
| `volatile`, `_Atomic`/`std::atomic`, thread-local storage | Compile-time error |

Integer and floating values are reconstructed from memory with the module's
target `DataLayout`. The implementation does not convert through host
`uint64_t` or `double`, so wide integers, target endianness, negative zero,
infinity, and NaN payload bits are preserved.

## Pipeline behavior

Clang emits three pieces of information:

- `!ejit.may_const` on ordinary loads of the annotated variable;
- an `ejit_const_after_init` marker on the LLVM global;
- the existing `ejit_period("static")` metadata used to register its address.

The AOT global stays mutable. PASS2 emits the existing static-variable registry
entry, and PASS1 keeps the referenced global as an external declaration in the
embedded bitcode. During Baseline or PGO Tier-1/Tier-2 compilation, PASS6 reads
the current bytes through that registered address and substitutes an LLVM
constant. The normal optimization pipeline can then eliminate dependent loads,
branches, and dead code.

The dedicated global marker lets PASS1 restore load metadata if AOT-time
optimization replaced the original load. Restoration is conservative: the
load must be non-volatile, non-atomic, rooted at the marked scalar, at offset
zero, and have the same storage size.

## Build impact

This changes a Clang attribute and emitted bitcode, so rebuild Clang, the EJIT
library, and the business package. The runtime C ABI and shared taskpool layout
do not change.

## Board demo

`ejit_test/ejit_const_after_init_sre_multicore_test.c` is a self-contained SRE
demo with no header dependency. Reset the board, run `test_ejit_period` on core
6 to start the fixed worker and arm capture, then run it on core 16. Core 16
finalizes three globals before its first entry call, waits for PGO Tier-1 and
Tier-2 compilation and publication, and verifies additional
calls after Tier-2 is ready. Run `test_ejit_period` on core 6 again to print the
captured function and module views. This demo validates only the PGO path; the
ordinary non-PGO initialization path is covered by host-side tests rather than
this board acceptance scenario.
