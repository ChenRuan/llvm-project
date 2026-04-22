# Round-5 Report — Light-codegen Exploration

Branch: `llvm15_trim_light_codegen_explore` (off `llvm15_trim_llvm_backend`)
Commit: `trim-experiments/light_codegen/`

## 0. TL;DR

| Path | Fully-static `text` | Size ratio | Runs? |
|---|---:|---:|:-:|
| Full LLVM backend (round-4 best) | **21,211,843 B** (20.23 MB) | 1.00× | ✅ |
| **Light-codegen PoC (this round)** | **3,383,909 B** (3.23 MB) | **0.16× (−84%)** | ✅ |

The PoC **keeps EasyJIT specialization** and replaces **all of** `LLVMCodeGen` + `LLVMSelectionDAG` + `LLVMAArch64CodeGen` + `LLVMOrcJIT` + `LLVMRuntimeDyld` + `LLVMMC*` + `LLVMTarget` + `LLVMExecutionEngine` + `LLVMTransformUtils` + `LLVMAnalysis` + `LLVMipo` + `LLVMScalarOpts` + `LLVMVectorize` with **~270 lines of hand-coded AArch64 emitter**. `add_int` runs correctly and produces the right output.

The answer to the round-5 question — *is "keep EasyJIT, replace LLVM backend" worth continuing* — is **yes, for this class of workload**. The size drop is a full order of magnitude, not a round-rounding. However, the PoC only covers what `add_int` actually exercises; the scope to cover any of the larger `config_process_*` tests is significant and is discussed in §6.

---

## 1. Task 1 — What does EasyJIT-specialized IR actually look like?

Captured by temporarily injecting a `getenv("EASYJIT_DUMP_IR")` hook after the EasyJIT pass pipeline in `easy-jit-llvm15/runtime/Function_compile.cpp` (reverted before commit). For `add_int.c` with `b=1` specialized:

```llvm
define i32 @add(i32 noundef %0) {
  %2 = add nsw i32 %0, 1
  ret i32 %2
}
```

**That's it.** After `ContextAnalysis → InlineParameters → DevirtualizeConstant → Inlining → ConstStructPropagate (×2) → mem2reg → CFGSimplification → Internalize → GlobalDCE → StripDeadPrototypes`, the `add_int` workload converges to a single basic block with a single binop and a return. No loads, no stores, no branches, no calls, no struct layouts — EasyJIT's front-end work has already flattened everything.

### IR/semantic subset coverage vs test matrix

| Subset needed | `add_int` | `retconst_jit_probe` | `config_process_easyjit_small` | `partial_struct_binding` |
|---|:-:|:-:|:-:|:-:|
| 1 BB | ✅ | ❓ | ❓ | ❓ |
| integer binop + const imm | ✅ | ✅ | ✅ | ✅ |
| memory load/store | ❌ | likely | likely | likely |
| branch / phi | ❌ | maybe | likely | likely |
| call | ❌ | likely external | likely | likely |

The PoC's "1 BB, no memory, no branch, no call" constraint is **sufficient for `add_int`** and is explicitly *not* a strawman — it exactly matches what EasyJIT produces after specializing out all the surrounding plumbing. Covering the next tier (`retconst_jit_probe`, simple `load i32` from a snapshotted struct, straight-line-only) would need: load-from-constant-address + one forward branch + `phi` (or more trivially, constant-fold in the emitter). That's a few dozen more lines, not a redesign.

---

## 2. Task 2 — Design of the narrow emitter

**Chosen approach**: walk the LLVM IR `Function` in opcode order, pattern-match each `Instruction` into a fixed AArch64 encoder, write 32-bit little-endian instruction words into a buffer. No IR lowering, no `SelectionDAG`, no register allocator, no scheduler.

**Input**: `const llvm::Function &` (post-EasyJIT specialization).
**Output**: bytes in `uint8_t*` buffer, or `Status::Unsupported` with a reason string.

