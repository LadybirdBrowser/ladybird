/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use flapc::{Architecture, CompilationUnit, CompileOptions, Compiler, ObjectFormat, Profile, SourceInput, Target};

fn compiler(architecture: Architecture) -> Compiler {
    Compiler::new(CompileOptions {
        target: Target {
            architecture,
            object_format: ObjectFormat::Elf,
        },
        has_jscvt: false,
        has_lzcnt: true,
        has_bmi1: true,
        enable_assertions: true,
        profiling: false,
        profile: Profile::WebAssembly,
    })
}

fn compile_with_constants(source: &str, constants: &str) -> Result<String, String> {
    compiler(Architecture::X86_64)
        .compile(CompilationUnit {
            source: SourceInput {
                name: "wasm.flap",
                contents: source,
            },
            constants: Some(SourceInput {
                name: "layout.conf",
                contents: constants,
            }),
        })
        .map(|assembly| assembly.as_str().to_owned())
        .map_err(|error| error.to_string())
}

fn compile(source: &str) -> Result<String, String> {
    compile_with_constants(source, "")
}

fn handler<'a>(assembly: &'a str, name: &str) -> &'a str {
    let start = assembly
        .find(&format!("asm_handler_{name}:\n"))
        .unwrap_or_else(|| panic!("missing handler {name}"));
    let end = assembly[start..].find(&format!("asm_handler_{name}_hot_end:")).unwrap();
    &assembly[start..start + end]
}

const PROGRAM: &str = r#"
handler Nop() @opcode(0x01) {
    dispatch_next;
}

handler LocalGet() @opcode(0x20) {
    let code: Sequence<u8> = alias(ip);
    let index: u64 = alias(code[1]);
    let locals: Sequence<u64> = alias(fp);
    let stack: Sequence<u64> = alias(vsp);
    stack[0] = locals[index];
    dispatch_variable(2);
}

handler Saturating() @opcode(0xfc, 7) {
    dispatch_variable(2);
}

handler Fallback() @fallback {
    exit;
}
"#;

#[test]
fn dispatches_on_the_byte_the_instruction_pointer_points_at() {
    let assembly = compile(PROGRAM).unwrap();
    let nop = handler(&assembly, "Nop");
    assert!(nop.contains("add r13, 1\n"), "{nop}");
    assert!(
        nop.contains("movzx eax, BYTE PTR [r13]\n    jmp [r12 + rax * 8]"),
        "{nop}"
    );
    assert!(!assembly.contains("r14 + r13"));

    let local_get = handler(&assembly, "LocalGet");
    assert!(local_get.contains("BYTE PTR [r13 + 1]"), "{local_get}");
    assert!(local_get.contains("[rbp + rax * 8]"), "{local_get}");
    assert!(local_get.contains("QWORD PTR [rbx]"), "{local_get}");
}

#[test]
fn builds_dispatch_tables_from_opcode_annotations() {
    let assembly = compile(PROGRAM).unwrap();
    let table = assembly
        .split("asm_dispatch_table:\n")
        .nth(1)
        .unwrap()
        .lines()
        .take(256)
        .collect::<Vec<_>>();
    assert_eq!(table[0x00], "    .quad asm_handler_Fallback");
    assert_eq!(table[0x01], "    .quad asm_handler_Nop");
    assert_eq!(table[0x20], "    .quad asm_handler_LocalGet");
    assert_eq!(table[0xfc], "    .quad asm_handler_Fallback");

    let secondary = assembly
        .split("asm_dispatch_table_fc:\n")
        .nth(1)
        .unwrap()
        .lines()
        .take(512)
        .collect::<Vec<_>>();
    assert_eq!(secondary.len(), 512);
    assert_eq!(secondary[7], "    .quad asm_handler_Saturating");
    assert_eq!(secondary[8], "    .quad asm_handler_Fallback");

    assert!(assembly.contains(".globl CSYM(wasm_interpreter)\n"));
    assert!(assembly.contains(".globl CSYM(wasm_interpreter_dispatch_table)\n"));
    assert!(assembly.contains(".globl CSYM(wasm_interpreter_end)\n"));
    assert!(!assembly.contains("js_interpreter"));
    assert!(!assembly.contains("asm_debugger_trampoline"));
}

