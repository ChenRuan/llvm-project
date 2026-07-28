// Driver test for -fejit-cross-inline (PR #102 hardening).
//
// ld.lld is the single owner of cross-TU inline processing. The clang driver
// never performs the merge itself: it requires ld.lld and forwards the request
// via --ejit-cross-inline; with any other linker it is a hard error; and it
// still conflicts with -flto.

// With ld.lld: forward --ejit-cross-inline to the linker, and pass the cc1
// -ejit-cross-inline that makes the pass emit the .ejit_cross section.
// RUN: %clang -### --target=x86_64-unknown-linux-gnu -fuse-ld=lld \
// RUN:     -fejit-cross-inline %s 2>&1 | FileCheck %s --check-prefix=LLD
// LLD: "-cc1"
// LLD: "-ejit-cross-inline"
// LLD: "--ejit-cross-inline"

// With a non-lld linker: hard error, no silent .bc / leftover .ejit_cross.
// RUN: not %clang -### --target=x86_64-unknown-linux-gnu -fuse-ld=gold \
// RUN:     -fejit-cross-inline %s 2>&1 | FileCheck %s --check-prefix=NOLLD
// NOLLD: invalid argument '-fejit-cross-inline' only allowed with '-fuse-ld=lld'

// Conflicts with -flto at compile time.
// RUN: not %clang -### --target=x86_64-unknown-linux-gnu -fuse-ld=lld -flto \
// RUN:     -fejit-cross-inline %s 2>&1 | FileCheck %s --check-prefix=LTO
// LTO: invalid argument '-fejit-cross-inline' not allowed with '-flto'

// Default (no -fejit-cross-inline): neither the cc1 flag nor the linker flag
// appears, so behaviour is unchanged.
// RUN: %clang -### --target=x86_64-unknown-linux-gnu -fuse-ld=lld \
// RUN:     %s 2>&1 | FileCheck %s --check-prefix=OFF
// OFF-NOT: "-ejit-cross-inline"
// OFF-NOT: "--ejit-cross-inline"

int main(void) { return 0; }
