// RUN: %clang_cc1 -fsyntax-only -verify %s

// Same behavior in C: a definition that inherits ejit_entry from a prototype
// without repeating the attribute is warned about and not JIT-specialized.

__attribute__((ejit_entry)) int entry_fn(void); // header declaration
// expected-note@-1 {{'ejit_entry' declared here}}
int entry_fn(void) { return 1; } // expected-warning {{function 'entry_fn' is declared with 'ejit_entry' but defined without it; EmbeddedJIT specialization is disabled for this definition}}

// No warning when the definition repeats the attribute.
__attribute__((ejit_entry)) int ok_fn(void);
__attribute__((ejit_entry)) int ok_fn(void) { return 2; }