#[test]
fn rejects_handlers_without_an_encoding() {
    let error = compile("handler Unencoded() { dispatch_next; }").unwrap_err();
    assert!(error.contains("needs an @opcode or @fallback annotation"), "{error}");

    let error = compile("handler A() @opcode(0x10) { dispatch_next; }\nhandler B() @opcode(0x10) { dispatch_next; }")
        .unwrap_err();
    assert!(error.contains("another handler already has this opcode"), "{error}");

    let error = compile("handler A(dst: out Operand) @opcode(0x10) { dispatch_next; }").unwrap_err();
    assert!(error.contains("cannot declare parameters"), "{error}");

    let error = compile("handler A() @opcode(0xfc, 7) { dispatch_next; }").unwrap_err();
    assert!(
        error.contains("variable-length handler must use variable dispatch"),
        "{error}"
    );
}

#[test]
fn rejects_registers_of_other_profiles() {
    let error = compile("handler A() @opcode(0x10) { let base: u64 = pb; dispatch_variable(base); }").unwrap_err();
    assert!(
        error.contains("the WebAssembly profile has no interpreter register 'pb'"),
        "{error}"
    );
}

fn compile_for(target: Target, source: &str) -> Result<String, String> {
    Compiler::new(CompileOptions {
        target,
        has_jscvt: false,
        has_lzcnt: false,
        has_bmi1: false,
        enable_assertions: true,
        profiling: false,
        profile: Profile::WebAssembly,
    })
    .compile(CompilationUnit {
        source: SourceInput {
            name: "wasm.flap",
            contents: source,
        },
        constants: None,
    })
    .map(|assembly| assembly.as_str().to_owned())
    .map_err(|error| error.to_string())
}

fn compile_aarch64(source: &str) -> Result<String, String> {
    compile_for(
        Target {
            architecture: Architecture::Aarch64,
            object_format: ObjectFormat::Elf,
        },
        source,
    )
}

#[test]
fn is_not_available_for_aarch64_windows() {
    let error = compile_for(
        Target {
            architecture: Architecture::Aarch64,
            object_format: ObjectFormat::Coff,
        },
        PROGRAM,
    )
    .unwrap_err();
    assert!(error.contains("not implemented for AArch64 Windows"), "{error}");
}

#[test]
fn dispatches_on_the_byte_the_instruction_pointer_points_at_on_aarch64() {
    let assembly = compile_aarch64(PROGRAM).unwrap();
    let nop = handler(&assembly, "Nop");
    assert!(nop.contains("ldrb w9, [x21, #1]\n"), "{nop}");
    assert!(nop.contains("add x21, x21, #1\n"), "{nop}");
    assert!(nop.contains("ldr x10, [x19, x9, lsl #3]\n    br x10"), "{nop}");
    assert!(!assembly.contains("x26, w25"), "{assembly}");

    let local_get = handler(&assembly, "LocalGet");
    assert!(local_get.contains("[x21, #1]"), "{local_get}");
    assert!(local_get.contains("[x29, "), "{local_get}");
    assert!(local_get.contains("[x27]"), "{local_get}");

    let entry = assembly.split("CSYM(wasm_interpreter):\n").nth(1).unwrap();
    assert!(entry.contains("mov x21, x0"), "{entry}");
    assert!(entry.contains("mov x27, x1"), "{entry}");
    assert!(entry.contains(":lo12:asm_dispatch_table"), "{entry}");
    assert!(assembly.contains(".Lexit:\n    mov sp, x29\n"), "{assembly}");
}

#[test]
fn dispatches_prefixed_opcodes_through_their_secondary_table_on_aarch64() {
    let assembly = compile_aarch64(
        r#"
handler Prefix() @opcode(0xfc) {
    let bytes: Sequence<u8> = alias(ip);
    let sub_opcode: u64 = alias(bytes[1]);
    dispatch_secondary(0xfc, sub_opcode);
}

handler Saturating() @opcode(0xfc, 3) {
    dispatch_variable(2);
}

handler Fallback() @fallback {
    exit();
}
"#,
    )
    .unwrap();
    let prefix = handler(&assembly, "Prefix");
    assert!(prefix.contains("adrp x9, asm_dispatch_table_fc\n"), "{prefix}");
    assert!(prefix.contains("add x9, x9, :lo12:asm_dispatch_table_fc\n"), "{prefix}");
    assert!(prefix.contains("ldr x10, [x9, x"), "{prefix}");
    assert!(prefix.contains(", lsl #3]\n    br x10"), "{prefix}");
}