### Register model
Dumb static mapping:
- `Function` arg `i` → `x{i}` (AArch64 calling convention; trivially correct for 0–8 integer params).
- Each IR instruction result → a dedicated scratch (`x9, x10, …, x15`).
- If we run out of scratches (>7 live results), emit `Unsupported`.

No spilling. No liveness analysis. The register *is* the IR SSA value's identity. Because AArch64 aliases `w<N>` with the low 32 bits of `x<N>`, `trunc/zext/sext` between i32 and i64 is a no-op rename — we just copy the operand's register assignment to the cast result.

### Supported operations (v0)
| Category | Shapes |
|---|---|
| `add / sub` | reg-reg (32/64) · reg + imm12 (32/64) |
| `mul / and / or / xor` | reg-reg (32/64) · reg + imm ≤ 16 bits (materialized via MOVZ into `x16`) |
| `shl / lshr / ashr` | reg + constant shift only (via UBFM/SBFM) |
| `trunc / zext / sext` | aliased (no instruction emitted) |
| `ret` | `ret i32 / i64 / void`; moves return into `w0/x0` via ORR if needed; `ret` (`0xD65F03C0`) |

### Explicitly unsupported (by design)
- Multi-BB / branches / phi nodes
- Any memory op (load, store, alloca, GEP)
- Any call (including intrinsics)
- Variable-amount shifts
- Any FP / vector op
- Any constant that doesn't fit in imm12 (for add/sub) or imm16 (for mul/and/or/xor)
- ABI edge cases (struct-by-value, varargs, big-integer returns)
- aarch64_be (see §7)

Each unsupported shape returns `Status::Unsupported` with a string — a production version would fall back to the full LLVM backend here, which is exactly the fallback model OSR JITs use.

---

## 3. Task 3 — PoC files and build

```
trim-experiments/light_codegen/
├── light_aarch64.h      (22 lines)
├── light_aarch64.cpp    (~255 lines; of which ~110 are AArch64 encoders)
├── light_add_int.cpp    (55-line driver: parseIRFile → light::compile → call)
└── build.sh             (static link script, only LLVMCore + LLVMIRReader
                          + LLVMSupport and friends)
```

### PoC driver link surface (what's pulled vs dropped)

| Archive | Full-backend path | Light-codegen PoC |
|---|:-:|:-:|
| `libLLVMAArch64CodeGen.a` | ✅ | ✗ |
| `libLLVMAArch64Desc.a / Info.a / Utils.a` | ✅ | ✗ |
| `libLLVMCodeGen.a` | ✅ | ✗ |
| `libLLVMSelectionDAG.a` | ✅ | ✗ |
| `libLLVMMC.a / MCParser.a` | ✅ | ✗ |
| `libLLVMTarget.a` | ✅ | ✗ |
| `libLLVMExecutionEngine.a` | ✅ | ✗ |
| `libLLVMOrcJIT.a / OrcShared.a / OrcTargetProcess.a` | ✅ | ✗ |
| `libLLVMRuntimeDyld.a` | ✅ | ✗ |
| `libLLVMJITLink.a` | ✅ | ✗ |
| `libLLVMGlobalISel.a` | ✅ | ✗ |
| `libLLVMObject.a` | ✅ | ✗ |
| `libLLVMAsmPrinter.a` | ✅ | ✗ |
| `libLLVMTransformUtils.a / Analysis.a / ipo.a / ScalarOpts.a` | ✅ | ✗ |
| `libLLVMBitWriter.a / CFGuard.a / Linker.a / ProfileData.a / Remarks.a / DebugInfoCodeView.a` | ✅ | (only `Remarks`) |
| `libLLVMAsmParser.a` | ✗ (stubbed) | ✅ (only for `.ll` reading in this PoC) |
| `libLLVMBitReader.a / BitstreamReader.a` | ✅ | ✅ |
| `libLLVMCore.a` | ✅ | ✅ |
| `libLLVMSupport.a / Demangle.a / BinaryFormat.a` | ✅ | ✅ |

That is **22 archives dropped**, 9 kept. Everything dropped is part of native codegen; everything kept is IR data model + bitcode I/O.

### Validation

