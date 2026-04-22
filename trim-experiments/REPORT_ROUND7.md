# Round 7 Report — Lightweight Codegen: Real External-Global Resolution

**Branch:** `llvm15_trim_light_codegen_explore` (tree at `llvm-project-15.0.4`)
**Scope (user directive):** implement the real host-symbol→host-address path
in the lightweight aarch64 emitter — **not** the IR-embedding workaround from
round-7 step A. Make `global_snapshot` and `global_partial_snapshot` tests
run on their unmodified dumped IR.

---

## 1. New emitter capabilities

**`light::GlobalSymbol` table** (`light_aarch64.h`)

```cpp
struct GlobalSymbol {
  const char *name;     // LLVM GlobalVariable::getName() spelling
  const void *address;  // Host address (live process memory)
};
```

Shape deliberately mirrors `easy::GlobalMapping { const char*; void*; }`
from `easy-jit-llvm15/include/easy/runtime/BitcodeTracker.h`, so an EasyJIT
runtime wrapper can pass its existing mapping array through unchanged.

**Extended APIs** — both `emit()` and `compile()` now accept an optional
`(const GlobalSymbol *globals, size_t nglobals)` pair (default `nullptr, 0`).

**Emitter-level additions** (`light_aarch64.cpp`):

| Feature | Encoders | Where |
|---|---|---|
| `MOVZ (64-bit, hw)` | `encMovzHw64(rd,imm16,hw)` = `0xD2800000 \| (hw<<21) \| (imm16<<5) \| rd` | new |
| `MOVK (64-bit, hw)` | `encMovkHw64(rd,imm16,hw)` = `0xF2800000 \| (hw<<21) \| (imm16<<5) \| rd` | new |
| `resolveGlobal` lambda | linear-scan `globals[]` matching `GV->getName()` | top of `emit()` |
| External-GV **load** fast-path | `MOVZ + {MOVK}*0..3` into `x17`, then `LDR rd, [x17, #uimm12_scaled]` | inside `LoadInst` block |
| External-GV **store** fast-path | same materialization, `STR rs, [x17, #uimm12_scaled]` | inside `StoreInst` block |
| Constant-GEP offset folding | `GEPOperator::accumulateConstantOffset` | both paths |

**Address materialization** — MOVZ at `hw=0`, then one MOVK per nonzero
16-bit halfword at `hw=1/2/3`, producing 1–4 instructions. Register `x17`
(IP1) is the designated scratch and does not collide with the existing
memcpy use (disjoint IR patterns).

---

## 2. Tests passing on unmodified dumped IR

Both driver invocations use the IR produced by the EasyJIT `EASYJIT_DUMP_IR`
hook — no manual IR surgery, no embedded initializers.

### 2.1 `global_snapshot`

Driver: `light_global_snapshot.cpp`. Declares `GlobalConfig g_cfg = {1, 7}`
as a real host global, passes `{"g_cfg", &g_cfg}` to `light::compile`.

```
[light] emitted 60 bytes of aarch64 machine code
[light] host addr of g_cfg = 0x760040
phase1 (enabled=1,bias=7)    f(5) = 12 (expect 12)
phase2 (enabled=0,bias=1000) f(5) = -94 (expect -94)
phase3 (enabled=1,bias=42)   f(5) = 47 (expect 47)
light_global_snapshot: PASS
```

The three-phase run is the important correctness check: the host global
is **mutated between invocations**, and each time the JIT'd code returns a
value consistent with the current live bytes — proving this is a real
address-indirection, not a compile-time snapshot constant.

### 2.2 `global_partial_snapshot`

Same driver, invoked with `g_partial_cfg`:

```
[light] emitted 60 bytes of aarch64 machine code
[light] host addr of g_partial_cfg = 0x760048
phase1 (enabled=1,bias=7)    f(5) = 12 (expect 12)
phase2 (enabled=0,bias=1000) f(5) = -94 (expect -94)
phase3 (enabled=1,bias=42)   f(5) = 47 (expect 47)
light_global_snapshot: PASS
```

The post-specialization IR for the two tests is **structurally identical**
— the EasyJIT pass defers external-global resolution to the JIT runtime in
both C-level shapes (`set_global_snapshot` and
`set_global_partial_struct`+`bind_global_field`). A single emitter
capability covers both.

### 2.3 Regression