#[test]
fn compiles_atomic_operations_on_aarch64() {
    let assembly = compile_aarch64(
        r#"
handler Atomics() @opcode(0x01) {
    let slots: Sequence<u64> = alias(vsp);
    let address = slots[0];
    let value = slots[1];
    let old = atomic_cmpxchg32([address, 4], value, 7);
    let swapped = atomic_xchg8([address, 0], old);
    let added = atomic_add64([address, 8], swapped);
    let loaded = atomic_load16([address, 0]);
    atomic_fence();
    slots[2] = added + loaded;
    dispatch_next;
}

handler Fallback() @fallback {
    exit();
}
"#,
    )
    .unwrap();
    let atomics = handler(&assembly, "Atomics");
    for expected in [
        "add x9, x",
        ", #4\n",
        "casal w",
        "swpalb w",
        "ldaddal x",
        "ldarh w",
        "dmb ish",
    ] {
        assert!(atomics.contains(expected), "missing {expected}: {atomics}");
    }
}

#[test]
fn compiles_atomic_loads_and_fences_on_x86_64() {
    let assembly = compile(
        r#"
handler Atomics() @opcode(0x01) {
    let slots: Sequence<u64> = alias(vsp);
    let address = slots[0];
    let loaded = atomic_load8([address, 3]);
    atomic_fence();
    slots[1] = loaded;
    dispatch_next;
}

handler Fallback() @fallback {
    exit();
}
"#,
    )
    .unwrap();
    let atomics = handler(&assembly, "Atomics");
    assert!(atomics.contains("movzx e"), "{atomics}");
    assert!(atomics.contains("BYTE PTR [r"), "{atomics}");
    assert!(atomics.contains("mfence"), "{atomics}");
}

#[test]
fn compiles_numeric_and_vector_operations_on_aarch64() {
    let assembly = compile_aarch64(
        r#"
handler Numbers() @opcode(0x01) {
    let slots: Sequence<u64> = alias(vsp);
    let a = slots[0];
    let b = slots[1];
    let narrow_a = u32(a);
    let narrow_b = u32(b);
    let quotient = div_u32(narrow_a, narrow_b);
    let remainder = rem_s64(a, b);
    let zeros = ctz64(a);
    let ones = popcnt32(narrow_b);
    let rotated = rotl32(narrow_a, narrow_b);
    let narrow = quotient + ones + rotated;
    let wide: u64 = alias(narrow);
    slots[2] = remainder + zeros + wide;
    dispatch_next;
}

handler Vectors() @opcode(0x02) {
    let top: u64 = vsp;
    let mut a: v128;
    load128(a, [top, 0]);
    let mut b: v128;
    load128(b, [top, 16]);
    let sum = v128_add_i32(a, b);
    let smaller = v128_min_f32(sum, b);
    let shuffled = v128_shuffle_i8(smaller, a);
    store128([top, 32], shuffled);
    let slots: Sequence<u64> = alias(top);
    let mask = v128_bitmask_i8(shuffled);
    let high = v128_high_u64(a);
    slots[7] = mask + high;
    dispatch_next;
}

handler Fallback() @fallback {
    exit();
}
"#,
    )
    .unwrap();
    let numbers = handler(&assembly, "Numbers");
    for expected in [
        "udiv w",
        "sdiv x9, x",
        "msub x",
        "rbit x9, x",
        "cnt v16.8b, v16.8b",
        "neg w9, w",
        "ror w",
    ] {
        assert!(numbers.contains(expected), "missing {expected}: {numbers}");
    }
    let vectors = handler(&assembly, "Vectors");
    for expected in [
        "add v",
        ".4s\n",
        "fcmgt v16.4s, v",
        "bsl v16.16b, v",
        "movi v16.16b, #0x8f",
        "tbl v",
        "usra v16.2d, v16.2d, #28",
        ", v",
        ".d[1]\n",
    ] {
        assert!(vectors.contains(expected), "missing {expected}: {vectors}");
    }
}

#[test]
fn calls_runtime_functions_with_the_system_v_convention_on_windows() {
    let assembly = compile_for(
        Target {
            architecture: Architecture::X86_64,
            object_format: ObjectFormat::Coff,
        },
        r#"
handler Grow() @opcode(0x01) {
    let slots: Sequence<u64> = alias(vsp);
    let context = slots[0];
    let result: u64 = call_helper(wasm_runtime_memory_grow, context);
    slots[1] = result;
    dispatch_next;
}

handler Fallback() @fallback {
    exit();
}
"#,
    )
    .unwrap();
    let grow = handler(&assembly, "Grow");
    assert!(
        grow.contains("mov rdi, rcx\n    call CSYM(wasm_runtime_memory_grow)"),
        "{grow}"
    );
    let entry = assembly.split("CSYM(wasm_interpreter):\n").nth(1).unwrap();
    assert!(entry.contains(".seh_pushreg rbx"), "{entry}");
    assert!(entry.contains(".seh_endprologue"), "{entry}");
    assert!(entry.contains("mov r13, rdi"), "{entry}");
    assert!(!entry.contains(".cfi_"), "{entry}");
}