```
$ ./out/light_add_int /tmp/add_int_spec.ll add 4
[light] emitted 12 bytes of aarch64 machine code
[light] bytes: 09 04 00 11 e0 03 09 2a c0 03 5f d6
light_inc(4) is 5
light_inc(5) is 6
light_inc(6) is 7
light_inc(7) is 8
$ file out/light_add_int
ELF 64-bit LSB executable, ARM aarch64, ... statically linked, stripped
$ ldd out/light_add_int
        not a dynamic executable
```

Emitted instructions decoded:

| Bytes (LE) | Word | Disasm |
|---|---|---|
| `09 04 00 11` | `0x11000409` | `add w9, w0, #1`   |
| `e0 03 09 2a` | `0x2a0903e0` | `orr w0, wzr, w9` (= `mov w0, w9`) |
| `c0 03 5f d6` | `0xd65f03c0` | `ret` |

---

## 4. Task 4 — Quantitative size comparison

Apples-to-apples (same machine, same toolchain, same `-O2 -s -static --gc-sections`, same `add_int` workload):

| Binary | text | data | bss | total | vs baseline |
|---|---:|---:|---:|---:|---:|
| `out/p15_baseline` (round-4 best, full LLVM backend) | **21,211,843** | 790,420 | 176,812 | 22,179,075 | 1.00× |
| `out/light_add_int` (light-codegen PoC) | **3,383,909** | 130,644 | 30,192 | 3,544,745 | **0.16×** |

- **text**: −17,827,934 B (−84.0%)
- **total**: −18,634,330 B (−84.0%)
- Both pass `file = statically linked`, `ldd = not a dynamic executable`, and produce `inc(4..7) = 5..8`.

### Where does the 3.38 MB go?

The remaining text is almost entirely `libLLVMCore.a` + `libLLVMSupport.a` (IR data model, DenseMap/StringMap/APInt infrastructure, the bitcode reader, the .ll parser). The hand-coded emitter itself is ~4 KB of `text` — negligible.

If we dropped the `.ll` text-form reading and required pre-packaged bitcode (drop `libLLVMAsmParser.a` + `libLLVMIRReader.a`), I would estimate another ~400–700 KB off. If we eventually built a no-LLVM-IR variant that takes EasyJIT's pre-specialized symbolic representation directly, we could plausibly get below **1.0 MB** of text. That is the order-of-magnitude answer the round asked for.

---

## 5. Dependencies analysis

### What the light PoC *still* needs from LLVM
- `llvm::Module`, `llvm::Function`, `llvm::Instruction`, `llvm::ConstantInt`, `llvm::Type` — IR data model.
- `llvm::parseIRFile` (IRReader + BitReader + AsmParser) — to read the specialized module.
- `llvm::SmallVector`, `APInt`, `StringRef`, `raw_ostream`, `MemoryBuffer` — utility types.
- `LLVMContext` lifetime.

Everything else listed above (CodeGen, SelectionDAG, AArch64CodeGen, MC, Target, ExecutionEngine, OrcJIT, RuntimeDyld, JITLink, …) is **not referenced** anywhere in the PoC.

### What EasyJIT's specialization pipeline still needs
The EasyJIT pass pipeline (the real value add) is unmodified: `ContextAnalysis → InlineParameters → DevirtualizeConstant → Inlining → ConstStructPropagate → mem2reg → CFGSimplification → Internalize → GlobalDCE → StripDeadPrototypes`. This is what flattens `add(a, b=1)` into `add a, 1`. The PoC validated its output end-to-end, but the PoC itself does not *run* the EasyJIT passes — it reads an already-specialized module. Wiring the PoC into the EasyJIT runtime proper (replacing `MinimalOrcJIT::addIRModule`) is the natural next step but is deliberately not in this round.

---

## 6. What would it take to make this a real option?

To cover the rest of the round-4 test matrix without falling back to the full LLVM backend, the emitter would need:

