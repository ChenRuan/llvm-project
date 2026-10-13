# Local ejit_free_dim value boundary (issue 246, v1)

Baseline: e09df842062735ba9f6279ae7d1c5b43fd723bec.

## Encoding frozen before implementation

Use a compiler-private, typed identity operation encoded as a direct call:
`%local = call i64 @__ejit_local_free_dim_v1_i64(i64 %initializer)`.
Its declaration has string attribute `"ejit.local_free_dim"="v1"`, memory(none),
nounwind, willreturn, nocallback and nosync. The consumer checks exact reserved
name, integer scalar signature, declaration-only shape and version attribute.
This is a pseudo-intrinsic, not a runtime function or SDK API. It deliberately
has no LLVM-known identity simplification: SROA/mem2reg promote its RESULT,
CSE can merge identical marked operations but cannot equate an unmarked input
to a marked result. No global facts about the initializer input are introduced.
Integer width is encoded in the name; no target register constraint is needed.

Insertion is after normal initializer evaluation and before the initializing
store, in CodeGenFunction::EmitScalarInit. Constant-expression emission of
marked locals is disabled so it cannot bypass this boundary. Effects of calls,
volatile/atomic source reads and cleanup expressions are unchanged.

## Extraction and cleanup

EJitRegisterBitcodePass runs before host optimization. Its extracted clone keeps
the operation through its actual SROA, AlwaysInline/cost-based O2 inliner,
mem2reg, EarlyCSE and InstCombine sequence (and lighter PGO extraction mode).
After extraction, an unconditional EJitLowerFreeDimPass in every PassBuilder
AOT pipeline lowers remaining calls to their actual input, even when EJIT
extraction/wrappers are disabled. No marker declaration or symbol registration
may remain in the AOT module.

EJitStructFieldPass::evalWithAssumed recognizes only the operation RESULT and
evaluates it as integer zero without walking/evaluating its initializer.
The original IR, initializer, GEP and ordinary uses are never rewritten to zero.
Existing witness bounds, poison/overflow, alignment, field eligibility, object
extent and registry/lifecycle checks remain authoritative. Unknown PHI/call
chains safely miss folding. Marker inputs cannot authorize sibling SSA uses.

EJitOptimizer keeps the boundary until its last may_const consumption:
Baseline/PGOUse after runOptimizationPipeline; Instrumented after counter
capture and all may_const handling. Each return lowers calls to actual inputs
and clears analyses. The Tier1/Tier2 common profile prefix retains matching
marker shape; cleanup after counter capture must not alter profile CFG.

An old consumer cannot understand the private operation: the new AOT does not
register it as a host symbol, so an old JIT fails unresolved-symbol compilation
and retains AOT fallback. New bitcode is not promised compatible with old
packages. No cache key, ABI, SDK/CMake feature requirement or sampling axis is
added.

## Declaration validation

Retain parameter diagnostics/merge behavior. Extend subject to Var but reject
global/static/thread storage, noninteger, volatile/atomic and non-entry locals.
After the full body is available, require initialization and a managed dimension,
and reject every use other than an lvalue-to-rvalue read (parentheses allowed),
including assignment/increment/address/reference escape and closure capture.
Const and unmodified nonconst integer locals use the same boundary.

## Expected IR

Before specialization:
```llvm
%source = load i64, ptr @runtime_slot
%offset = call i64 @__ejit_local_free_dim_v1_i64(i64 %source)
%marked = getelementptr i8, ptr %cell_base, i64 %offset
%mode = load i32, ptr %marked, !ejit.may_const !0
%raw = getelementptr i8, ptr %cell_base, i64 %source
%unmarked = load i32, ptr %raw, !ejit.may_const !0
```

After successful witness consumption and mandatory cleanup:
```llvm
; mode is replaced by the generation's baked value, e.g. 7.
; raw/unmarked and non-may_const loads still use %source.
; all surviving offset computations also use %source, never zero.
```

Zero must designate the same semantic field and preserve cell/TRP/bound-pointer
identity. All legal offsets must yield the same field value for the whole
managed generation. A numerically in-bounds different field is still an invalid
business promise, not something this compiler can fully prove.

## Verification gates

Add Sema C/C++ positive/negative coverage, constant-local emission, O2 extraction
and no-marker AOT output, real optimizer/ORC Baseline/PGO tests, same-SSA mixed
uses, side effects, witness rejection, cell independence and generation refresh.
Keep audit and diagnostics ON. No value verifier exists in this frozen baseline
(PR237 is independent); verifier acceptance is unavailable, not passed.
Supply single-C worker6/owner16 demo using existing initialization. Host/BE
compile/link is not SDK final-link or actual board acceptance. LFD01-LFD14
remain pending until backed by individually recorded evidence.