/// Twelve values live at once, more than there are allocatable registers.
const HIGH_PRESSURE: &str = r#"
handler Sum() @opcode(0x01) {
    let slots: Sequence<u64> = alias(vsp);
    let a = slots[0];
    let b = slots[1];
    let c = slots[2];
    let d = slots[3];
    let e = slots[4];
    let f = slots[5];
    let g = slots[6];
    let h = slots[7];
    let i = slots[8];
    let j = slots[9];
    let k = slots[10];
    let l = slots[11];
    slots[12] = a + l;
    slots[13] = b + k;
    slots[14] = c + j;
    slots[15] = d + i;
    slots[16] = e + h;
    slots[17] = f + g;
    slots[18] = a + b + c + d + e + f + g + h + i + j + k + l;
    dispatch_next;
}

handler Fallback() @fallback {
    exit();
}
"#;

#[test]
fn spills_to_the_spill_area_when_registers_run_out() {
    let error = compile(HIGH_PRESSURE).unwrap_err();
    assert!(error.contains("could not allocate spill-free handler"), "{error}");

    let assembly = compile_with_constants(
        HIGH_PRESSURE,
        "const SPILL_AREA_OFFSET = 64\nconst SPILL_AREA_SLOTS = 8\n",
    )
    .unwrap();
    let sum = handler(&assembly, "Sum");
    assert!(sum.contains("QWORD PTR [rsp + 64]"), "{sum}");
    assert!(!sum.contains("QWORD PTR [rsp + 128]"), "{sum}");
}

const FLOAT_PRESSURE: &str = r#"
handler FloatSum() @opcode(0x01) {
    let slots: Sequence<u64> = alias(vsp);
    let a = f64_from_bits(slots[0]);
    let b = f64_from_bits(slots[1]);
    let c = f64_from_bits(slots[2]);
    let d = f64_from_bits(slots[3]);
    let e = f64_from_bits(slots[4]);
    let f = f64_from_bits(slots[5]);
    let ab = a + b;
    let abc = ab + c;
    let abcd = abc + d;
    let abcde = abcd + e;
    let sum = abcde + f;
    let fe = f * e;
    let fed = fe * d;
    let fedc = fed * c;
    let fedcb = fedc * b;
    let product = fedcb * a;
    slots[6] = f64_to_bits(sum);
    slots[7] = f64_to_bits(product);
    dispatch_next;
}

handler Fallback() @fallback {
    exit();
}
"#;

#[test]
fn spills_floating_point_values() {
    let error = compile(FLOAT_PRESSURE).unwrap_err();
    assert!(error.contains("could not allocate spill-free handler"), "{error}");

    let assembly = compile_with_constants(
        FLOAT_PRESSURE,
        "const SPILL_AREA_OFFSET = 64\nconst SPILL_AREA_SLOTS = 8\n",
    )
    .unwrap();
    let sum = handler(&assembly, "FloatSum");
    assert!(sum.contains("movsd QWORD PTR [rsp + 64], xmm"), "{sum}");
}

const VECTOR_PRESSURE: &str = r#"
handler Reverse() @opcode(0x01) {
    let top: u64 = vsp;
    let mut a: v128;
    load128(a, [top, 0]);
    let mut b: v128;
    load128(b, [top, 16]);
    let mut c: v128;
    load128(c, [top, 32]);
    let mut d: v128;
    load128(d, [top, 48]);
    let mut e: v128;
    load128(e, [top, 64]);
    let mut f: v128;
    load128(f, [top, 80]);
    store128([top, 96], f);
    store128([top, 112], e);
    store128([top, 128], d);
    store128([top, 144], c);
    store128([top, 160], b);
    store128([top, 176], a);
    dispatch_next;
}

handler Fallback() @fallback {
    exit();
}
"#;