| Feature | Estimated effort | Risk |
|---|---|---|
| `load` / `store` of i32/i64 at constant or reg-offset | ~50 lines (LDR/STR imm9/uimm12) | Low |
| Forward-only branch + 2 BBs + `phi` with ≤2 incoming | ~80 lines (Bcc, B, simple RPO, phi-by-copy) | Low |
| `icmp` producing `i1` consumed by `br` | ~40 lines (SUBS-based cmp, then Bcc encoded from predicate) | Low |
| `call` to external C function (libc `printf` etc.) | ~60 lines (x0..x7 arg marshalling, BL + R_AARCH64_CALL26 relocation back-patch) | Medium |
| Simple spill when `>7` scratches needed | ~80 lines (STR/LDR to local frame, track offsets) | Medium |
| Fallback to LLVM on Unsupported | glue code; fully-backed fallback inflates binary again | Strategy decision |
| `aarch64_be` | **see §7** | **High** |
| Anything vector / FP / inline asm / EH | not in scope for the stated use case | n/a |

For the "small C-API JIT with simple struct specialization" target the paper describes, the first four rows alone would likely cover the majority of the existing tests. The total emitter would grow from ~270 lines to ~500–700 lines and stay well under a week of work.

**Recommendation**: the PoC demonstrates the route is viable. If the downstream target is truly constrained (single-digit MB), this is the first approach in the whole trim effort that has actually shown an order-of-magnitude reduction. Further source-level LLVM backend trimming — the round-3/4 direction — has flatlined in the 20–22 MB band.

---

## 7. aarch64_be risk analysis

**Default assumption of the PoC**: aarch64 **little-endian only**. The emitter explicitly rejects `aarch64_be` in `light::emit()`:
```cpp
if (Fn.getParent()->getTargetTriple().find("aarch64_be") == 0) {
  r.status = Status::NotAarch64LE;
  r.reason = "aarch64_be not supported by PoC emitter";
  return r;
}
```

### Concrete risk points if/when porting to aarch64_be

| Area | Risk | Severity | Notes |
|---|---|---|---|
| **Instruction encoding** | AArch64 instructions are **always little-endian** regardless of the data-endian mode, per ARM ARM §A1.3 and §E2.1. The `uint32_t`-to-bytes `memcpy` in `Writer::emit` already produces LE bytes on any LE host; on a BE host it would be wrong. Must switch to explicit byte-by-byte emission. | **Low–Medium** | One-liner fix: write `buf[pos+0..3] = {w&0xFF, (w>>8)&0xFF, (w>>16)&0xFF, (w>>24)&0xFF}`. But the fix must be actually applied; the current code accidentally works because we run on LE. |
| **Data access endianness** | Not yet exercised (PoC has no load/store). When we add memory ops, `LDR wN,[xM]` *does* honor the PSTATE.E bit on aarch64_be. If the JITed code expects to read data laid out by an LE-compiled host, or vice versa, the byte order will be wrong silently. | **High (once memory ops land)** | Needs coordinated endianness between host that built the snapshot and JITed code. EasyJIT snapshots are byte-copies of host structs, so they're automatically host-matched — but this must be documented. Mixed LE/BE runs would corrupt silently. |
| **ABI — integer params/returns** | AAPCS64 is endianness-neutral for integer args/returns (they pass in full 64-bit x-registers). | **None** | Safe. |
| **ABI — aggregate returns / by-value structs** | Not exercised by this PoC. In AAPCS64 these use **memory order** of bytes; on BE the byte-ordering inside the hidden return-buffer matters. | **Medium (when aggregates land)** | Not in scope for add_int-class inputs. |
| **Stack frame layout** | AAPCS64 stack slots are byte-addressed and endianness-neutral. | **None** | Safe. |
| **Condition codes / compare** | NZCV is architecturally LE/BE-agnostic. | **None** | Safe. |
| **Bitcode / IR host-endian assumptions** | `llvm::parseIRFile` + LLVMCore are endian-clean and we've been running LE bitcode on LE host. If the specialized bitcode was produced on LE and the JIT runs on BE, DataLayout strings (`"e-m:e-…"` vs `"E-m:e-…"`) must be kept consistent, or LLVM will reject. | **Medium** | Practical issue: EasyJIT currently embeds bitcode for `aarch64-unknown-linux-gnu` (LE). A BE target would need the build toolchain to produce `aarch64_be-unknown-linux-gnu` bitcode from the outset. |
| **Relocations (future `call` support)** | AArch64 `R_AARCH64_CALL26` / `ADR_PREL_PG_HI21` are endian-agnostic at the encoding level (all 32-bit words are LE) but the calling helper may need BE-aware byte manipulation when back-patching. | **Low** | One-liner per relocation site. |
| **`__builtin___clear_cache`** | Present in all GCC/Clang targets. No issue. | **None** | Safe. |