| Driver | IR | Func | Result |
|---|---|---|---|
| `light_add_int` | `add_int.spec.ll` | `add` | PASS |
| `light_partial_struct` | `partial_struct_binding.spec.ll` | `eval_partial_config` | PASS (88-byte emission, unchanged) |
| `light_global_snapshot` | `global_snapshot.spec.ll` | `eval_global_snapshot_c` | PASS (60-byte emission) |
| `light_global_snapshot` | `global_partial_snapshot.spec.ll` | `eval_global_partial_c` | PASS (60-byte emission) |

---

## 3. Binary metrics

```
file light_global_snapshot
  ELF 64-bit LSB executable, ARM aarch64, statically linked, stripped

ldd  light_global_snapshot
  not a dynamic executable

size light_global_snapshot
     text     data     bss    total
  3400893   130660   30192  3561745

file size: 3 557 304 B
```

For reference, round-6's `light_partial_struct` = 3 404 669 B text;
`light_add_int` = 3 400 453 B text. The new driver adds ~440 B of text
over `light_add_int` (the GlobalSymbol plumbing and the new MOVZ/MOVK
fast-paths), still ~6× smaller than the PoC round-4 baseline.

---

## 4. aarch64_be analysis (address materialization)

**Claim:** the new address-materialization code is inherently
**endian-neutral**.

- The 64-bit host address is an *integer value*, not a memory read — no
  byte-ordering of the payload is involved in synthesizing it.
- `MOVZ/MOVK` are position-addressed by the `hw` field (halfword index
  0..3), not by byte offsets. On aarch64_be the same logical halfword
  lands in the same logical register bit-range.
- The AArch64 *instruction encoding in the instruction stream is always
  little-endian* regardless of the data endian the ABI selects, so
  `W.emit(uint32_t)` (LE word store) is correct on both endians.

**LE risk remains** (unchanged from round 6) exclusively on the
`LDR`/`STR` that follow — i.e. the i32/i64 field reads themselves assume
target-LE byte order. This was already documented as the dominant
aarch64_be gap, and the round-7 addition does not widen it.

Concretely, `emit()` still rejects `aarch64_be-*` triples up-front, so
none of this code runs in a BE ABI today; the note is about what would
be required if support were added.

---

## 5. Answers to the five questions

1. **New capabilities introduced this round.** (a) External-global symbol
   resolution via a caller-supplied `GlobalSymbol[]` table, (b) 1–4-instr
   MOVZ/MOVK address materialization into x17, (c) uimm12-scaled LDR/STR
   at constant-GEP offsets of externals, (d) load and store variants
   both covered.

2. **Which tests pass.** `add_int`, `partial_struct_binding`,
   `global_snapshot`, `global_partial_snapshot` — the last two on their
   unmodified dumped IR with real live host memory.

3. **Text size for the new driver.** 3 400 893 B (`light_global_snapshot`,
   static PIE-disabled, stripped).

4. **Emitted machine code for the global tests.** 60 bytes each
   (MOVZ/MOVK for base address ×2 loads + 2 LDRs + icmp/csel + ret).

5. **Next biggest capability gap.** `eval_global_partial_array_c` in
   `global_partial_snapshot.c` — has an **embedded pointer-to-array field**
   (`const int *values`) with a **runtime** index: requires reg-based
   pointer + variable offset LDR (`LDR rd, [xbase, xindex, LSL #2]`),
   which the current emitter does not support. Separately, `br`/`phi`
   was added in round 6 but is still straightline-heavy — any IR with a
   call to a non-intrinsic function is unsupported.

---

## 6. Files changed

- `trim-experiments/light_codegen/light_aarch64.h`: `GlobalSymbol` +
  extended `emit`/`compile` signatures.
- `trim-experiments/light_codegen/light_aarch64.cpp`: `encMovzHw64`,
  `encMovkHw64`, `resolveGlobal` lambda, external-GV load/store
  fast-paths, `compile()` plumbing.
- `trim-experiments/light_codegen/light_global_snapshot.cpp`: **new**
  driver with three-phase live-memory check.
- `trim-experiments/light_codegen/build_global_snapshot.sh`: **new**
  build script.
- `trim-experiments/ir_dumps/global_snapshot.spec.ll`: captured IR.
- `trim-experiments/ir_dumps/global_partial_snapshot.spec.ll`: captured IR.