#[test]
fn keeps_and_spills_vectors_whole() {
    let error = compile(VECTOR_PRESSURE).unwrap_err();
    assert!(error.contains("could not allocate spill-free handler"), "{error}");

    let assembly = compile_with_constants(
        VECTOR_PRESSURE,
        "const SPILL_AREA_OFFSET = 8\nconst SPILL_AREA_SLOTS = 8\n",
    )
    .unwrap();
    let reverse = handler(&assembly, "Reverse");
    // Vector slots take 16 bytes each, starting on a 16-byte boundary.
    assert!(reverse.contains("movdqu XMMWORD PTR [rsp + 16], xmm"), "{reverse}");
    assert!(reverse.contains("movdqu XMMWORD PTR [rsp + 32], xmm"), "{reverse}");
    assert!(
        !reverse.contains("[rsp + 8]") && !reverse.contains("[rsp + 24]"),
        "{reverse}"
    );
}

#[test]
fn moves_vectors_with_full_width_moves() {
    let assembly = compile(
        r#"
handler Swap() @opcode(0x01) {
    let top: u64 = vsp;
    let mut a: v128;
    load128(a, [top, 0]);
    let mut b: v128;
    load128(b, [top, 16]);
    let mut first: v128 = a;
    let mut second: v128 = b;
    let slots: Sequence<u64> = alias(top);
    let flag = slots[4];
    if flag != 0 {
        first = b;
        second = a;
    }
    store128([top, 32], first);
    store128([top, 48], second);
    dispatch_next;
}

handler Fallback() @fallback {
    exit();
}
"#,
    )
    .unwrap();
    let swap = handler(&assembly, "Swap");
    assert!(swap.contains("movaps xmm"), "{swap}");
    assert!(!swap.contains("movq xmm"), "{swap}");
}

const ACROSS_CALL: &str = r#"
handler Call() @opcode(0x01) {
    let slots: Sequence<u64> = alias(vsp);
    let kept = slots[0];
    let current: u64 = ip;
    let result: u64 = call_helper(wasm_helper, kept);
    let sum = result + kept;
    slots[1] = sum;
    let next = current + 1;
    ip = next;
    dispatch;
}

handler Fallback() @fallback {
    exit();
}
"#;

#[test]
fn spills_values_live_across_calls_first() {
    let error = compile(ACROSS_CALL).unwrap_err();
    assert!(error.contains("could not allocate spill-free handler"), "{error}");

    // One slot is enough: the copy of ip is reread from ip, so only the value is stored.
    let assembly = compile_with_constants(
        ACROSS_CALL,
        "const SPILL_AREA_OFFSET = 64\nconst SPILL_AREA_SLOTS = 1\n",
    )
    .unwrap();
    let call = handler(&assembly, "Call");
    assert!(call.contains("QWORD PTR [rsp + 64]"), "{call}");
    assert!(call.contains("call "), "{call}");
}

#[test]
fn compiles_atomic_operations() {
    let assembly = compile(
        r#"
handler Atomics() @opcode(0x01) {
    let slots: Sequence<u64> = alias(vsp);
    let address = slots[0];
    let value = slots[1];
    let old = atomic_cmpxchg32([address, 4], value, 7);
    let swapped = atomic_xchg8([address, 0], old);
    let added = atomic_add64([address, 8], swapped);
    slots[2] = added;
    dispatch_next;
}

handler Fallback() @fallback {
    exit();
}
"#,
    )
    .unwrap();
    let atomics = handler(&assembly, "Atomics");
    for expected in [
        "lock cmpxchg DWORD PTR [",
        "xchg BYTE PTR [",
        "movzx e",
        "lock xadd QWORD PTR [",
    ] {
        assert!(atomics.contains(expected), "missing {expected}: {atomics}");
    }
}

#[test]
fn compiles_vector_operations_to_sse_instructions() {
    let assembly = compile(
        r#"
handler Vectors() @opcode(0x01) {
    let top: u64 = vsp;
    let mut a: v128;
    load128(a, [top, 0]);
    let mut b: v128;
    load128(b, [top, 16]);
    let sum = v128_add_i32(a, b);
    let rounded = v128_floor_f64(sum);
    store128([top, 32], rounded);
    let slots: Sequence<u64> = alias(top);
    let low = slots[6];
    let splat = v128_from_u64x2(low, low);
    let mask = v128_bitmask_i8(splat);
    let high = v128_high_u64(a);
    slots[7] = mask + high;
    dispatch_next;
}

handler Fallback() @fallback {
    exit();
}
"#,
    )
    .unwrap();
    let vectors = handler(&assembly, "Vectors");
    for expected in [
        "paddd xmm",
        "roundpd xmm",
        ", 9\n",
        "pinsrq xmm",
        "pmovmskb e",
        "pextrq r",
    ] {
        assert!(vectors.contains(expected), "missing {expected}: {vectors}");
    }
}

