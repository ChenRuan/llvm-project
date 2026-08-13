// RUN: %clang_cc1 -fsyntax-only -verify %s

// EmbeddedJIT: a function declared with ejit_entry / ejit_period_lc in a
// header but defined without the attribute is not JIT-specialized. The
// definition must repeat the attribute; otherwise a warning is emitted and
// the inherited attribute is dropped, so the definition compiles as a plain
// AOT function (see clang/test/CodeGen/ejit_attr_missing_on_definition.c).

// --- Warning: declared with ejit_entry, defined without it ---

__attribute__((ejit_entry)) int entry_fn(void); // header declaration
// expected-note@-1 {{'ejit_entry' declared here}}
int entry_fn(void) { return 1; } // expected-warning {{function 'entry_fn' is declared with 'ejit_entry' but defined without it; EmbeddedJIT specialization is disabled for this definition}}

// --- Warning: declared with ejit_period_lc, defined without it ---

__attribute__((ejit_period_lc("static")))
void lc_fn(__attribute__((ejit_period_arr_ind("static"))) int idx);
// expected-note@-2 {{'ejit_period_lc' declared here}}
void lc_fn(int idx) {} // expected-warning {{function 'lc_fn' is declared with 'ejit_period_lc' but defined without it; EmbeddedJIT specialization is disabled for this definition}}

// --- Definition repeats ejit_entry but omits ejit_period_lc: warn only for lc ---

__attribute__((ejit_entry, ejit_period_lc("static")))
void mixed_fn(__attribute__((ejit_period_arr_ind("static"))) int idx);
// expected-note@-2 {{'ejit_period_lc' declared here}}
__attribute__((ejit_entry)) void mixed_fn(int idx) {} // expected-warning {{function 'mixed_fn' is declared with 'ejit_period_lc' but defined without it; EmbeddedJIT specialization is disabled for this definition}}

// --- No warning: definition repeats the attribute ---

__attribute__((ejit_entry)) int ok_entry_fn(void);
__attribute__((ejit_entry)) int ok_entry_fn(void) { return 2; }

__attribute__((ejit_period_lc("static")))
void ok_lc_fn(__attribute__((ejit_period_arr_ind("static"))) int idx);
__attribute__((ejit_period_lc("static")))
void ok_lc_fn(__attribute__((ejit_period_arr_ind("static"))) int idx) {}

// --- No warning: mere redeclaration without attribute (no definition here) ---

__attribute__((ejit_entry)) int redecl_fn(void);
int redecl_fn(void);

// --- No warning: no prior declaration with the attribute ---

int plain_fn(void) { return 3; }

// --- Warning: out-of-line member definition without the attribute ---

struct Worker {
  __attribute__((ejit_entry)) void run(int idx); // in-class declaration
};
// expected-note@-2 {{'ejit_entry' declared here}}
void Worker::run(int idx) {} // expected-warning {{function 'run' is declared with 'ejit_entry' but defined without it; EmbeddedJIT specialization is disabled for this definition}}

// --- Warning once at the template pattern; explicit instantiations are
//     handled silently (no duplicate warning) ---

template <class T> __attribute__((ejit_entry)) T tmpl_fn(T v);
// expected-note@-1 {{'ejit_entry' declared here}}
template <class T> T tmpl_fn(T v) { return v; } // expected-warning {{function 'tmpl_fn' is declared with 'ejit_entry' but defined without it; EmbeddedJIT specialization is disabled for this definition}}
template int tmpl_fn(int); // instantiates the body: no extra warning

// --- Warning: explicit specialization is a definition of its own ---

template <class T> __attribute__((ejit_entry)) T spec_fn(T v);
// expected-note@-1 {{'ejit_entry' declared here}}
template <> int spec_fn<int>(int v) { return v; } // expected-warning {{function 'spec_fn<int>' is declared with 'ejit_entry' but defined without it; EmbeddedJIT specialization is disabled for this definition}}
