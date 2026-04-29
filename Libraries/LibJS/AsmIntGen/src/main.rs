/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! # AsmIntGen -- Assembly Interpreter Code Generator
//!
//! Compiles an architecture-neutral DSL (`asmint.asm`) into platform-specific
//! assembly for the LibJS bytecode interpreter's hot loop.
//!
//! ## Architecture overview
//!
//! The generated assembly implements a **threaded dispatch loop**:
//!
//! 1. A 256-entry **dispatch table** maps each u8 opcode to its handler address.
//! 2. Each handler executes one bytecode instruction, then dispatches to the next
//!    by reading the opcode byte at `pb[pc]` and jumping through the table.
//! 3. Opcodes without an assembly handler are caught by a **fallback handler**
//!    that calls into C++ (`asm_fallback_handler`).
//!
//! The entry point is `asm_interpreter_entry(bytecode, entry_point, values, vm)`.
//! It saves callee-saved registers, sets up pinned registers, and dispatches.
//!
//! ## Pinned registers
//!
//! These DSL names are mapped to callee-saved platform registers so they survive
//! C++ calls. See `registers.rs` for the actual mappings.
//!
//! | DSL name   | Purpose                                           |
//! |------------|---------------------------------------------------|
//! | `pc`       | Program counter (u32 byte offset into bytecode)   |
//! | `pb`       | Pointer to bytecode base (u8 const*)              |
//! | `values`   | Pointer to Value[] array                          |
//! | `exec_ctx` | Running ExecutionContext*                          |
//! | `dispatch` | Dispatch table base pointer                       |
//!
//! ## Temporary registers
//!
//! Temporaries are introduced by name with `temp` (GPR) and `ftemp` (FPR)
//! at the top of a handler or macro body; the register allocator assigns
//! each to a physical register from the platform's caller-saved pool. The
//! DSL also exposes `sp` and `fp`. None of the temporaries survive C++
//! calls (`call_slow_path`, `call_helper`, `call_interp`, `call_raw_native`).
//!
//! Some physical registers are reserved as codegen scratch and are not
//! addressable as DSL names. On aarch64 these are `x9`, `x10`, and `d16`.
//! On x86_64 the codegen claims `rax`, `rcx`, `r11`, and `xmm3` as scratch
//! at specific instructions -- the per-instruction metadata in
//! `instructions.rs` records exactly which physical registers are killed
//! by each DSL operation, and the allocator avoids placing a live named
//! temp in any of those.
//!
//! ## DSL instruction reference
//!
//! ### Control flow
//!
//! - `dispatch_next` -- Advance pc by the handler's instruction size and
//!   dispatch to the next handler. The size is determined from `Bytecode.def`
//!   or the handler's explicit `size=N` attribute.
//! - `dispatch_variable reg` -- Advance pc by the value in `reg` (32-bit)
//!   and dispatch.
//! - `goto_handler reg` -- Set pc to `reg` and dispatch (unconditional jump
//!   to a bytecode address).
//! - `jmp label` -- Unconditional branch to a local label within the handler.
//! - `exit` -- Jump to the exit path, returning control to C++.
//! - `assert_eq a, b` -- In assertion-enabled builds, trap if `a != b`.
//! - `assert_ne a, b` -- In assertion-enabled builds, trap if `a == b`.
//! - `assert_lt_unsigned a, b` -- In assertion-enabled builds, trap if
//!   `a >= b`.
//! - `assert_ge_unsigned a, b` -- In assertion-enabled builds, trap if
//!   `a < b`.
//! - `assert_zero a` -- In assertion-enabled builds, trap if `a != 0`.
//! - `assert_nonzero a` -- In assertion-enabled builds, trap if `a == 0`.
//! - `assert_bits_set a, mask` -- In assertion-enabled builds, trap if
//!   `(a & mask) == 0`.
//! - `assert_bits_clear a, mask` -- In assertion-enabled builds, trap if
//!   `(a & mask) != 0`.
//! - `assert_tag value, tag` -- In assertion-enabled builds, trap if
//!   `value`'s upper 16-bit NaN-boxing tag is not `tag`.
//! - `assert_not_tag value, tag` -- In assertion-enabled builds, trap if
//!   `value`'s upper 16-bit NaN-boxing tag is `tag`.
//!   Release/distribution builds pass assertions through the parser and
//!   allocator, but emit no code for them.
//!
//! ### C++ interop
//!
//! - `call_slow_path func` -- **TERMINAL.** Calls `i64 func(VM*, u32 pc)`.
//!   If return >= 0, reloads pinned state (exec_ctx, pb, values -- they may
//!   have changed due to exception unwinding), sets pc to the return value,
//!   and dispatches. If return < 0, exits. Control does NOT return to the
//!   handler after this instruction.
//! - `call_helper func` -- **Non-terminal.** Calls `u64 func(u64 value)`.
//!   Passes `t1` as the argument. Result lands in `t0`. The handler continues
//!   after the call. Does NOT reload pinned state.
//! - `call_interp func` -- **Non-terminal.** Calls `i64 func(VM*, u32 pc)`.
//!   Result lands in `t0`. The handler continues. Does NOT reload pinned state.
//! - `call_raw_native reg` -- **Non-terminal.** Indirectly calls a
//!   `ThrowCompletionOr<Value> (*)(VM&)` function pointer stored in `reg`.
//!   Passes the hidden VM* as the sole argument. On return, `t0` holds the
//!   payload and `t1` holds the variant index on both supported architectures.
//! - `reload_exec_ctx` -- Reload the exec_ctx register from the VM*.
//!   Used after non-terminal calls that may modify the running execution context.
//! - `load_vm dst` -- Load the hidden VM* into `dst`.
//!   Used when a handler needs to update VM-owned bookkeeping directly.
//!
//! ### Bytecode operand access
//!
//! - `load_operand dst, m_field` -- Read the u32 operand at `m_field` from
//!   the current bytecode instruction, use it as an index into the values
//!   array, and load the 64-bit Value into `dst`. Clobbers a scratch register.
//! - `store_operand m_field, src` -- Read the u32 operand at `m_field`, use
//!   it as an index into the values array, and store `src` there.
//! - `load_label dst, m_field` -- Load a raw u32 (Label/address) from bytecode
//!   field `m_field` into `dst` (zero-extended).
//!
//! ### Memory access
//!
//! Memory operands use bracket syntax: `[base]`, `[base, offset]`,
//! `[base, index, scale]`. Offsets can be immediate values, constants,
//! field references (`m_*`), or registers.
//!
//! - `load64 dst, [base, offset]` -- Load 64-bit value.
//! - `load32 dst, [base, offset]` -- Load 32-bit value, zero-extended to 64.
//! - `load_pair64 dst1, dst2, mem1, mem2` -- Load two adjacent 64-bit values.
//!   The DSL must name both memory operands explicitly; AsmIntGen verifies that
//!   `mem2` is immediately after `mem1`, then lowers to a paired load where the
//!   target supports it.
//! - `load_pair32 dst1, dst2, mem1, mem2` -- Load two adjacent 32-bit values.
//!   Like `load_pair64`, both operands must be named and adjacent or codegen
//!   rejects the handler.
//! - `load16 dst, [base, offset]` -- Load 16-bit value, zero-extended to 64.
//! - `load8 dst, [base, offset]` -- Load 8-bit value, zero-extended to 64.
//! - `load16s dst, [base, offset]` -- Load 16-bit value, sign-extended.
//! - `load8s dst, [base, offset]` -- Load 8-bit value, sign-extended.
//! - `store64 [base, offset], src` -- Store 64 bits.
//! - `store32 [base, offset], src` -- Store low 32 bits.
//! - `store_pair64 mem1, mem2, src1, src2` -- Store two adjacent 64-bit
//!   values. Like `load_pair64`, the DSL must name both destinations
//!   explicitly and AsmIntGen verifies that `mem2` is immediately after
//!   `mem1`.
//! - `store_pair32 mem1, mem2, src1, src2` -- Store two adjacent 32-bit
//!   values. Like `store_pair64`, both memory operands must be named and
//!   adjacent or codegen rejects the handler.
//! - `store16 [base, offset], src` -- Store low 16 bits.
//! - `store8 [base, offset], src` -- Store low 8 bits.
//! - `inc32_mem [base, offset]` -- Increment a 32-bit memory slot by 1.
//!
//! ### Integer ALU
//!
//! Two-operand form: `op dst, src` means `dst = dst OP src`.
//!
//! - `mov dst, src` -- Move register or immediate to register.
//! - `add dst, src` -- Add.
//! - `sub dst, src` -- Subtract.
//! - `mul dst, src, imm` -- Signed multiply (3-operand: `dst = src * imm`).
//!   Also 2-operand: `dst *= src`.
//! - `and dst, src` -- Bitwise AND.
//! - `or dst, src` -- Bitwise OR.
//! - `xor dst, src` -- Bitwise XOR. `xor dst, dst` zeros the register.
//! - `neg dst` -- Negate (`dst = -dst`).
//! - `not dst` -- Bitwise NOT (`dst = ~dst`).
//! - `shl dst, count` -- Shift left.
//! - `shr dst, count` -- Logical shift right.
//! - `sar dst, count` -- Arithmetic shift right.
//! - `movsxd dst, src` -- Sign-extend 32-bit value in `src` to 64-bit `dst`.
//! - `lea dst, [base, offset]` -- Load effective address (compute address
//!   without memory access).
//! - `divmod quot, rem, dividend, divisor` -- Signed integer divide.
//!   `quot = dividend / divisor`, `rem = dividend % divisor`.
//!
//! ### 32-bit arithmetic with overflow detection
//!
//! These perform the operation on the low 32 bits and branch to `fail`
//! if the result overflows signed int32. On x86_64, this uses the
//! hardware overflow flag (`jo`). On aarch64, `adds`/`subs` w-register
//! forms with `b.vs`, or `smull` + sign-extend check for multiply.
//! After the operation, the upper 32 bits of `dst` are zeroed.
//!
//! - `add32_overflow dst, src, fail` -- `dst += src` (32-bit).
//! - `sub32_overflow dst, src, fail` -- `dst -= src` (32-bit).
//! - `mul32_overflow dst, src, fail` -- `dst *= src` (32-bit).
//! - `neg32_overflow dst, fail` -- `dst = -dst` (32-bit).
//! - `not32 dst` -- `dst = ~dst` (32-bit, upper bits zeroed).
//!
//! ### NaN-boxing operations
//!
//! These are codegen instructions (not macros), so each backend can emit
//! optimal platform-specific code.
//!
//! - `extract_tag dst, src` -- Extract upper 16-bit NaN-boxing tag from
//!   a 64-bit value: `dst = src >> 48`. On aarch64, emits a single
//!   3-operand `lsr`. On x86_64, `mov` + `shr`.
//! - `unbox_int32 dst, src` -- Sign-extend the low 32 bits of a NaN-boxed
//!   int32 value to 64 bits.
//! - `unbox_object dst, src` -- Zero-extend lower 48 bits to extract a
//!   pointer from a NaN-boxed object/string/symbol/bigint value. On
//!   aarch64, emits a single `and` with a 48-bit logical immediate.
//! - `box_int32 dst, src` -- NaN-box a raw 32-bit integer. Masks the low
//!   32 bits and sets the INT32_TAG in the upper 16 bits. On aarch64,
//!   `mov wD, wS` + `movk xD, #tag, lsl #48` (2 instructions).
//! - `box_int32_clean dst, src` -- Like `box_int32` but the source is
//!   known to have its upper 32 bits already zeroed. Skips the masking.
//!
//! ### Bit manipulation
//!
//! - `toggle_bit dst, N` -- Flip bit N: `dst ^= (1 << N)`.
//! - `clear_bit dst, N` -- Clear bit N: `dst &= ~(1 << N)`.
//!
//! ### Floating-point
//!
//! All FP operations use scalar doubles (`ft0`-`ft3`).
//!
//! - `loadf32 dst, [base, offset]` -- Load a 32-bit float into an FP
//!   register.
//! - `storef32 [base, offset], src` -- Store a 32-bit float from an FP
//!   register.
//! - `fp_add dst, src` -- `dst += src`
//! - `fp_sub dst, src` -- `dst -= src`
//! - `fp_mul dst, src` -- `dst *= src`
//! - `fp_div dst, src` -- `dst /= src`
//! - `fp_sqrt dst, src` -- `dst = sqrt(src)`
//! - `fp_floor dst, src` -- Round toward negative infinity.
//! - `fp_ceil dst, src` -- Round toward positive infinity.
//! - `fp_mov dst, src` -- Bitwise move between integer and FP registers.
//! - `float_to_double dst, src` -- Convert float32 to double.
//! - `double_to_float dst, src` -- Convert double to float32.
//! - `int_to_double fpr, gpr` -- Convert signed int64 to double.
//! - `double_to_int32 dst, fpr, fail_label` -- Truncate double to int32
//!   with strict round-trip check. Branches to `fail_label` if the value
//!   is fractional, out of i32 range, or NaN. No modular reduction.
//! - `js_to_int32 dst, fpr, fail_label` -- Convert double to int32 using
//!   JS ToInt32 semantics. With FEAT_JSCVT (ARMv8.3), uses `fjcvtzs` and
//!   never branches. On success, the low 32 bits hold the result and the
//!   upper 32 bits are cleared so callers can use `box_int32_clean`.
//!   Without FEAT_JSCVT, truncates and branches to `fail_label` if the
//!   value doesn't round-trip (fractional, out of range, NaN).
//! - `canonicalize_nan dst_gpr, src_fpr` -- Bitwise-copy `src_fpr` to
//!   `dst_gpr`, but if the value is NaN, write `CANON_NAN_BITS` instead.
//!   JS requires all NaN values to be canonicalized in Value storage.
//!
//! ### Branching
//!
//! All branch instructions encode comparison + branch as a single operation,
//! so backends are free from flag-register semantics.
//!
//! - `branch_eq a, b, label` -- Branch if `a == b`.
//! - `branch_ne a, b, label` -- Branch if `a != b`.
//! - `branch_ge_unsigned a, b, label` -- Branch if `a >= b` (unsigned).
//! - `branch_zero a, label` -- Branch if `a == 0`.
//! - `branch_nonzero a, label` -- Branch if `a != 0`.
//! - `branch_negative a, label` -- Branch if `a < 0` (sign bit set).
//! - `branch_not_negative a, label` -- Branch if `a >= 0`.
//! - `branch_bits_set a, mask, label` -- Branch if `(a & mask) != 0`.
//! - `branch_bits_clear a, mask, label` -- Branch if `(a & mask) == 0`.
//! - `branch_bit_set a, N, label` -- Branch if bit N is set.
//! - `branch_any_eq reg, v1, v2, ..., label` -- Branch if `reg` equals
//!   any of the values. On x86, emits a chain of cmp+je. On aarch64,
//!   uses a ccmp chain with a single final branch.
//! - `branch_fp_unordered a, b, label` -- Branch if FP comparison is
//!   unordered (either operand is NaN).
//! - `branch_fp_equal a, b, label` -- Branch if `a == b` (FP).
//!
//! ## DSL syntax
//!
//! ```text
//! # Comments start with #
//! const NAME = VALUE           # Named constant (integer)
//!
//! macro name(params)           # Macro definition
//!     instructions...
//! end
//!
//! handler OpName, size=N       # Bytecode handler (size optional if in Bytecode.def)
//!     temp name1, name2, ...   # Declare GPR temporaries (allocator-assigned)
//!     ftemp name1, ...         # Declare FPR temporaries
//!     instructions...
//! end
//! ```
//!
//! Named temporaries declared with `temp` / `ftemp` are scoped to the
//! enclosing handler or macro body. The register allocator assigns each
//! to a physical register from the public temp pool; positional aliases
//! (`t0`-`t8`, `ft0`-`ft3`) remain available for code that wants to pin
//! a value to a specific platform register.
//!
//! Labels within handlers: `.name:` (local, prefixed with handler name in output).
//!
//! Field references: `m_dst`, `m_src`, etc. resolve to byte offsets within the
//! current handler's bytecode instruction, computed from `Bytecode.def`.
//!
//! Constants in ALL_CAPS are looked up in the constants table (from
//! `gen_asm_offsets` or `const` declarations).
//!
//! ## Porting to a new architecture
//!
//! 1. Add a variant to `Arch` in `registers.rs`.
//! 2. Add a `RegisterMapping` const with pinned and temporary register
//!    assignments. Pinned registers MUST be callee-saved.
//! 3. Create `codegen_<arch>.rs` implementing `pub fn generate(&Program) -> String`.
//!    The codegen must emit:
//!    - A 256-entry dispatch table of handler addresses.
//!    - An `asm_interpreter_entry` function that saves callee-saved registers,
//!      sets up pinned registers from the C ABI arguments, and dispatches.
//!    - A fallback handler that calls `asm_fallback_handler`, handles exit
//!      (return < 0), reloads state, and dispatches on new pc.
//!    - An exit path that restores callee-saved registers and returns.
//!    - A handler for each DSL instruction in `emit_instruction()`.
//! 4. Wire it up in `main.rs` (one match arm).
//! 5. Add the architecture detection and build rules in `CMakeLists.txt`.
//!
//! Each DSL instruction must be implemented. The `_ =>` fallback arm emits an
//! "UNKNOWN" comment for any unhandled mnemonic, which makes it easy to find
//! missing instructions during development.