const ACROSS_STACK_POINTER_WRITE: &str = r#"
handler NewFrame() @opcode(0x01) {
    let slots: Sequence<u64> = alias(vsp);
    let a = slots[0];
    let b = slots[1];
    let c = slots[2];
    let d = slots[3];
    let e = slots[4];
    let f = slots[5];
    let g = slots[6];
    let h = slots[7];
    let i = slots[8];
    let j = slots[9];
    let old_stack_pointer: u64 = sp;
    let new_stack_pointer = old_stack_pointer - 512;
    sp = new_stack_pointer;
    slots[10] = a + j;
    slots[11] = b + i;
    slots[12] = c + h;
    slots[13] = d + g;
    slots[14] = e + f;
    slots[15] = a + b + c + d + e + f + g + h + i + j;
    dispatch_next;
}

handler Fallback() @fallback {
    exit();
}
"#;

#[test]
fn spills_values_live_across_writes_to_the_stack_pointer() {
    let assembly = compile_with_constants(
        ACROSS_STACK_POINTER_WRITE,
        "const SPILL_AREA_OFFSET = 0\nconst SPILL_AREA_SLOTS = 8\n",
    )
    .unwrap();
    let frame = handler(&assembly, "NewFrame");
    let write = frame.find("mov rsp, ").expect("the handler writes rsp");
    let (before, after) = frame.split_at(write);
    // A value spilled before the write moves to its slot below the new stack pointer, and is reread from there.
    let new_stack_pointer = after["mov rsp, ".len()..].lines().next().unwrap().trim();
    assert!(
        before.contains(&format!("QWORD PTR [{new_stack_pointer}"))
            || before.contains(&format!("QWORD PTR [{new_stack_pointer} +")),
        "{frame}"
    );
    assert!(after.contains("QWORD PTR [rsp"), "{frame}");
}

#[test]
fn does_not_spill_handlers_that_fit() {
    let assembly =
        compile_with_constants(PROGRAM, "const SPILL_AREA_OFFSET = 64\nconst SPILL_AREA_SLOTS = 8\n").unwrap();
    assert!(!assembly.contains("rsp +"));
}

#[test]
fn wraps_u32_arithmetic_to_32_bits() {
    let assembly = compile(
        r#"
handler Arithmetic() @opcode(0x01) {
    let slots: Sequence<u64> = alias(vsp);
    let lhs = u32(slots[0]);
    let rhs = u32(slots[1]);
    let sum = lhs + rhs;
    let difference = rhs - lhs;
    let product = lhs * rhs;
    let wide_sum: u64 = alias(sum);
    let wide_difference: u64 = alias(difference);
    let wide_product: u64 = alias(product);
    slots[1] = wide_sum;
    slots[2] = wide_difference;
    slots[3] = wide_product;
    dispatch_next;
}

handler Fallback() @fallback {
    exit();
}
"#,
    )
    .unwrap();
    let arithmetic = handler(&assembly, "Arithmetic");
    assert!(arithmetic.contains("add e"), "{arithmetic}");
    assert!(arithmetic.contains("sub e"), "{arithmetic}");
    assert!(arithmetic.contains("imul e"), "{arithmetic}");
    assert!(
        !arithmetic
            .lines()
            .any(|line| line.trim_start().starts_with("add r") && !line.contains("r13")),
        "{arithmetic}"
    );
}

#[test]
fn accepts_literal_left_operands_and_nested_arguments() {
    let assembly = compile(
        r#"
inline fn advance(length: u64) {
    let current: u64 = ip;
    let next = current + length;
    ip = next;
    dispatch;
}

handler Skip() @opcode(0x01) {
    let slots: Sequence<u64> = alias(vsp);
    let length = slots[0];
    let extra = slots[1];
    advance(1 + length + (extra >> 1));
}

handler Fallback() @fallback {
    exit();
}
"#,
    )
    .unwrap();
    let skip = handler(&assembly, "Skip");
    assert!(skip.contains("mov r13, "), "{skip}");
}

