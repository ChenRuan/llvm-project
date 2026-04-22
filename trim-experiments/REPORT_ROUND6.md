# Round 6 — Extending the light AArch64 backend to a real EasyJIT C API test

## Goal

Take the round-5 tiny AArch64 emitter (add_int only, 1-BB, integer-only) and
grow it just enough to actually execute a real EasyJIT-specialized function,
starting with `partial_struct_binding`. Stay source-level, keep
LLVMCodeGen / SelectionDAG / AArch64CodeGen / MC / RuntimeDyld / OrcJIT out of
the link, keep the thing single-file, and be endian-aware from day one.

## What was dumped

Re-enabled the `EASYJIT_DUMP_IR=<path>` hook in
`easy-jit-llvm15/runtime/Function_compile.cpp` (after `MPM.run(M)`), rebuilt
`libEasyJitRuntime.a`, then ran each candidate test statically linked against
the full LLVM backend with the env var set.

| Test                       | Runs JIT? | Dumped IR shape (after EasyJIT specialization) |
|---                         |---        |---                                             |
| `partial_struct_binding`   | yes       | 1 func, 4 BBs, alloca + memcpy(8) + GEP + load/store + icmp + cond-br + phi + add/sub/ret |
| `config_process_base`      | **no**    | Pure baseline — does not call `easyjit_compile` |
| `config_process_easyjit`   | yes       | 1 func, 1 BB, literally `ret i32 15` (const-folded by specialization) |

So the real capability target is driven by `partial_struct_binding`.
`config_process_easyjit` is a freebie that the round-5 PoC could already
handle. `config_process_base` is irrelevant to the codegen story.

## Capability gap analysis (partial_struct_binding.spec.ll)

| IR feature (observed)                               | Round-5 | Round-6 |
|---                                                  |---      |---      |
| `alloca %struct.PartialConfig, align 8`             | ✗       | ✓ static alloca only |
| `call void @llvm.memcpy.p0.p0.i64(..., i64 8, …)`   | ✗       | ✓ const N ≤ 32B, multiple of 4B |
| `getelementptr %T, ptr %alloca, i32 0, i32 1`       | ✗       | ✓ constant offsets on stack ptrs |
| `store i32 7, ptr %gep` (imm value)                 | ✗       | ✓ via MOVZ into x16 |
| `load i32, ptr %gep` / `load i32, ptr %alloca`      | ✗       | ✓ uimm12-scaled, sp-rel |
| `icmp ne i32 %5, 0`                                 | ✗       | ✓ fused into following cond-br |
| `br i1 %6, label %7, label %11`                     | ✗       | ✓ B.cond + B (imm19/imm26 backpatched) |
| `br label %.exit`                                   | ✗       | ✓ |
| Multi-BB layout                                     | ✗       | ✓ two-pass via `bbStart` + fixups |
| `phi i32 [ %10, %7 ], [ %13, %11 ]`                 | ✗       | ✓ pre-assigned reg, copy at each predecessor |
| `add nsw / sub nsw i32`                             | ✓       | ✓ |
| `ret i32 %14` with stack-frame restore              | partial | ✓ `add sp, sp, #frame; ret` |

## New encoders added to `light_aarch64.cpp`

- `encLdrStrUI(load, size∈{2,3}, rt, rn, imm12)` — `LDR/STR` (unsigned-offset, 32/64-bit)
- `encSubsImm32(rn, imm12)` — `CMP (imm)` on w-reg
- `encSubsReg32(rn, rm)` — `CMP (reg)` on w-reg
- `encBcondStub(cond)` / `encBStub()` — placeholders backpatched with imm19/imm26

Predicate→cond mapping covers all signed/unsigned `icmp` forms.

## Emitter restructure

Round-5 was a single pass over the entry BB. Round-6 splits into:

1. **Pass 0** — scan allocas, compute `frameSize`, lay out each alloca at a
   fixed sp-relative offset. Emit prologue `sub sp, sp, #frame` iff
   `frameSize > 0`.
2. **Pass 1** — pre-assign scratch registers for every integer-result SSA
   value. Function-arg values keep their ABI reg slot (x0..x7). Phi nodes
   are assigned up-front so predecessors can copy into them without having
   visited the phi's block yet.
3. **Pass 2** — pointer-location analysis. Every pointer-typed SSA value is
   classified as either `InReg{reg, off=0}` (function-arg pointers) or
   `StackRel{spOff}` (allocas and their constant-index GEPs / bitcasts).
4. **Pass 3** — linear code emission. Records `bbStart[BB] = W.pos` per
   block and collects `{patchPos, target, isBcond, cond}` fixups for every
   branch.
5. **Backpatch pass** — walks fixups, writes final imm19 / imm26 offsets.