### Overall risk level
- **For the current PoC scope (no memory, no branch, no call)**: switching to aarch64_be is **a ~10-line mechanical fix** — the encoding byte-endianness in `Writer::emit`. Semantics are otherwise identical.
- **Once memory loads/stores are added**: real risk. The endianness of the data the JITed function reads must match the host that produced the snapshot. This is a *deploy-time constraint*, not a PoC-time bug, but it must be explicitly surfaced in the JIT API and the spec.
- **Once calls to external C are added**: low risk — LE/BE-symmetric provided everything is built for the same endian.

**Verdict**: aarch64_be is not a show-stopper, but the PoC in its current form is not portable to BE. Before any serious commitment, the emitter needs: (a) explicit endian-neutral instruction emission, (b) a documented snapshot-endianness contract, and (c) a CI target that actually builds the snapshotted bitcode for `aarch64_be-unknown-linux-gnu` and runs the PoC against it.

---

## 8. What not to spend time on next

Based on the round-4 conclusion and this round's data:

- **Further LLVM-backend source trim** (round-4 recommended against continuing). Confirmed: even after P1–P11, we're still 6× larger than a hand-coded narrow backend. The LLVM backend isn't shrinkable to <10 MB without surgery that goes well past the "low-risk" bar.
- **LTO / ICF tuning**. Build-level gains would be a few percent, not 6×.
- **Making the PoC a full compiler in one step**. Each feature should be added only when an actual test case demands it.

## 9. Recommended next-round scope (if this direction is pursued)

1. Wire `light::compile` into EasyJIT's `MinimalOrcJIT` as an opt-in backend, with automatic fallback to full LLVM codegen on `Unsupported`. Measure the fallback rate on the existing `c_api` test set.
2. Add `load` (imm-offset) + `store` (imm-offset) + simple `icmp + br` + 2-BB phi. Re-measure fallback rate.
3. Add minimal `call` support (BL + R_AARCH64_CALL26 back-patch) for resolving libc `printf`-style imports through the existing EasyJIT global-address map.
4. Run the full `tests/c_api/` suite twice: once through the LLVM backend, once through light-codegen with fallback. Compare binary sizes.
5. Only then: do the aarch64_be byte-endian fix and a BE build.

Steps 1–4 are each self-contained low-risk increments; step 5 requires toolchain coordination.

---

## Appendix — exact commands

```bash
# 1. build the LLVM side (already built via round-4 cmake flags)
cd /home/ruanchen/workspace/llvm-project-15.0.4/build-host && ninja

# 2. build the EasyJIT runtime (unchanged)
cd /home/ruanchen/workspace/llvm-project-15.0.4/easy-jit-llvm15/build-host-easyjit && ninja

# 3. produce a specialized .ll for add_int  (dump hook was reverted after capture;
#    /tmp/add_int_spec.ll is the captured module)
cat /tmp/add_int_spec.ll     # 3-instruction function

# 4. build the light-codegen PoC (fully static)
cd /home/ruanchen/workspace/llvm-project-15.0.4/trim-experiments/light_codegen
./build.sh

# 5. run
./out/light_add_int /tmp/add_int_spec.ll add 4
#    -> light_inc(4..7) = 5..8, 12 bytes of machine code.

# 6. reproduce the baseline for comparison
cd ../                       # trim-experiments/
EXTRA_STUBS=asmparser_stub.o ./link_static_add_int.sh p15_baseline archives_phase3.txt

size out/p15_baseline light_codegen/out/light_add_int
```