#[test]
fn materializes_wide_immediates() {
    let assembly = compile(
        r#"
handler Wide() @opcode(0x01) {
    let slots: Sequence<u64> = alias(vsp);
    let value = slots[0];
    let flipped = value ^ 0x8000000000000000;
    slots[1] = flipped;
    let masked = value & 0x3ffffffff;
    slots[2] = masked;
    let low = value & 0xffffffff;
    slots[3] = low;
    let spread = value * 0x0101010101010101;
    slots[4] = spread;
    if value == 0x8000000000000000 {
        exit();
    }
    dispatch_next;
}

handler Fallback() @fallback {
    exit();
}
"#,
    )
    .unwrap();
    let wide = handler(&assembly, "Wide");
    assert!(
        wide.contains("0x8000000000000000") || wide.contains("9223372036854775808"),
        "{wide}"
    );
    assert!(wide.contains("xor r"), "{wide}");
    assert!(wide.contains("and r"), "{wide}");
    assert!(wide.contains("imul r"), "{wide}");
    assert!(
        wide.lines().any(|line| line.trim_start().starts_with("mov e")),
        "{wide}"
    );
    assert!(wide.contains("cmp r"), "{wide}");
}

#[test]
fn cold_blocks_reload_instead_of_reusing_hot_loads() {
    let assembly = compile(
        r#"
handler Reload() @opcode(0x01) {
    let slots: Sequence<u64> = alias(vsp);
    let value = slots[1];
    let flag = slots[0];
    if flag == 0 @cold {
        let again = slots[1];
        slots[3] = again;
        exit();
    }
    slots[2] = value;
    dispatch_next;
}

handler Fallback() @fallback {
    exit();
}
"#,
    )
    .unwrap();
    // The cold block is laid out away from the handler, so this counts the loads in both.
    assert_eq!(assembly.matches("QWORD PTR [rbx + 8]").count(), 2, "{assembly}");
}

#[test]
fn compiles_numeric_operations_to_single_instructions() {
    let assembly = compile_with_constants(
        r#"
handler Numeric() @opcode(0x01) {
    let slots: Sequence<u64> = alias(vsp);
    let lhs = u32(slots[0]);
    let rhs = u32(slots[1]);
    let quotient = div_u32(lhs, rhs);
    let remainder = rem_s32(lhs, rhs);
    let leading = clz32(lhs);
    let trailing = ctz64(slots[2]);
    let population = popcnt64(slots[3]);
    let rotated = rotl32(lhs, rhs);
    let float = f64_from_bits(slots[4]);
    let truncated = trunc_f64(float);
    let bits = f64_to_bits(truncated);
    let wide_quotient: u64 = alias(quotient);
    let wide_remainder: u64 = alias(remainder);
    let wide_leading: u64 = alias(leading);
    let wide_rotated: u64 = alias(rotated);
    slots[5] = wide_quotient;
    slots[6] = wide_remainder;
    slots[7] = wide_leading;
    slots[8] = trailing;
    slots[9] = population;
    slots[10] = wide_rotated;
    slots[11] = bits;
    dispatch_next;
}

handler Fallback() @fallback {
    exit();
}
"#,
        "const SPILL_AREA_OFFSET = 64\nconst SPILL_AREA_SLOTS = 8\n",
    )
    .unwrap();
    let numeric = handler(&assembly, "Numeric");
    for instruction in [
        "div ecx",
        "idiv ecx",
        "lzcnt e",
        "tzcnt r",
        "popcnt r",
        "rol e",
        "movq xmm",
        "roundsd xmm0, xmm0, 3",
    ] {
        assert!(numeric.contains(instruction), "missing {instruction}:\n{numeric}");
    }
}

#[test]
fn counts_zeros_with_bit_scans_without_lzcnt_and_bmi1() {
    let assembly = compile_for(
        Target {
            architecture: Architecture::X86_64,
            object_format: ObjectFormat::Elf,
        },
        r#"
handler Zeros() @opcode(0x01) {
    let slots: Sequence<u64> = alias(vsp);
    let leading = clz32(u32(slots[0]));
    let trailing = ctz64(slots[1]);
    let wide_leading: u64 = alias(leading);
    slots[2] = wide_leading;
    slots[3] = trailing;
    dispatch_next;
}

handler Fallback() @fallback {
    exit();
}
"#,
    )
    .unwrap();
    let zeros = handler(&assembly, "Zeros");
    for instruction in ["bsr e", ", 63", "xor e", ", 31", "bsf r", ", 64"] {
        assert!(zeros.contains(instruction), "missing {instruction}:\n{zeros}");
    }
    for instruction in ["lzcnt", "tzcnt"] {
        assert!(!zeros.contains(instruction), "unexpected {instruction}:\n{zeros}");
    }
}