The `icmp` + `br i1` fusion is key: the `icmp` instruction itself emits no
code — it just records `(lhsReg, rhsImm-or-rhsReg, predicate)`, which the
subsequent conditional `br` consumes. If an `icmp` result is used elsewhere
(not the next br) we bail out with `Unsupported`. Good enough for this IR
shape; a real backend would need selection-based materialization.

## Cond-br layout (and why it's inverted)

The standard shape I emit for `br i1 %c, %T, %F`:

```
    B.cond <inv>, FALSE_LABEL
    <phi copies for T>
    B TRUE
FALSE_LABEL:
    <phi copies for F>
    B FALSE
```

`<inv>` is the original cond-code with its low bit flipped (EQ↔NE, LT↔GE,
LE↔GT, LO↔HS, LS↔HI), because we want to fall through into the TRUE-edge
phi copies. The B.cond's imm19 is written at emit time as a stub, then
patched with the actual delta once we know where FALSE_LABEL landed. This
keeps the emitter single-pass with a trivial fixup table.

## Phi SSA-destruction

Dead-simple scheme, since we can afford to be wasteful:

- During Pass 1, every phi gets its own scratch register (e.g. `%14 → x15`).
- At the END of every predecessor BB (just before the terminator is fully
  emitted), walk the successor's phi list; for each phi, `mov phiReg,
  incomingReg` (or MOVZ if the incoming is a ConstantInt ≤ 16-bit unsigned).
- No lost-copy / swap problems arise because we only allocate ONE register
  per phi and never read from any other phi's register here.

For the partial_struct IR, the phi reads `%10` (x12) from BB7 and `%13`
(x14) from BB11. The predecessor emits the mov, the `.exit` block simply
uses x15 as the return value source. Total extra moves: 2.

## Result — partial_struct_binding

```
$ ./out/light_partial_struct ../ir_dumps/partial_struct_binding.spec.ll \
                              eval_partial_config
[light] emitted 88 bytes of aarch64 machine code
[light] bytes: ff 43 00 d1 11 00 40 f9 f1 03 00 f9 f0 00 80 52
               f0 07 00 b9 ea 07 40 b9 5f 01 00 71 40 00 00 54
               02 00 00 14 06 00 00 14 eb 03 40 b9 2c 00 0b 0b
               8d 01 0a 0b e9 03 0d 2a 05 00 00 14 ee 03 40 b9
               2f 00 0e 4b e9 03 0f 2a 01 00 00 14 e0 03 09 2a
               ff 43 00 91 c0 03 5f d6
light_partial_struct.result_a=20 (expect 20)
light_partial_struct.result_b=26 (expect 26)
light_partial_struct: PASS
```

Disassembly walk-through (22 insns, 88 B):

```
 0  d10043ff  sub   sp, sp, #0x10            ; prologue: 16B frame
 4  f9400011  ldr   x17, [x0]                ; memcpy word 0 (x17 = *(u64)x0)
 8  f90003f1  str   x17, [sp]                ; memcpy: [sp+0] = x17
 c  528000f0  movz  w16, #7                  ; materialize store-imm 7