mod allocator;
mod codegen_aarch64;
mod codegen_x86_64;
mod instructions;
mod parser;
mod registers;
mod shared;

use parser::ObjectFormat;
use registers::Arch;
use std::fs;

fn main() {
    let mut args = std::env::args().skip(1);

    let mut arch = Arch::X86_64;
    let mut object_format = ObjectFormat::MachO;
    let mut input_path = None;
    let mut output_path = None;
    let mut constants_path = None;
    let mut bytecode_def_path = None;
    let mut has_jscvt = false;
    let mut enable_assertions = false;

    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--has-jscvt" => has_jscvt = true,
            "--enable-assertions" => enable_assertions = true,
            "--arch" => {
                let val = args.next().expect("--arch requires a value");
                arch = match val.as_str() {
                    "x86_64" => Arch::X86_64,
                    "aarch64" => Arch::Aarch64,
                    other => {
                        eprintln!("Unknown architecture: {other}");
                        std::process::exit(1);
                    }
                };
            }
            "--object-format" => {
                let val = args.next().expect("--object-format requires a value");
                object_format = match val.as_str() {
                    "macho" => ObjectFormat::MachO,
                    "elf" => ObjectFormat::Elf,
                    other => {
                        eprintln!("Unknown object format: {other}");
                        std::process::exit(1);
                    }
                };
            }
            "--input" => input_path = args.next(),
            "--output" => output_path = args.next(),
            "--constants" => constants_path = args.next(),
            "--bytecode-def" => bytecode_def_path = args.next(),
            _ => {
                eprintln!("Unknown argument: {arg}");
                std::process::exit(1);
            }
        }
    }

    let input_path = input_path.unwrap_or_else(|| {
        eprintln!("Usage: asmintgen --arch x86_64 --input <file.asm> --output <file.S> [--constants <file>] [--bytecode-def <file>]");
        std::process::exit(1);
    });
    let output_path = output_path.unwrap_or_else(|| {
        eprintln!("Usage: asmintgen --arch x86_64 --input <file.asm> --output <file.S> [--constants <file>] [--bytecode-def <file>]");
        std::process::exit(1);
    });

    let source = fs::read_to_string(&input_path).unwrap_or_else(|e| {
        eprintln!("Failed to read {input_path}: {e}");
        std::process::exit(1);
    });

    let mut program = parser::parse(&source);

    if let Some(path) = constants_path {
        let constants_source = fs::read_to_string(&path).unwrap_or_else(|e| {
            eprintln!("Failed to read {path}: {e}");
            std::process::exit(1);
        });
        let constants_program = parser::parse(&constants_source);
        for (name, value) in constants_program.constants {
            program.constants.insert(name, value);
        }
    }

    if let Some(path) = bytecode_def_path {
        let def_source = fs::read_to_string(&path).unwrap_or_else(|e| {
            eprintln!("Failed to read {path}: {e}");
            std::process::exit(1);
        });
        let ops = bytecode_def::parse_bytecode_def(&def_source);
        program.op_layouts = bytecode_def::compute_layouts(&ops);
        program.opcode_list = ops.iter().map(|op| op.name.clone()).collect();
    }

    program.object_format = object_format;
    program.has_jscvt = has_jscvt;
    program.enable_assertions = enable_assertions;

    let output = match arch {
        Arch::X86_64 => codegen_x86_64::generate(&program),
        Arch::Aarch64 => codegen_aarch64::generate(&program),
    };

    fs::write(&output_path, output).unwrap_or_else(|e| {
        eprintln!("Failed to write {output_path}: {e}");
        std::process::exit(1);
    });
}