#[test]
fn moves_continuing_cold_ifs_out_of_line() {
    let assembly = compile(
        r#"
handler Decode() @opcode(0x01) {
    let bytes: Sequence<u8> = alias(ip);
    let first: u64 = alias(bytes[1]);
    let mut length: u64 = 2;
    if first >= 128 @cold {
        let mut byte: u64 = first;
        while byte >= 128 {
            byte = alias(bytes[length]);
            length = length + 1;
        }
    }
    dispatch_variable(length);
}

handler Fallback() @fallback {
    exit();
}
"#,
    )
    .unwrap();
    let hot = handler(&assembly, "Decode");
    assert!(!hot.contains("cmp rax, 128\n    jb"), "{hot}");
    let cold = assembly.split("asm_handler_Decode_cold:").nth(1).unwrap();
    let cold = cold.split("asm_handler_Decode_cold_end:").next().unwrap();
    assert!(cold.contains("cmp"), "{cold}");
}

#[test]
fn rereads_copies_of_interpreter_registers_instead_of_spilling_them() {
    let source = HIGH_PRESSURE
        .replace(
            "    let slots: Sequence<u64> = alias(vsp);",
            "    let start: u64 = ip;\n    let slots: Sequence<u64> = alias(vsp);",
        )
        .replace(
            "    dispatch_next;\n}\n\nhandler Fallback",
            "    let next = start + 1;\n    ip = next;\n    dispatch;\n}\n\nhandler Fallback",
        );
    let assembly =
        compile_with_constants(&source, "const SPILL_AREA_OFFSET = 64\nconst SPILL_AREA_SLOTS = 8\n").unwrap();
    let sum = handler(&assembly, "Sum");
    assert!(sum.contains(", r13\n"), "{sum}");
    assert!(
        !sum.lines()
            .any(|line| line.contains("PTR [rsp") && line.trim_start().starts_with("mov r") && line.contains("r13")),
        "{sum}"
    );
}

#[test]
fn compares_floats_with_ieee_semantics() {
    let assembly = compile(
        r#"
handler Compare() @opcode(0x01) {
    let slots: Sequence<u64> = alias(vsp);
    let lhs = f64_from_bits(slots[0]);
    let rhs = f64_from_bits(slots[1]);
    let mut result: u64 = 0;
    if lhs < rhs {
        result = 1;
    }
    if lhs != rhs {
        result = result + 2;
    }
    slots[2] = result;
    dispatch_next;
}

handler Fallback() @fallback {
    exit();
}
"#,
    )
    .unwrap();
    let compare = handler(&assembly, "Compare");
    // An unordered comparison sets the "below" flags, so a less-than tests the swapped operands for "above".
    assert!(compare.contains("ucomisd xmm1, xmm0\n    ja "), "{compare}");
    assert!(compare.contains("jp "), "{compare}");
}

#[test]
fn dispatches_prefixed_opcodes_through_their_secondary_table() {
    let assembly = compile(
        r#"
handler Prefix() @opcode(0xfc) {
    let bytes: Sequence<u8> = alias(ip);
    let sub_opcode: u64 = alias(bytes[1]);
    dispatch_secondary(0xfc, sub_opcode);
}

handler Saturating() @opcode(0xfc, 3) {
    dispatch_variable(2);
}

handler Fallback() @fallback {
    exit();
}
"#,
    )
    .unwrap();
    let prefix = handler(&assembly, "Prefix");
    assert!(prefix.contains("lea rax, [rip + asm_dispatch_table_fc]"), "{prefix}");
    assert!(prefix.contains("jmp [rax + "), "{prefix}");

    // 0xff is free for a runtime's own prefixed forms.
    let assembly = compile(
        r#"
handler Internal() @opcode(0xff) {
    let bytes: Sequence<u8> = alias(ip);
    let sub_opcode: u64 = alias(bytes[1]);
    dispatch_secondary(0xff, sub_opcode);
}

handler Quickened() @opcode(0xff, 0x28) {
    dispatch_variable(2);
}

handler Fallback() @fallback {
    exit();
}
"#,
    )
    .unwrap();
    assert!(assembly.contains("asm_dispatch_table_ff:"), "{assembly}");
    assert!(handler(&assembly, "Internal").contains("[rip + asm_dispatch_table_ff]"));

    let error = compile("handler A() @opcode(0x10, 3) { dispatch_variable(2); }").unwrap_err();
    assert!(
        error.contains("secondary dispatch tables exist for the prefixes"),
        "{error}"
    );
}