10  b90007f0  str   w16, [sp, #4]            ; store i32 7 → fixed_flag
14  b94007ea  ldr   w10, [sp, #4]            ; %5 = load %4 (fixed_flag)
18  7100015f  subs  wzr, w10, #0             ; icmp ne 0  (fused)
1c  54000040  b.eq  28  (FALSE_LABEL)
20  14000002  b     28+0x8 = TRUE block      ; (T phi copy trivial: none)
24  14000006  b     .exit                    ; <- in FALSE branch too below
          ... actual decoded seq:
   bb7 (true):     ldr w11,[sp,#0]; add w11,w1,w11; add w13,w11,w10; mov w9,w13; b .exit
   bb11 (false):   ldr w14,[sp,#0]; sub w15,w1,w14; mov w9,w15;     b .exit
   .exit:          mov w0,w9; add sp,sp,#0x10; ret
```

Matches what the source says, produces the expected return values for both
runtime configs (dynamic_value=3 → 10+3+7=20; dynamic_value=9 → 10+9+7=26).

## Binary size

| Variant                                       | text (B)    | Δ vs round-4 LLVM backend |
|---                                            |---          |---                        |
| Round-4 "kitchen sink" LLVM backend           | 21,211,843  | baseline                  |
| Round-5 `light_add_int` (1-BB PoC)            |  3,383,909  | −84.0%                    |
| Round-6 `light_partial_struct`                |  **3,417,453** | **−83.9%** (+33.5 KB over round-5) |
| Round-6 `light_add_int` (same emitter, rebuilt) |  3,429,605 | −83.8%                    |

The +33.5 KB over round-5's `light_add_int` is almost entirely pulled in from
`LLVMCore` (the new emitter references `DataLayout`, `GEPOperator::accumulate
ConstantOffset`, `PHINode`, `IntrinsicInst`, `BranchInst`, `StoreInst`,
`LoadInst`, `ICmpInst`). The emitter's own `.text` is only a few KB.

The emitter-only code is ~540 lines of C++.

## Endian awareness (aarch64_be)

Every LE-only assumption site is tagged in the source. Summary:

| Site                                                  | LE assumption |
|---                                                    |---            |
| `Writer::emit()` — `memcpy(buf, &uint32_t, 4)`        | Instruction words are little-endian; aarch64_be would need byte-reversed words (same bits in-register, different memory layout) — actually AArch64 instruction stream is **always** LE regardless of data endian, so this site is endian-neutral in practice but remains tagged for clarity |
| All `LDR/STR` emissions (`encLdrStrUI`)               | AArch64 `LDR` reads in CPU data endian; an aarch64_be kernel+program would load bytes in the opposite order. Integer loads of integer SSA values are fine (whole word), but `memcpy` lowered as LDR-x / STR-x is **only** correct when SRC and DST are the same endian (they always are on a single CPU), so this is safe for whole-word copies. **Unsafe** if we ever lower a sub-word memcpy through int registers on aarch64_be. |
| `emitMemcpy` requiring `(N % 4) == 0`                 | Dodges the sub-byte case entirely, so endian-safe as currently written |
| `asImm12` / `encMovz16` / MOVZ materialization         | Immediate encoding is independent of data endian |
| `constGepOffset` via `DataLayout`                      | DL comes from the IR module (`e-...` = LE); on aarch64_be IR the triple + DL would be `E-...` and we reject the module before any of this runs |

Up-front rejection at the top of `emit()`:

```cpp
if (Triple.rfind("aarch64_be", 0) == 0) {
  r.status = Status::NotAarch64LE;
  r.reason = "aarch64_be not supported (PoC is LE-only)";
  return r;
}
```

**To port to aarch64_be**: the only real hazard is any code path that uses
GPRs as a byte-buffer via `memcpy` lowering. Current `emitMemcpy` is fine
(whole-word copies only). If we later add byte/halfword memcpy chunks, those
**must** use `LDRB`/`STRB` / `LDRH`/`STRH` (which are per-byte and hence
endian-agnostic for single-unit access), not packed `LDR x` slicing.

## What would extend this next (NOT this round)

Based on the other dumps I skimmed but did not target:

- **`array_snapshot.c`** — loops (back-edges handled; ok), but array-of-struct
  GEPs with a variable loop-induction-var index. Need a real register-based
  pointer-arithmetic path: `add xdst, xbase, xidx, lsl #log2(size)`.
- **`pointer_field_snapshot.c`** — loads through pointer-of-pointer; need
  `PtrLoc{InReg, reg, off}` with non-zero off → `ldr xN, [xM, #off]`.
- **`struct_snapshot.c`** — larger memcpy (likely > 32B); need a short
  unrolled LDP/STP sequence, or a small loop with ADD+SUBS+B.NE.
- **`wireless_beamform.c`** — almost certainly float/vector; out of scope for
  any reasonable "narrow emitter" — this is where the full backend still
  wins decisively.
- **Calls to externs** (e.g. memcpy runtime fallback, printf, libc math) —
  need a PLT-free direct-B via 28-bit imm + a way to resolve runtime symbols.
  This is the single biggest remaining gap before a general EasyJIT replacement.

## Commits

- `trim-experiments/light_codegen/light_aarch64.cpp` rewritten (round-6)
- `trim-experiments/light_codegen/light_aarch64.h` — updated capability comment
- `trim-experiments/light_codegen/light_partial_struct.cpp` — new driver
- `trim-experiments/light_codegen/build_partial_struct.sh` — new build script
- `trim-experiments/light_codegen/light_aarch64.cpp.round5` — round-5 archive
- `trim-experiments/dump_ir_test.sh` — new harness for IR dumping
- `easy-jit-llvm15/runtime/Function_compile.cpp` — `EASYJIT_DUMP_IR` hook

## Status

✅ **partial_struct_binding runs end-to-end through the light emitter, both
runtime inputs produce the expected values.**  Round-5 add_int still passes
(no regression). LLVM backend (LLVMCodeGen / SelectionDAG / AArch64CodeGen /
MC / OrcJIT / RuntimeDyld) remains entirely out of the link.

Size delta from round-5 PoC: **+33,544 B** (almost all from LLVMCore's GEP /
PHI / Instruction APIs). Total fully-static text: **3,417,453 B**, still a
**6.2× reduction** vs the round-4 LLVM-backend baseline.
