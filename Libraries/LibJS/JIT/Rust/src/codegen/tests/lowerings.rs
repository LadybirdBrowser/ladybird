/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Runs the instructions built as IR on many operand values and checks their results against the JS
//! semantics, and that every other operand value goes to the slow path
//! (whose fakes here produce `MARKER`).

use super::*;
use crate::builder::OperationInput;
use crate::bytecode::EnvironmentCoordinate;
use crate::ir::BinaryOp;
use crate::ir::Comparison;

/// What the fake slow paths here produce.
const MARKER: u64 = (value::UNDEFINED_TAG as u64) << 48 | 0xDEAD_BEEF;
const NEGATIVE_ZERO: u64 = 1 << 63;
const CANONICAL_NAN: u64 = 0x7FF8_0000_0000_0000;
const STRING_TAG: u16 = 0xFFFA;
const SYMBOL_TAG: u16 = 0xFFFB;
const BIGINT_TAG: u16 = 0xFFFD;
/// The word of the fake VM that the fake jump and comparison slow paths
/// count their calls in.
const VM_SLOW_JUMPS_WORD: usize = 8;

/// The `ArithFeedback` the instructions here run with: every kind of
/// operand, and int32 results that did not fit, so that no instruction
/// exits.
const GENERAL_FEEDBACK: u8 = 1 | 1 << 1 | 1 << 2 | 1 << 3;
/// Every kind of operand, with int32 results that always fit: int32
/// operations exit where theirs do not.
const INT32_RESULTS_FEEDBACK: u8 = 1 | 1 << 1 | 1 << 3;
/// What `run_instruction_with()` results in for code that exited.
const EXITED: u64 = (value::UNDEFINED_TAG as u64) << 48 | 0xE817;

extern "C" fn marker_values(vm: *mut u64, pc: u32, dst: *mut u64, _lhs: u64, _rhs: u64) -> i64 {
    // SAFETY: Compiled code passes the address of a frame slot.
    unsafe { *dst = MARKER };
    continuation_after(vm, pc)
}

extern "C" fn marker_scalar(vm: *mut u64, pc: u32, _instruction: *const u8, _value: u64) -> SlowPathResult {
    SlowPathResult {
        control: continuation_after(vm, pc),
        value: MARKER,
    }
}

/// Counts its calls, and writes `MARKER`.
extern "C" fn marker_comparison(vm: *mut u64, pc: u32, dst: *mut u64, _lhs: u64, _rhs: u64) -> i64 {
    // SAFETY: The fake VM has a word for this, and compiled code passes the
    // address of a frame slot.
    unsafe {
        *vm.add(VM_SLOW_JUMPS_WORD) += 1;
        *dst = MARKER;
    };
    continuation_after(vm, pc)
}

/// Counts its calls, and jumps to the true target.
extern "C" fn marker_jump(vm: *mut u64, _pc: u32, _lhs: u64, _rhs: u64, if_true: u32, _if_false: u32) -> i64 {
    // SAFETY: The fake VM has a word for this.
    unsafe { *vm.add(VM_SLOW_JUMPS_WORD) += 1 };
    i64::from(if_true)
}

/// The word of the fake VM that the fake `PutByValue` slow path counts its
/// calls in, and the one that holds the primitive storage cage base.
const VM_SLOW_PUTS_WORD: usize = 9;
const VM_CAGE_BASE_WORD: usize = 10;

extern "C" fn marker_put(
    vm: *mut u64,
    pc: u32,
    _instruction: *const u8,
    _base: u64,
    _property: u64,
    _src: u64,
) -> SlowPathResult {
    // SAFETY: The tests that put by value give the fake VM this word.
    unsafe { *vm.add(VM_SLOW_PUTS_WORD) += 1 };
    SlowPathResult {
        control: continuation_after(vm, pc),
        value: 0,
    }
}

/// Counts its calls like `marker_put`, and throws: the interpreter is to
/// take over.
extern "C" fn marker_throw(
    vm: *mut u64,
    _pc: u32,
    _instruction: *const u8,
    _base: u64,
    _property: u64,
    _src: u64,
) -> SlowPathResult {
    // SAFETY: The tests that throw give the fake VM this word.
    unsafe { *vm.add(VM_SLOW_PUTS_WORD) += 1 };
    SlowPathResult { control: -1, value: 0 }
}

/// For reads of bindings, which only reach their slow paths where a binding
/// is uninitialized: throws, and the interpreter is to take over.
extern "C" fn throwing_binding_read(_vm: *mut u64, _pc: u32) -> SlowPathResult {
    SlowPathResult { control: -1, value: 0 }
}

/// For Increment and Decrement (one output) and their postfix forms (two).
extern "C" fn marker_record(vm: *mut u64, pc: u32, instruction: *const u8, outputs: *mut u64, _input: u64) -> i64 {
    // SAFETY: `outputs` is the record in the JIT frame, with a word per field.
    unsafe {
        *outputs = MARKER;
        let opcode = *instruction;
        if opcode == OpCode::PostfixIncrement as u8
            || opcode == OpCode::PostfixDecrement as u8
            || opcode == OpCode::GetCalleeAndThisFromEnvironment as u8
        {
            *outputs.add(1) = MARKER;
        }
    }
    continuation_after(vm, pc)
}

static NO_PROPERTY_CACHES: [u64; 16] = [0; 16];

/// Word 0 of fake plain objects, like the virtual table pointer of real
/// ones.
const PLAIN_OBJECT_VTABLE: u64 = 0x7000;

/// What the fake cache probes find: objects whose word 0 is 1 have the
/// property 42 in every cache, and accept every store into their word 1.
extern "C" fn fake_try_get_by_id_cache(base: u64, _cache: *const u64) -> u64 {
    let object = (base & ((1 << 48) - 1)) as *const u64;
    // SAFETY: The tests only probe with objects.
    if unsafe { *object } == 1 {
        value::int32(42)
    } else {
        value::EMPTY
    }
}

extern "C" fn fake_try_put_by_id_cache(_vm: *mut u64, _pc: u32, _instruction: *const u8, values: *const u64) -> i64 {
    // SAFETY: The record has the base, then the value; the tests only probe
    // with objects.
    unsafe {
        let object = (*values & ((1 << 48) - 1)) as *mut u64;
        if *object == 1 {
            *object.add(1) = *values.add(1);
            return 0;
        }
    }
    1
}

fn layout() -> FrameLayout {
    FrameLayout {
        number_of_registers: 8,
        registers_and_locals_count: 10,
        number_of_constants: 2,
        number_of_arguments: 3,
    }
}

fn argument(index: u32) -> Operand {
    Operand::from_raw(layout().arguments_base() + index)
}

fn marker_runtime() -> RuntimeInfo {
    use OpCode as O;
    let mut runtime = runtime();
    let mut set = |opcode: OpCode, address: u64| runtime.slow_paths[opcode as usize] = address;
    for opcode in [
        O::Add,
        O::AddLhsInt32,
        O::AddRhsInt32,
        O::Sub,
        O::SubRhsInt32,
        O::Mul,
        O::MulRhsInt32,
        O::Div,
        O::DivRhsInt32,
        O::Mod,
        O::ModRhsInt32,
        O::BitwiseAnd,
        O::BitwiseAndRhsInt32,
        O::BitwiseOr,
        O::BitwiseOrRhsInt32,
        O::BitwiseXor,
        O::BitwiseXorRhsInt32,
        O::LeftShift,
        O::LeftShiftRhsInt32,
        O::RightShift,
        O::RightShiftRhsInt32,
        O::UnsignedRightShift,
        O::UnsignedRightShiftRhsInt32,
    ] {
        set(opcode, marker_values as *const () as u64);
    }
    for opcode in [
        O::LessThan,
        O::LessThanRhsInt32,
        O::LessThanEquals,
        O::LessThanEqualsRhsInt32,
        O::GreaterThan,
        O::GreaterThanRhsInt32,
        O::GreaterThanEquals,
        O::GreaterThanEqualsRhsInt32,
        O::StrictlyEquals,
        O::StrictlyEqualsRhsInt32,
        O::StrictlyInequals,
        O::StrictlyInequalsRhsInt32,
        O::LooselyEquals,
        O::LooselyEqualsRhsInt32,
        O::LooselyInequals,
        O::LooselyInequalsRhsInt32,
    ] {
        set(opcode, marker_comparison as *const () as u64);
    }
    for opcode in 0..crate::bytecode::NUM_OPCODES {
        let opcode = OpCode::from_u8(u8::try_from(opcode).unwrap()).unwrap();
        let name = opcode.name();
        if name.starts_with("Jump") && slow_path_call(opcode) == Some(SlowPathCall::JumpValues) {
            set(opcode, marker_jump as *const () as u64);
        }
    }
    for opcode in [
        O::UnaryPlus,
        O::UnaryMinus,
        O::BitwiseNot,
        O::ToInt32,
        O::ToObject,
        O::ToLength,
    ] {
        set(opcode, marker_scalar as *const () as u64);
    }
    for opcode in [O::GetByValue, O::GetLength, O::GetGlobal, O::GetById, O::In] {
        set(opcode, marker_scalar as *const () as u64);
    }
    for opcode in [
        O::PutByValue,
        O::SetLexicalBinding,
        O::SetVariableBinding,
        O::SetGlobal,
        O::PutById,
    ] {
        set(opcode, marker_put as *const () as u64);
    }
    for opcode in [
        O::GetBinding,
        O::GetInitializedBinding,
        O::GetCalleeAndThisFromEnvironment,
    ] {
        set(opcode, throwing_binding_read as *const () as u64);
    }
    for opcode in [
        O::CallBuiltinStringPrototypeCharCodeAt,
        O::CallBuiltinStringPrototypeCharAt,
        O::CallBuiltinMathAbs,
        O::CallBuiltinMathFloor,
        O::CallBuiltinMathCeil,
        O::CallBuiltinMathRound,
        O::CallBuiltinMathSqrt,
    ] {
        set(opcode, marker_scalar as *const () as u64);
    }
    for opcode in [O::ObjectPropertyIteratorNext, O::ResolveThisBinding, O::CreateVariable] {
        set(opcode, marker_put as *const () as u64);
    }
    for opcode in [O::ThrowIfTDZ, O::ThrowIfNotObject, O::ThrowIfNullish] {
        set(opcode, marker_throw as *const () as u64);
    }
    runtime.layout = fake_object_layout();
    // Fake objects have the capacity of their inline storage at byte 40,
    // their shape at word 6, their named properties at word 7 and inline
    // storage from word 8 on; shapes their dictionary generation at byte 84.
    runtime.offsets.object_shape = 48;
    runtime.offsets.object_named_properties = 56;
    runtime.object_allocation.inline_storage_offset = 64;
    runtime.object_allocation.inline_capacity_offset = 40;
    runtime.offsets.shape_dictionary_generation = 84;
    runtime.object_allocation.template = vec![PLAIN_OBJECT_VTABLE];
    runtime.dynamic_calls.object_flag_is_ecmascript_function = FLAG_IS_ECMASCRIPT_FUNCTION;
    for opcode in [O::Increment, O::Decrement, O::PostfixIncrement, O::PostfixDecrement] {
        set(opcode, marker_record as *const () as u64);
    }
    runtime
}

/// Compiles `program` with the marker slow paths and runs it with `arguments`.
fn run_program(program: &Program, arguments: &[u64], prepare: impl FnOnce(&mut Machine)) -> (JitResult, Machine) {
    run_program_with_snapshot(program, arguments, |_| {}, prepare)
}

/// Like `run_program()`, with `prepare_snapshot` changing the snapshot first.
fn run_program_with_snapshot(
    program: &Program,
    arguments: &[u64],
    prepare_snapshot: impl FnOnce(&mut Snapshot),
    prepare: impl FnOnce(&mut Machine),
) -> (JitResult, Machine) {
    let mut snapshot = crate::builder::tests::snapshot_for(program, layout());
    snapshot.runtime = marker_runtime();
    snapshot.executables[0].feedback.arith = vec![GENERAL_FEEDBACK];
    prepare_snapshot(&mut snapshot);
    let compiled = compile_for::<crate::asm::MacroAssembler>(&snapshot, &|_| true)
        .unwrap_or_else(|failure| panic!("compilation failed: {failure:?}"));
    let mut machine = Machine::new(layout(), arguments);
    // Room for the words the fakes here use; the cage base is 0.
    machine.vm.resize(12, 0);
    // An executable whose property lookup caches are all empty.
    machine.executable.resize(4, 0);
    machine.executable[3] = NO_PROPERTY_CACHES.as_ptr() as u64;
    machine.frame[4] = machine.executable.as_ptr() as u64;
    prepare(&mut machine);
    let result = machine.run(&snapshot, &compiled);
    (result, machine)
}

/// Runs `instruction`, which writes `r(5)` from the arguments, and returns `r(5)`.
fn run_instruction(instruction: Instruction, arguments: &[u64]) -> u64 {
    run_instruction_with(GENERAL_FEEDBACK, instruction, arguments).0
}

/// Like `run_instruction()`, with arithmetic feedback `feedback`, and
/// `EXITED` if the code exits. Also returns the number of calls of the
/// fake comparison and jump slow paths.
fn run_instruction_with(feedback: u8, instruction: Instruction, arguments: &[u64]) -> (u64, u64) {
    let program = assemble(|_| {
        vec![
            Instruction::Enter,
            instruction.clone(),
            Instruction::Return { value: r(5) },
        ]
    });
    let (result, machine) = run_program_with_snapshot(
        &program,
        arguments,
        |snapshot| snapshot.executables[0].feedback.arith = vec![feedback],
        |_| {},
    );
    let slow_calls = machine.vm[VM_SLOW_JUMPS_WORD];
    if result.status == crate::code::JitStatus::Resume as u64 {
        return (EXITED, slow_calls);
    }
    assert_eq!(result.status, RETURNED);
    (result.value, slow_calls)
}

fn double(number: f64) -> u64 {
    number.to_bits()
}

/// Operand values covering every case the fast paths distinguish.
fn values() -> Vec<u64> {
    let mut values = [
        0,
        1,
        -1,
        2,
        -7,
        7,
        31,
        32,
        33,
        1 << 20,
        i32::MAX,
        i32::MIN,
        i32::MAX - 1,
        i32::MIN + 1,
    ]
    .into_iter()
    .map(value::int32)
    .collect::<Vec<_>>();
    values.extend(
        [
            0.5,
            -2.5,
            3.0,
            -0.0,
            1e10,
            -1e10,
            2_147_483_648.0,
            -2_147_483_649.0,
            4_294_967_297.0,
            1e300,
            9.3e18,
            -9_223_372_036_854_775_808.0,
            f64::INFINITY,
            f64::NEG_INFINITY,
            f64::MIN_POSITIVE,
        ]
        .into_iter()
        .map(double),
    );
    values.extend([
        CANONICAL_NAN,
        value::TRUE,
        value::FALSE,
        value::UNDEFINED,
        value::NULL,
        value::EMPTY,
        object_value(&PLAIN_OBJECTS[0]),
        object_value(&PLAIN_OBJECTS[1]),
        object_value(&HTMLDDA_OBJECT),
        fake_string(false),
        fake_string(false),
        fake_string(true),
        fake_string(true),
        u64::from(SYMBOL_TAG) << 48 | 0x1000,
        u64::from(BIGINT_TAG) << 48 | 0x1000,
    ]);
    values
}

/// The offset of `PrimitiveString::m_is_interned` in the fake strings, see
/// `primitive_string_is_interned` in the test layout.
const FAKE_STRING_IS_INTERNED_OFFSET: usize = 9;

/// A string value pointing at fake (leaked) string memory.
fn fake_string(interned: bool) -> u64 {
    let memory: &'static mut [u8; 32] = Box::leak(Box::new([0u8; 32]));
    memory[FAKE_STRING_IS_INTERNED_OFFSET] = u8::from(interned);
    u64::from(STRING_TAG) << 48 | memory.as_ptr() as u64
}

fn fake_string_is_interned(bits: u64) -> bool {
    let pointer = (bits & ((1 << 48) - 1)) as *const u8;
    // SAFETY: Only called on values made by fake_string(), whose memory is leaked.
    unsafe { *pointer.add(FAKE_STRING_IS_INTERNED_OFFSET) != 0 }
}

/// Fake objects whose flags (at byte 8) fast paths may read.
static PLAIN_OBJECTS: [[u64; 4]; 2] = [[0; 4]; 2];
static HTMLDDA_OBJECT: [u64; 4] = [0, HTMLDDA_FLAG as u64, 0, 0];

/// The number of an int32 or a double (NaN too), which the instructions
/// built as IR handle inline.
fn number(bits: u64) -> Option<f64> {
    if is_int32(bits) {
        Some(f64::from(int(bits)))
    } else if bits == CANONICAL_NAN || (bits >> 48) & 0x7FF8 != 0x7FF8 {
        Some(f64::from_bits(bits))
    } else {
        None
    }
}

/// ToInt32 of a number.
fn to_int32(bits: u64) -> Option<i32> {
    let number = number(bits)?;
    if !number.is_finite() {
        return Some(0);
    }
    Some((number.trunc().rem_euclid(4_294_967_296.0) as u64 as u32).cast_signed())
}

/// The result of `op` with arithmetic `feedback`: `MARKER` for the slow
/// path, `EXITED` where the code exits.
fn binary(op: BinaryOp, lhs: u64, rhs: u64, feedback: u8) -> u64 {
    let int32_results = feedback == INT32_RESULTS_FEEDBACK;
    let both_int32 = is_int32(lhs) && is_int32(rhs);
    let (l, r) = (int(lhs), int(rhs));
    match op {
        BinaryOp::Add | BinaryOp::Sub | BinaryOp::Mul | BinaryOp::Div => {
            if int32_results && both_int32 && op != BinaryOp::Div {
                let exact = match op {
                    BinaryOp::Add => l.checked_add(r),
                    BinaryOp::Sub => l.checked_sub(r),
                    _ => l.checked_mul(r).filter(|product| *product != 0 || (l | r) >= 0),
                };
                return exact.map_or(EXITED, value::int32);
            }
            match (number(lhs), number(rhs)) {
                (Some(l), Some(r)) => box_number(double_operation(op, l, r)),
                _ => MARKER,
            }
        }
        BinaryOp::Mod => {
            if !int32_results || !both_int32 {
                return MARKER;
            }
            if r == 0 {
                return EXITED;
            }
            let remainder = i64::from(l) % i64::from(r);
            if remainder == 0 && l < 0 {
                EXITED
            } else {
                value::int32(remainder as i32)
            }
        }
        _ => {
            let (Some(l), Some(r)) = (to_int32(lhs), to_int32(rhs)) else {
                return MARKER;
            };
            let count = r.cast_unsigned() & 31;
            match op {
                BinaryOp::BitwiseAnd => value::int32(l & r),
                BinaryOp::BitwiseOr => value::int32(l | r),
                BinaryOp::BitwiseXor => value::int32(l ^ r),
                BinaryOp::LeftShift => value::int32(l.wrapping_shl(count)),
                BinaryOp::RightShift => value::int32(l >> count),
                _ => {
                    let result = l.cast_unsigned() >> count;
                    match i32::try_from(result) {
                        Ok(result) => value::int32(result),
                        Err(_) if int32_results => EXITED,
                        Err(_) => f64::from(result).to_bits(),
                    }
                }
            }
        }
    }
}

fn double_operation(op: BinaryOp, lhs: f64, rhs: f64) -> f64 {
    match op {
        BinaryOp::Add => lhs + rhs,
        BinaryOp::Sub => lhs - rhs,
        BinaryOp::Mul => lhs * rhs,
        BinaryOp::Div => lhs / rhs,
        _ => unreachable!(),
    }
}

fn binary_instruction(op: BinaryOp, lhs: OperationInput, rhs: OperationInput) -> Instruction {
    use OperationInput::Int32;
    use OperationInput::Operand as Slot;
    let dst = r(5);
    macro_rules! pick {
        ($slots:ident, $rhs_int32:ident) => {
            match (lhs, rhs) {
                (Slot(lhs), Slot(rhs)) => Instruction::$slots {
                    arith_feedback: 0,
                    dst,
                    lhs,
                    rhs,
                },
                (Slot(lhs), Int32(rhs)) => Instruction::$rhs_int32 {
                    arith_feedback: 0,
                    dst,
                    lhs,
                    rhs,
                },
                _ => unreachable!(),
            }
        };
    }
    match op {
        BinaryOp::Add => match (lhs, rhs) {
            (Int32(lhs), Slot(rhs)) => Instruction::AddLhsInt32 {
                arith_feedback: 0,
                dst,
                lhs,
                rhs,
            },
            _ => pick!(Add, AddRhsInt32),
        },
        BinaryOp::Sub => pick!(Sub, SubRhsInt32),
        BinaryOp::Mul => pick!(Mul, MulRhsInt32),
        BinaryOp::Div => pick!(Div, DivRhsInt32),
        BinaryOp::Mod => pick!(Mod, ModRhsInt32),
        BinaryOp::BitwiseAnd => pick!(BitwiseAnd, BitwiseAndRhsInt32),
        BinaryOp::BitwiseOr => pick!(BitwiseOr, BitwiseOrRhsInt32),
        BinaryOp::BitwiseXor => pick!(BitwiseXor, BitwiseXorRhsInt32),
        BinaryOp::LeftShift => pick!(LeftShift, LeftShiftRhsInt32),
        BinaryOp::RightShift => pick!(RightShift, RightShiftRhsInt32),
        BinaryOp::UnsignedRightShift => pick!(UnsignedRightShift, UnsignedRightShiftRhsInt32),
    }
}

const BINARY_OPS: [BinaryOp; 11] = [
    BinaryOp::Add,
    BinaryOp::Sub,
    BinaryOp::Mul,
    BinaryOp::Div,
    BinaryOp::Mod,
    BinaryOp::BitwiseAnd,
    BinaryOp::BitwiseOr,
    BinaryOp::BitwiseXor,
    BinaryOp::LeftShift,
    BinaryOp::RightShift,
    BinaryOp::UnsignedRightShift,
];

#[test]
fn binary_operations_on_two_operands() {
    let values = values();
    for feedback in [GENERAL_FEEDBACK, INT32_RESULTS_FEEDBACK] {
        for op in BINARY_OPS {
            let instruction = binary_instruction(
                op,
                OperationInput::Operand(argument(0)),
                OperationInput::Operand(argument(1)),
            );
            for lhs in &values {
                for rhs in &values {
                    let (result, _) = run_instruction_with(feedback, instruction.clone(), &[*lhs, *rhs]);
                    assert_eq!(
                        result,
                        binary(op, *lhs, *rhs, feedback),
                        "{op:?} {lhs:#x} {rhs:#x} with feedback {feedback:#x}: got {result:#x}"
                    );
                }
            }
        }
    }
}

#[test]
fn binary_operations_with_an_int32_operand() {
    let values = values();
    for feedback in [GENERAL_FEEDBACK, INT32_RESULTS_FEEDBACK] {
        for op in BINARY_OPS {
            for immediate in [0, 1, -1, 5, 31, 33, i32::MAX, i32::MIN] {
                let instruction = binary_instruction(
                    op,
                    OperationInput::Operand(argument(0)),
                    OperationInput::Int32(immediate),
                );
                for lhs in &values {
                    let (result, _) = run_instruction_with(feedback, instruction.clone(), &[*lhs, value::UNDEFINED]);
                    let expected = binary(op, *lhs, value::int32(immediate), feedback);
                    assert_eq!(result, expected, "{op:?} {lhs:#x} {immediate}: got {result:#x}");
                }
                if op == BinaryOp::Add {
                    let instruction = binary_instruction(
                        op,
                        OperationInput::Int32(immediate),
                        OperationInput::Operand(argument(0)),
                    );
                    for rhs in &values {
                        let (result, _) =
                            run_instruction_with(feedback, instruction.clone(), &[*rhs, value::UNDEFINED]);
                        let expected = binary(op, value::int32(immediate), *rhs, feedback);
                        assert_eq!(result, expected, "{immediate} + {rhs:#x}: got {result:#x}");
                    }
                }
            }
        }
    }
}

#[test]
fn unary_operations() {
    for (feedback, value) in [GENERAL_FEEDBACK, INT32_RESULTS_FEEDBACK]
        .into_iter()
        .flat_map(|feedback| values().into_iter().map(move |value| (feedback, value)))
    {
        let plus = if number(value).is_some() { value } else { MARKER };
        let minus = match number(value) {
            _ if feedback == INT32_RESULTS_FEEDBACK && is_int32(value) && matches!(int(value), 0 | i32::MIN) => EXITED,
            Some(number) => box_number(-number),
            None => MARKER,
        };
        let not = to_int32(value).map_or(MARKER, |integer| value::int32(!integer));
        let to_int32 = to_int32(value).map_or(MARKER, value::int32);
        let to_object = if (value >> 48) as u16 == value::OBJECT_TAG {
            value
        } else {
            MARKER
        };
        let to_length = if is_int32(value) && int(value) >= 0 {
            value
        } else {
            MARKER
        };
        let (dst, src) = (r(5), argument(0));
        for (instruction, expected) in [
            (
                Instruction::UnaryPlus {
                    arith_feedback: 0,
                    dst,
                    src,
                },
                plus,
            ),
            (
                Instruction::UnaryMinus {
                    arith_feedback: 0,
                    dst,
                    src,
                },
                minus,
            ),
            (
                Instruction::BitwiseNot {
                    arith_feedback: 0,
                    dst,
                    src,
                },
                not,
            ),
            (
                Instruction::ToInt32 {
                    arith_feedback: 0,
                    dst,
                    value: src,
                },
                to_int32,
            ),
            (Instruction::ToObject { dst, value: src }, to_object),
            (Instruction::ToLength { dst, value: src }, to_length),
        ] {
            let (result, _) = run_instruction_with(feedback, instruction.clone(), &[value, value::UNDEFINED]);
            assert_eq!(
                result, expected,
                "{instruction:?} of {value:#x} with feedback {feedback:#x}: got {result:#x}"
            );
        }
    }
}

fn updated(value: u64, delta: i32, feedback: u8) -> u64 {
    if feedback == INT32_RESULTS_FEEDBACK && is_int32(value) {
        return int(value).checked_add(delta).map_or(EXITED, value::int32);
    }
    number(value).map_or(MARKER, |number| box_number(number + f64::from(delta)))
}

#[test]
fn updates() {
    for (feedback, value) in [GENERAL_FEEDBACK, INT32_RESULTS_FEEDBACK]
        .into_iter()
        .flat_map(|feedback| values().into_iter().map(move |value| (feedback, value)))
    {
        let run = |program: &Program| {
            let (result, _) = run_program_with_snapshot(
                program,
                &[value, value::UNDEFINED],
                |snapshot| snapshot.executables[0].feedback.arith = vec![feedback],
                |_| {},
            );
            if result.status == crate::code::JitStatus::Resume as u64 {
                return EXITED;
            }
            assert_eq!(result.status, RETURNED);
            result.value
        };
        for (delta, increment) in [(1, true), (-1, false)] {
            let expected = updated(value, delta, feedback);
            let instruction = if increment {
                Instruction::Increment {
                    arith_feedback: 0,
                    dst: r(5),
                }
            } else {
                Instruction::Decrement {
                    arith_feedback: 0,
                    dst: r(5),
                }
            };
            let program = assemble(|_| {
                vec![
                    Instruction::Enter,
                    Instruction::Mov {
                        dst: r(5),
                        src: argument(0),
                    },
                    instruction.clone(),
                    Instruction::Return { value: r(5) },
                ]
            });
            let result = run(&program);
            assert_eq!(result, expected, "{delta:+} of {value:#x} with feedback {feedback:#x}");

            // Postfix: r6 gets the old value, r5 the new one.
            let instruction = if increment {
                Instruction::PostfixIncrement {
                    arith_feedback: 0,
                    dst: r(6),
                    src: r(5),
                }
            } else {
                Instruction::PostfixDecrement {
                    arith_feedback: 0,
                    dst: r(6),
                    src: r(5),
                }
            };
            let postfix = |returned: Operand| {
                assemble(|_| {
                    vec![
                        Instruction::Enter,
                        Instruction::Mov {
                            dst: r(5),
                            src: argument(0),
                        },
                        instruction.clone(),
                        Instruction::Return { value: returned },
                    ]
                })
            };
            let result = run(&postfix(r(5)));
            assert_eq!(
                result, expected,
                "postfix {delta:+} of {value:#x} with feedback {feedback:#x}"
            );
            let old_value = if matches!(expected, MARKER | EXITED) {
                expected
            } else {
                value
            };
            let result = run(&postfix(r(6)));
            assert_eq!(result, old_value, "old value of postfix {delta:+} of {value:#x}");
        }
    }
}

#[test]
fn slow_paths_preserve_registers() {
    // r6 and r7 live across the Add in registers, and keep them when the Add
    // takes its slow path.
    let program = assemble(|_| {
        vec![
            Instruction::Enter,
            Instruction::Mov {
                dst: r(6),
                src: argument(1),
            },
            Instruction::AddRhsInt32 {
                arith_feedback: 0,
                dst: r(7),
                lhs: argument(1),
                rhs: 1,
            },
            Instruction::Add {
                arith_feedback: 0,
                dst: r(5),
                lhs: argument(0),
                rhs: r(6),
            },
            Instruction::NewArray {
                dst: l(0),
                element_count: 3,
                elements: vec![r(5), r(6), r(7)],
            },
            Instruction::Return { value: l(0) },
        ]
    });
    let add_pc = program.offsets[3];
    // The fast path: 2 + 40 + 40 + 41.
    let (result, _) = run_program(&program, &[value::int32(2), value::int32(40)], |_| {});
    assert_eq!(result, returned(value::int32(123)));
    // The slow path: MARKER (as an int32, 0xDEADBEEF) + 40 + 41.
    let (result, _) = run_program(&program, &[value::UNDEFINED, value::int32(40)], |_| {});
    assert_eq!(result, returned(value::int32(int(MARKER).wrapping_add(81))));
    // An exception that leaves the interpreter needs no more of the frame
    // than where it was thrown.
    let (result, machine) = run_program(&program, &[value::UNDEFINED, value::int32(40)], |machine| {
        machine.state.borrow_mut().fail = Some((add_pc, -1));
    });
    assert_eq!(result.status, EXIT_INTERPRETER);
    assert_eq!(machine.program_counter(), add_pc);
    assert_ne!(machine.slot(r(7)), value::int32(41));
}

fn tag(bits: u64) -> u16 {
    (bits >> 48) as u16
}

/// Strict and loose equality of the kinds of values the instructions built
/// as IR handle inline, or `None` for the slow path: numbers, and strings
/// with the same bits or that are both interned. A strict equality with an
/// int32 immediate is false for anything but numbers.
fn equality(lhs: u64, rhs: u64, loose: bool, rhs_is_immediate: bool) -> Option<bool> {
    if let (Some(lhs), Some(rhs)) = (number(lhs), number(rhs)) {
        return Some(lhs == rhs);
    }
    if rhs_is_immediate {
        return if loose { None } else { Some(false) };
    }
    if tag(lhs) == STRING_TAG && tag(rhs) == STRING_TAG {
        if lhs == rhs {
            return Some(true);
        }
        // Two different interned strings never have the same contents.
        if fake_string_is_interned(lhs) && fake_string_is_interned(rhs) {
            return Some(false);
        }
    }
    None
}

/// The result of a comparison's inline paths, or `None` for the slow path.
fn comparison(comparison: Comparison, lhs: u64, rhs: u64, rhs_is_immediate: bool) -> Option<bool> {
    let relation = |lhs: f64, rhs: f64| match comparison {
        Comparison::LessThan => lhs < rhs,
        Comparison::LessThanEquals => lhs <= rhs,
        Comparison::GreaterThan => lhs > rhs,
        _ => lhs >= rhs,
    };
    let equal = |loose| equality(lhs, rhs, loose, rhs_is_immediate);
    match comparison {
        Comparison::StrictlyEquals => equal(false),
        Comparison::StrictlyInequals => equal(false).map(|equal| !equal),
        Comparison::LooselyEquals => equal(true),
        Comparison::LooselyInequals => equal(true).map(|equal| !equal),
        _ => Some(relation(number(lhs)?, number(rhs)?)),
    }
}

const COMPARISONS: [Comparison; 8] = [
    Comparison::LessThan,
    Comparison::LessThanEquals,
    Comparison::GreaterThan,
    Comparison::GreaterThanEquals,
    Comparison::StrictlyEquals,
    Comparison::StrictlyInequals,
    Comparison::LooselyEquals,
    Comparison::LooselyInequals,
];

fn compare_instruction(comparison: Comparison, dst: Operand, lhs: Operand, rhs: OperationInput) -> Instruction {
    use OperationInput::Int32;
    use OperationInput::Operand as Slot;
    macro_rules! pick {
        ($slots:ident, $rhs_int32:ident) => {
            match rhs {
                Slot(rhs) => Instruction::$slots {
                    arith_feedback: 0,
                    dst,
                    lhs,
                    rhs,
                },
                Int32(rhs) => Instruction::$rhs_int32 {
                    arith_feedback: 0,
                    dst,
                    lhs,
                    rhs,
                },
            }
        };
    }
    match comparison {
        Comparison::LessThan => pick!(LessThan, LessThanRhsInt32),
        Comparison::LessThanEquals => pick!(LessThanEquals, LessThanEqualsRhsInt32),
        Comparison::GreaterThan => pick!(GreaterThan, GreaterThanRhsInt32),
        Comparison::GreaterThanEquals => pick!(GreaterThanEquals, GreaterThanEqualsRhsInt32),
        Comparison::StrictlyEquals => pick!(StrictlyEquals, StrictlyEqualsRhsInt32),
        Comparison::StrictlyInequals => pick!(StrictlyInequals, StrictlyInequalsRhsInt32),
        Comparison::LooselyEquals => pick!(LooselyEquals, LooselyEqualsRhsInt32),
        Comparison::LooselyInequals => pick!(LooselyInequals, LooselyInequalsRhsInt32),
    }
}

/// The conditional jump of `comparison`, in its plain or loop form.
fn compare_jump_instruction(
    comparison: Comparison,
    lhs: Operand,
    rhs: OperationInput,
    is_loop: bool,
    true_target: crate::bytecode::Label,
    false_target: crate::bytecode::Label,
) -> Instruction {
    use OperationInput::Int32;
    use OperationInput::Operand as Slot;
    macro_rules! pick {
        ($slots:ident, $loop_slots:ident, $int32:ident, $loop_int32:ident) => {
            match (rhs, is_loop) {
                (Slot(rhs), false) => Instruction::$slots {
                    arith_feedback: 0,
                    lhs,
                    rhs,
                    true_target,
                    false_target,
                },
                (Slot(rhs), true) => Instruction::$loop_slots {
                    arith_feedback: 0,
                    lhs,
                    rhs,
                    true_target,
                    false_target,
                },
                (Int32(rhs), false) => Instruction::$int32 {
                    arith_feedback: 0,
                    lhs,
                    rhs,
                    true_target,
                    false_target,
                },
                (Int32(rhs), true) => Instruction::$loop_int32 {
                    arith_feedback: 0,
                    lhs,
                    rhs,
                    true_target,
                    false_target,
                },
            }
        };
    }
    match comparison {
        Comparison::LessThan => pick!(
            JumpLessThan,
            JumpLessThanLoop,
            JumpLessThanRhsInt32,
            JumpLessThanLoopRhsInt32
        ),
        Comparison::LessThanEquals => pick!(
            JumpLessThanEquals,
            JumpLessThanEqualsLoop,
            JumpLessThanEqualsRhsInt32,
            JumpLessThanEqualsLoopRhsInt32
        ),
        Comparison::GreaterThan => pick!(
            JumpGreaterThan,
            JumpGreaterThanLoop,
            JumpGreaterThanRhsInt32,
            JumpGreaterThanLoopRhsInt32
        ),
        Comparison::GreaterThanEquals => pick!(
            JumpGreaterThanEquals,
            JumpGreaterThanEqualsLoop,
            JumpGreaterThanEqualsRhsInt32,
            JumpGreaterThanEqualsLoopRhsInt32
        ),
        Comparison::StrictlyEquals => pick!(
            JumpStrictlyEquals,
            JumpStrictlyEqualsLoop,
            JumpStrictlyEqualsRhsInt32,
            JumpStrictlyEqualsLoopRhsInt32
        ),
        Comparison::StrictlyInequals => pick!(
            JumpStrictlyInequals,
            JumpStrictlyInequalsLoop,
            JumpStrictlyInequalsRhsInt32,
            JumpStrictlyInequalsLoopRhsInt32
        ),
        Comparison::LooselyEquals => pick!(
            JumpLooselyEquals,
            JumpLooselyEqualsLoop,
            JumpLooselyEqualsRhsInt32,
            JumpLooselyEqualsLoopRhsInt32
        ),
        Comparison::LooselyInequals => pick!(
            JumpLooselyInequals,
            JumpLooselyInequalsLoop,
            JumpLooselyInequalsRhsInt32,
            JumpLooselyInequalsLoopRhsInt32
        ),
    }
}

/// What a comparison results in: its boolean, or for the slow path, what it
/// makes of the `MARKER` the fake writes, which is no `true`.
fn boolean(result: Option<bool>) -> u64 {
    match result {
        Some(true) => value::TRUE,
        Some(false) | None => value::FALSE,
    }
}

#[test]
fn comparisons() {
    let values = values();
    let check = |instruction: &Instruction, lhs: u64, rhs: u64, expected: Option<bool>| {
        let (result, slow_calls) = run_instruction_with(GENERAL_FEEDBACK, instruction.clone(), &[lhs, rhs]);
        let context = format!("{instruction:?} {lhs:#x} {rhs:#x}: got {result:#x}");
        assert_eq!(result, boolean(expected), "{context}");
        assert_eq!(slow_calls, u64::from(expected.is_none()), "{context}");
    };
    for comparison in COMPARISONS {
        let instruction = compare_instruction(comparison, r(5), argument(0), OperationInput::Operand(argument(1)));
        for lhs in &values {
            for rhs in &values {
                check(
                    &instruction,
                    *lhs,
                    *rhs,
                    self::comparison(comparison, *lhs, *rhs, false),
                );
            }
        }
        for immediate in [0, 1, -1, 7, i32::MAX, i32::MIN] {
            let instruction = compare_instruction(comparison, r(5), argument(0), OperationInput::Int32(immediate));
            for lhs in &values {
                let expected = self::comparison(comparison, *lhs, value::int32(immediate), true);
                check(&instruction, *lhs, value::UNDEFINED, expected);
            }
        }
    }
}

#[test]
fn conditional_jumps() {
    let values = values();
    let rhs_choices = [
        OperationInput::Operand(argument(1)),
        OperationInput::Int32(7),
        OperationInput::Int32(-1),
    ];
    for comparison in COMPARISONS {
        for rhs in rhs_choices {
            for is_loop in [false, true] {
                let program = assemble(|label| {
                    vec![
                        Instruction::Enter,
                        compare_jump_instruction(comparison, argument(0), rhs, is_loop, label(2), label(3)),
                        Instruction::Return { value: c(1) },
                        Instruction::Return { value: c(0) },
                    ]
                });
                let constants = crate::builder::tests::test_constants();
                let (true_value, false_value) = (constants[1], constants[0]);
                for lhs in &values {
                    for rhs_value in &values {
                        let (rhs_bits, rhs_is_immediate) = match rhs {
                            OperationInput::Operand(_) => (*rhs_value, false),
                            OperationInput::Int32(integer) => (value::int32(integer), true),
                        };
                        let (result, machine) = run_program(&program, &[*lhs, *rhs_value], |_| {});
                        let expected = self::comparison(comparison, *lhs, rhs_bits, rhs_is_immediate);
                        let context = format!("{comparison:?} {lhs:#x} {rhs_bits:#x} loop {is_loop}");
                        let slow_jumps = machine.vm[VM_SLOW_JUMPS_WORD];
                        match expected {
                            Some(taken) => {
                                assert_eq!(slow_jumps, 0, "{context}");
                                let value = if taken { true_value } else { false_value };
                                assert_eq!(result, returned(value), "{context}");
                            }
                            None => {
                                assert_eq!(slow_jumps, 1, "{context}");
                                assert_eq!(result, returned(true_value), "{context}");
                            }
                        }
                        if matches!(rhs, OperationInput::Int32(_)) {
                            break;
                        }
                    }
                }
            }
        }
    }
}

// Keyed access, on fake objects laid out like this, in words: 1 the flags,
// 2 the indexed elements, 3 the storage kind (byte 0) and the array-like
// size (bytes 4-7), 4 the typed array data offset, 5 the typed array length
// (bytes 0-3) and kind (byte 4). Strings have their length at byte 16.

const FLAG_HAS_MAGICAL_LENGTH: u16 = 1 << 2;
const FLAG_IS_TYPED_ARRAY: u16 = 1 << 3;
const FLAG_MAY_INTERFERE: u16 = 1 << 4;
const FLAG_IS_EXTENSIBLE: u16 = 1 << 0;
const FLAG_REQUIRES_SLOW_ADD_OWN_PROPERTY: u16 = 1 << 8;
const FLAG_IS_ECMASCRIPT_FUNCTION: u16 = 1 << 6;
const PACKED: u8 = 1;
const HOLEY: u8 = 2;

fn fake_object_layout() -> RuntimeLayout {
    RuntimeLayout {
        object_indexed_elements: 16,
        object_indexed_storage_kind: 24,
        object_indexed_array_like_size: 28,
        indexed_elements_capacity: -8,
        indexed_storage_kind_none: 0,
        indexed_storage_kind_packed: PACKED,
        indexed_storage_kind_holey: HOLEY,
        object_flag_is_extensible: FLAG_IS_EXTENSIBLE,
        object_flag_has_magical_length: FLAG_HAS_MAGICAL_LENGTH,
        object_flag_may_interfere: FLAG_MAY_INTERFERE,
        array_length_writable: 0,
        array_is_proxy_target: 0,
        shape_prototype: 0,
        object_flag_is_typed_array: FLAG_IS_TYPED_ARRAY,
        object_flag_is_htmldda: HTMLDDA_FLAG,
        object_flag_requires_slow_add_own_property: FLAG_REQUIRES_SLOW_ADD_OWN_PROPERTY,
        named_properties_capacity: -8,
        shape_property_count: 80,
        typed_array_cached_data_offset: 32,
        typed_array_cached_data_offset_invalid: u64::MAX,
        typed_array_array_length: 40,
        typed_array_kind: 44,
        typed_array_kind_uint8: 0,
        typed_array_kind_uint8_clamped: 1,
        typed_array_kind_uint16: 2,
        typed_array_kind_uint32: 3,
        typed_array_kind_int8: 4,
        typed_array_kind_int16: 5,
        typed_array_kind_int32: 6,
        typed_array_kind_float32: 7,
        typed_array_kind_float64: 8,
        vm_primitive_storage_cage_base: 8 * VM_CAGE_BASE_WORD as u32,
        primitive_storage_cage_offset_mask: (1 << 48) - 1,
        primitive_string_length: 16,
        primitive_string_storage: 24,
        primitive_string_is_interned: 9,
        primitive_string_interned_mask: 1,
        primitive_string_deferred_kind_mask: 0b110,
        primitive_string_deferred_kind_inline: 0b110,
        primitive_string_inline_storage: 32,
        utf16_short_string_flag: 1,
        utf16_short_string_byte_count_shift: 2,
        utf16_string_data_flags: 16,
        utf16_string_data_has_utf16_storage: 1,
        utf16_string_data_storage: 24,
        single_ascii_character_strings: 0,
        function_object_builtin: 24,
        function_object_has_builtin: 25,
        builtin_string_prototype_char_code_at: 7,
        builtin_string_prototype_char_at: 8,
        builtin_math_abs: 9,
        builtin_math_floor: 10,
        builtin_math_ceil: 11,
        builtin_math_round: 12,
        builtin_math_sqrt: 13,
        typeof_strings: crate::snapshot::TypeofStrings::default(),
        fly_string_cache: 0,
        fly_string_cache_mask: 0,
        numeric_string_cache: 0,
        numeric_string_cache_size: 0,
        environment_outer: 16,
        environment_declarative: 13,
        module_environment_class: MODULE_ENVIRONMENT_CLASS,
        declarative_environment_binding_values_size: 32,
        declarative_environment_binding_values_capacity: 40,
        environment_shape_binding_names: 48,
        environment_shape_has_unique_binding_names: 32,
        declarative_environment_shape: 24,
        declarative_environment_binding_values: 48,
        declarative_environment_rare_data: 56,
        declarative_environment_serial: 64,
        rare_data_binding_flags: 40,
        environment_shape_binding_flags_size: 40,
        environment_shape_binding_flags: 56,
        binding_flag_mutable: BINDING_MUTABLE,
        binding_flag_strict: 1,
        binding_flag_can_be_deleted: 4,
        realm_global_object: 24,
        realm_global_declarative_environment: 32,
        executable_global_variable_caches: 8,
        global_variable_cache_size: 64,
        global_variable_cache_property_offset: 4,
        global_variable_cache_dictionary_generation: 8,
        global_variable_cache_writes_data_property: 13,
        global_variable_cache_shape: 24,
        global_variable_cache_environment_serial: 48,
        global_variable_cache_environment_binding_index: 56,
        global_variable_cache_has_environment_binding: 60,
        property_iterator_fast_path: 8,
        property_iterator_shape_is_dictionary: 9,
        property_iterator_shape_dictionary_generation: 12,
        property_iterator_shape: 16,
        property_iterator_indexed_property_count: 24,
        property_iterator_prototype_chain_validity: 32,
        property_iterator_property_values: 40,
        property_iterator_property_value_count: 48,
        property_iterator_fast_path_none: 0,
        property_iterator_fast_path_packed_indexed: 2,
        executable_property_lookup_caches: 24,
        property_lookup_cache_data_pointer_mask: !3,
        property_lookup_cache_entry_type: 0,
        property_lookup_cache_entry_property_offset: 4,
        property_lookup_cache_entry_dictionary_generation: 8,
        property_lookup_cache_entry_writes_data_property: 13,
        property_lookup_cache_entry_shape: 24,
        property_lookup_cache_entry_prototype: 32,
        property_lookup_cache_entry_prototype_chain_validity: 40,
        property_lookup_cache_entry_type_get_missing_property: 6,
        property_lookup_cache_entry_type_add_own_property: 1,
        property_lookup_cache_entry_type_get_own_property: 3,
        property_lookup_cache_entry_type_change_own_property: 2,
        class_object_methods: 8,
        object_methods_get_prototype_of: 0,
        // NB: Tests that look in the VM's keyed lookup cache set its entries.
        keyed_lookup_cache_entries: 0,
        keyed_store_cache_entries: 0,
        keyed_lookup_cache_index_bits: 11,
        keyed_lookup_cache_entry_size: 64,
        keyed_lookup_cache_entry_type: 0,
        keyed_lookup_cache_property_offset: 4,
        keyed_lookup_cache_dictionary_generation: 8,
        keyed_lookup_cache_shape: 16,
        keyed_lookup_cache_name: 40,
        property_lookup_cache_polymorphic_tag: 1,
        property_lookup_cache_polymorphic_entry_count: 4,
        property_lookup_cache_entry_size: 64,
        property_lookup_cache_entry_key: 48,
        property_lookup_cache_keyed_generic: 3,
        property_lookup_cache_entry_from_shape: 16,
        property_lookup_cache_megamorphic_tag: 2,
        property_lookup_cache_megamorphic_primary_entries: 64,
        property_lookup_cache_megamorphic_secondary_entries: 64 + 64 * 64,
        property_lookup_cache_megamorphic_index_bits: 6,
        property_lookup_cache_megamorphic_hash_multiplier: 0x9e37_79b9,
        try_get_by_id_cache: fake_try_get_by_id_cache as *const () as u64,
        try_put_by_id_cache: fake_try_put_by_id_cache as *const () as u64,
    }
}

/// An array with `elements` in a buffer of `capacity` elements (at least
/// as many), whose array-like size is `size`.
struct FakeArray {
    object: Vec<u64>,
    buffer: Vec<u64>,
}

impl FakeArray {
    fn new(flags: u16, kind: u8, size: u32, elements: &[u64], capacity: usize) -> Self {
        let mut buffer = vec![capacity as u64];
        buffer.extend(elements);
        buffer.resize(capacity + 1, value::EMPTY);
        let mut array = Self {
            object: vec![0; 8],
            buffer,
        };
        array.object[1] = u64::from(flags);
        array.object[2] = array.buffer.as_ptr().wrapping_add(1) as u64;
        array.object[3] = u64::from(kind) | u64::from(size) << 32;
        array
    }

    fn value(&self) -> u64 {
        u64::from(value::OBJECT_TAG) << 48 | self.object.as_ptr() as u64
    }
}

/// A typed array of `kind` over `data`.
fn fake_typed_array(kind: u8, data: &[u8], length: u32) -> Vec<u64> {
    let mut object = vec![0; 8];
    object[1] = u64::from(FLAG_IS_TYPED_ARRAY);
    object[4] = data.as_ptr() as u64;
    object[5] = u64::from(length) | u64::from(kind) << 32;
    object
}

fn object_value(object: &[u64]) -> u64 {
    u64::from(value::OBJECT_TAG) << 48 | object.as_ptr() as u64
}

fn get_by_value(base: u64, index: u64) -> u64 {
    run_instruction(
        Instruction::GetByValue {
            value_feedback: 0,
            keyed_feedback: 0,
            dst: r(5),
            base: argument(0),
            property: argument(1),
            base_identifier: None,
            cache: 0,
        },
        &[base, index],
    )
}

/// Runs a PutByValue, and returns how often it took the slow path, or
/// `u64::MAX` if it exited.
fn put_by_value(base: u64, index: u64, src: u64) -> u64 {
    let program = assemble(|_| {
        vec![
            Instruction::Enter,
            Instruction::PutByValue {
                keyed_feedback: 0,
                base: argument(0),
                property: argument(1),
                src: argument(2),
                kind: 0,
                base_identifier: None,
                cache: 0,
            },
            Instruction::Return { value: c(0) },
        ]
    });
    let (result, machine) = run_program(&program, &[base, index, src], |_| {});
    if result.status == crate::code::JitStatus::Resume as u64 {
        return u64::MAX;
    }
    assert_eq!(result.status, RETURNED);
    machine.vm[VM_SLOW_PUTS_WORD]
}

#[test]
fn get_by_value_on_arrays() {
    let elements = [value::int32(10), value::int32(11), value::EMPTY, value::int32(13)];
    let packed = FakeArray::new(FLAG_HAS_MAGICAL_LENGTH, PACKED, 4, &elements, 4);
    for (index, expected) in [
        (0, value::int32(10)),
        (3, value::int32(13)),
        // NB: Returning the empty value returns undefined.
        (2, value::UNDEFINED),
        (4, MARKER),
        (-1, MARKER),
    ] {
        assert_eq!(
            get_by_value(packed.value(), value::int32(index)),
            expected,
            "packed [{index}]"
        );
    }
    assert_eq!(get_by_value(packed.value(), double(1.0)), MARKER);
    assert_eq!(get_by_value(packed.value(), value::UNDEFINED), MARKER);
    assert_eq!(get_by_value(value::int32(1), value::int32(0)), MARKER);
    let interfering = FakeArray::new(FLAG_MAY_INTERFERE, PACKED, 4, &elements, 4);
    assert_eq!(get_by_value(interfering.value(), value::int32(0)), MARKER);
    // The capacity bounds the access even if the size claims more.
    let short = FakeArray::new(0, PACKED, 8, &elements, 4);
    assert_eq!(get_by_value(short.value(), value::int32(5)), MARKER);

    let holey = FakeArray::new(0, HOLEY, 4, &elements, 4);
    assert_eq!(get_by_value(holey.value(), value::int32(1)), value::int32(11));
    assert_eq!(get_by_value(holey.value(), value::int32(2)), MARKER);
    let mut without_buffer = FakeArray::new(0, HOLEY, 4, &elements, 4);
    without_buffer.object[2] = 0;
    assert_eq!(get_by_value(without_buffer.value(), value::int32(1)), MARKER);
    let dictionary = FakeArray::new(0, 3, 4, &elements, 4);
    assert_eq!(get_by_value(dictionary.value(), value::int32(1)), MARKER);
}

#[test]
fn get_by_value_on_typed_arrays() {
    let bytes: Vec<u8> = (0..32u8).map(|byte| byte.wrapping_mul(37).wrapping_add(200)).collect();
    let word = |offset: usize, size: usize| {
        let mut buffer = [0u8; 8];
        buffer[..size].copy_from_slice(&bytes[offset..offset + size]);
        u64::from_le_bytes(buffer)
    };
    for kind in 0..=8u8 {
        let array = fake_typed_array(kind, &bytes, 3);
        for index in [0usize, 2] {
            let expected = match kind {
                0 | 1 => value::int32(i32::from(bytes[index])),
                2 => value::int32(i32::from(word(2 * index, 2) as u16)),
                3 => {
                    let element = word(4 * index, 4) as u32;
                    match i32::try_from(element) {
                        Ok(element) => value::int32(element),
                        Err(_) => f64::from(element).to_bits(),
                    }
                }
                4 => value::int32(i32::from(bytes[index] as i8)),
                5 => value::int32(i32::from(word(2 * index, 2) as u16 as i16)),
                6 => value::int32(word(4 * index, 4) as u32 as i32),
                7 => box_number(f64::from(f32::from_bits(word(4 * index, 4) as u32))),
                _ => box_number(f64::from_bits(word(8 * index, 8))),
            };
            assert_eq!(
                get_by_value(object_value(&array), value::int32(index as i32)),
                expected,
                "kind {kind} [{index}]"
            );
        }
        assert_eq!(
            get_by_value(object_value(&array), value::int32(3)),
            MARKER,
            "kind {kind} [3]"
        );
    }
    // Float64 elements box like numbers.
    let doubles = [3.0f64, -0.0, f64::NAN, 0.25];
    let data = doubles
        .iter()
        .flat_map(|number| number.to_le_bytes())
        .collect::<Vec<_>>();
    let array = fake_typed_array(8, &data, 4);
    let results = (0..4)
        .map(|index| get_by_value(object_value(&array), value::int32(index)))
        .collect::<Vec<_>>();
    assert_eq!(
        results,
        [value::int32(3), NEGATIVE_ZERO, CANONICAL_NAN, 0.25f64.to_bits()]
    );
    let mut detached = fake_typed_array(0, &data, 4);
    detached[4] = u64::MAX;
    assert_eq!(get_by_value(object_value(&detached), value::int32(0)), MARKER);
}

#[test]
fn put_by_value_on_arrays() {
    let elements = [value::int32(10), value::EMPTY, value::int32(12)];
    let packed = FakeArray::new(0, PACKED, 3, &elements, 3);
    assert_eq!(put_by_value(packed.value(), value::int32(2), value::TRUE), 0);
    assert_eq!(packed.buffer[3], value::TRUE);
    // Appends exit for arrays that push could not append to, like this one,
    // which is not extensible.
    assert_eq!(put_by_value(packed.value(), value::int32(3), value::TRUE), u64::MAX);
    assert_eq!(put_by_value(packed.value(), value::int32(4), value::TRUE), 1);
    assert_eq!(put_by_value(packed.value(), value::UNDEFINED, value::TRUE), 1);

    let holey = FakeArray::new(0, HOLEY, 3, &elements, 3);
    assert_eq!(put_by_value(holey.value(), value::int32(0), value::NULL), 0);
    assert_eq!(holey.buffer[1], value::NULL);
    // Holes are filled by the slow path.
    assert_eq!(put_by_value(holey.value(), value::int32(1), value::NULL), 1);
    assert_eq!(holey.buffer[2], value::EMPTY);
    let interfering = FakeArray::new(FLAG_MAY_INTERFERE, PACKED, 3, &elements, 3);
    assert_eq!(put_by_value(interfering.value(), value::int32(0), value::NULL), 1);
}

#[test]
fn put_by_value_on_typed_arrays() {
    let mut data = vec![0u8; 32];
    for (kind, src, expected) in [
        (0u8, value::int32(300), vec![44u8]),
        (4, value::int32(-1), vec![255]),
        (1, value::int32(300), vec![255]),
        (1, value::int32(-5), vec![0]),
        (1, value::int32(77), vec![77]),
        (2, value::int32(0x12345), vec![0x45, 0x23]),
        (6, value::int32(-2), vec![0xfe, 0xff, 0xff, 0xff]),
        (8, value::int32(2), 2.0f64.to_le_bytes().to_vec()),
        (8, double(0.5), 0.5f64.to_le_bytes().to_vec()),
        (7, value::int32(3), 3.0f32.to_le_bytes().to_vec()),
        (7, double(0.1), 0.1f32.to_le_bytes().to_vec()),
        (7, CANONICAL_NAN, f32::NAN.to_le_bytes().to_vec()),
        (8, CANONICAL_NAN, CANONICAL_NAN.to_le_bytes().to_vec()),
    ] {
        data.fill(0xAA);
        let array = fake_typed_array(kind, &data, 2);
        assert_eq!(
            put_by_value(object_value(&array), value::int32(1), src),
            0,
            "kind {kind} {src:#x}"
        );
        let size = expected.len();
        assert_eq!(&data[size..2 * size], expected.as_slice(), "kind {kind} {src:#x}");
        assert!(data[..size].iter().all(|byte| *byte == 0xAA), "kind {kind} {src:#x}");
    }
    // Integer arrays only take int32 values inline, and float arrays
    // numbers.
    for (kind, src) in [
        (0u8, double(1.9)),
        (0, value::TRUE),
        (5, double(-2.5)),
        (3, double(4_294_967_297.0)),
        (1, double(1.5)),
        (7, value::UNDEFINED),
        (0, value::UNDEFINED),
        (6, double(1e300)),
    ] {
        data.fill(0xAA);
        let array = fake_typed_array(kind, &data, 2);
        assert_eq!(
            put_by_value(object_value(&array), value::int32(1), src),
            1,
            "kind {kind} {src:#x}"
        );
        assert!(data.iter().all(|byte| *byte == 0xAA), "kind {kind} {src:#x}");
    }
    let array = fake_typed_array(0, &data, 2);
    assert_eq!(put_by_value(object_value(&array), value::int32(2), value::int32(1)), 1);
}

#[test]
fn get_length() {
    let get_length = |base: u64| {
        run_instruction(
            Instruction::GetLength {
                value_feedback: 0,
                dst: r(5),
                base: argument(0),
                base_identifier: None,
                cache: 0,
            },
            &[base],
        )
    };
    let array = FakeArray::new(FLAG_HAS_MAGICAL_LENGTH, PACKED, 7, &[], 0);
    assert_eq!(get_length(array.value()), value::int32(7));
    let huge = FakeArray::new(FLAG_HAS_MAGICAL_LENGTH, HOLEY, 0x8000_0001, &[], 0);
    // NB: Lengths that are no int32 take the slow path.
    assert_eq!(get_length(huge.value()), MARKER);
    let plain = FakeArray::new(0, PACKED, 7, &[], 0);
    assert_eq!(get_length(plain.value()), MARKER);
    let string = [0u64, 0, 5, 0];
    assert_eq!(
        get_length(u64::from(STRING_TAG) << 48 | string.as_ptr() as u64),
        value::int32(5)
    );
    let huge_string = [0u64, 0, 0x8000_0000, 0];
    assert_eq!(
        get_length(u64::from(STRING_TAG) << 48 | huge_string.as_ptr() as u64),
        MARKER
    );
    assert_eq!(get_length(value::int32(1)), MARKER);
}

// Objects also have their shape at word 6 and their named properties at
// word 7. Key snapshots of for-in loops have the fast path kind (byte 8),
// whether the shape is a dictionary (byte 9) and its dictionary generation
// (bytes 12-15) in word 1, the shape at word 2, the indexed property count
// at word 3, the prototype chain validity at word 4, and the keys and their
// count at words 5 and 6. Shapes have their dictionary generation at byte 84.

/// A for-in key snapshot of `keys` for objects with `shape`.
struct FakeKeySnapshot {
    words: Vec<u64>,
    keys: Vec<u64>,
}

impl FakeKeySnapshot {
    fn new(fast_path: u8, shape: &[u64], keys: &[u64]) -> Box<Self> {
        let mut snapshot = Box::new(Self {
            words: vec![0; 8],
            keys: keys.to_vec(),
        });
        snapshot.words[1] = u64::from(fast_path);
        snapshot.words[2] = shape.as_ptr() as u64;
        snapshot.words[5] = snapshot.keys.as_ptr() as u64;
        snapshot.words[6] = keys.len() as u64;
        snapshot
    }

    fn value(&self) -> u64 {
        0xFFF8 << 48 | self.words.as_ptr() as u64
    }
}

/// Runs ObjectPropertyIteratorNext with `receiver`, `keys` and `cursor`;
/// returns the value, done flag and cursor it wrote, and how often the slow
/// path ran.
fn property_iterator_next(receiver: u64, keys: u64, cursor: u64) -> (u64, u64, u64, u64) {
    let run = |written: Operand| {
        let program = assemble(|_| {
            vec![
                Instruction::Enter,
                Instruction::ObjectPropertyIteratorNext {
                    dst_value: r(5),
                    dst_done: r(6),
                    receiver: argument(0),
                    keys: argument(1),
                    cursor: argument(2),
                },
                // NB: Unlike Return, End returns the empty value as is.
                Instruction::End { value: written },
            ]
        });
        let (result, machine) = run_program(&program, &[receiver, keys, cursor], |_| {});
        assert_eq!(result.status, RETURNED);
        (result.value, machine.vm[VM_SLOW_PUTS_WORD])
    };
    let (value, slow_paths) = run(r(5));
    (value, run(r(6)).0, run(argument(2)).0, slow_paths)
}

#[test]
fn property_iterator_next_reads_key_snapshots() {
    let shape = vec![0u64; 12];
    let mut object = FakeArray::new(0, PACKED, 2, &[], 2);
    object.object[6] = shape.as_ptr() as u64;
    let keys = [value::int32(100), value::int32(101)];
    let named = FakeKeySnapshot::new(1, &shape, &keys);
    let empty = value::EMPTY;

    // Keys while there are some, then done.
    assert_eq!(
        property_iterator_next(object.value(), named.value(), value::int32(0)),
        (keys[0], value::FALSE, value::int32(1), 0)
    );
    assert_eq!(
        property_iterator_next(object.value(), named.value(), value::int32(1)),
        (keys[1], value::FALSE, value::int32(2), 0)
    );
    assert_eq!(
        property_iterator_next(object.value(), named.value(), value::int32(2)),
        (empty, value::TRUE, value::int32(2), 0)
    );
    // Other shapes, cursors and receivers are slow.
    let other_shape = vec![0u64; 12];
    let other = FakeKeySnapshot::new(1, &other_shape, &keys);
    assert_eq!(
        property_iterator_next(object.value(), other.value(), value::int32(0)).3,
        1
    );
    assert_eq!(
        property_iterator_next(object.value(), named.value(), value::int32(-1)).3,
        1
    );
    assert_eq!(property_iterator_next(object.value(), named.value(), double(1.0)).3, 1);
    assert_eq!(
        property_iterator_next(value::int32(1), named.value(), value::int32(0)).3,
        1
    );
    let none = FakeKeySnapshot::new(0, &shape, &keys);
    assert_eq!(
        property_iterator_next(object.value(), none.value(), value::int32(0)).3,
        1
    );

    // An invalidated prototype chain is slow.
    let mut validity = [1u64];
    let mut checked = FakeKeySnapshot::new(1, &shape, &keys);
    checked.words[4] = validity.as_ptr() as u64;
    assert_eq!(
        property_iterator_next(object.value(), checked.value(), value::int32(0)).3,
        0
    );
    validity[0] = 0;
    std::hint::black_box(&validity);
    assert_eq!(
        property_iterator_next(object.value(), checked.value(), value::int32(0)).3,
        1
    );

    // Packed indexed snapshots need as many packed elements as they saw.
    let mut indexed = FakeKeySnapshot::new(2, &shape, &keys);
    indexed.words[3] = 2;
    assert_eq!(
        property_iterator_next(object.value(), indexed.value(), value::int32(0)).3,
        0
    );
    indexed.words[3] = 3;
    assert_eq!(
        property_iterator_next(object.value(), indexed.value(), value::int32(0)).3,
        1
    );
}

// Environments, laid out like this, in words: 2 the outer environment, 3
// the shape, 6 the binding values, 7 the rare data, 8 the serial number.
// Rare data has its binding flags at word 5. Environment shapes have their
// flag count at word 5 and their flags at word 7. Realms have the global
// object at word 3 and the global declarative environment at word 4. The
// frame has the variable environment at word 12 and the realm at word 9.

const BINDING_MUTABLE: u8 = 2;

struct FakeEnvironment {
    words: Vec<u64>,
    values: Vec<u64>,
    rare_data: Vec<u64>,
    rare_flags: Vec<u8>,
    shape: Vec<u64>,
    shape_flags: Vec<u8>,
}

impl FakeEnvironment {
    /// An environment with `values`, whose first `shape_flags` flags are in
    /// a shape (if any) and whose other flags are in its rare data.
    fn new(values: &[u64], flags: &[u8], shape_flags: Option<usize>, outer: Option<&FakeEnvironment>) -> Box<Self> {
        let in_shape = shape_flags.unwrap_or(0);
        let mut environment = Box::new(Self {
            words: vec![0; 10],
            values: values.to_vec(),
            rare_data: vec![0; 8],
            rare_flags: flags[in_shape..].to_vec(),
            shape: vec![0; 8],
            shape_flags: flags[..in_shape].to_vec(),
        });
        environment.rare_data[5] = environment.rare_flags.as_ptr() as u64;
        environment.shape[5] = in_shape as u64;
        environment.shape[7] = environment.shape_flags.as_ptr() as u64;
        environment.words[2] = outer.map_or(0, |outer| outer.words.as_ptr() as u64);
        if shape_flags.is_some() {
            environment.words[3] = environment.shape.as_ptr() as u64;
        }
        environment.words[6] = environment.values.as_ptr() as u64;
        environment.words[7] = environment.rare_data.as_ptr() as u64;
        environment
    }

    fn address(&self) -> u64 {
        self.words.as_ptr() as u64
    }
}

/// Runs `instructions` with the lexical and variable environments set, and
/// returns the result and how often a slow path that counts ran.
fn run_with_environments(
    instructions: Vec<Instruction>,
    arguments: &[u64],
    lexical: &FakeEnvironment,
    variable: &FakeEnvironment,
) -> (JitResult, u64) {
    let program = assemble(|_| instructions.clone());
    let (result, machine) = run_program(&program, arguments, |machine| {
        machine.frame[1] = lexical.address();
        machine.frame[12] = variable.address();
    });
    (result, machine.vm[VM_SLOW_PUTS_WORD])
}

fn coordinate(hops: u32, index: u32) -> EnvironmentCoordinate {
    EnvironmentCoordinate { hops, index }
}

#[test]
fn bindings() {
    let flags = [BINDING_MUTABLE, 0, BINDING_MUTABLE, BINDING_MUTABLE];
    let outer = FakeEnvironment::new(
        &[value::int32(1), value::EMPTY, value::int32(3), value::int32(4)],
        &flags,
        Some(1),
        None,
    );
    let middle = FakeEnvironment::new(&[value::int32(5)], &[0], None, Some(&outer));
    let inner = FakeEnvironment::new(&[value::int32(9)], &[BINDING_MUTABLE], Some(1), Some(&middle));
    let get = |hops, index, initialized| {
        let get = if initialized {
            Instruction::GetInitializedBinding {
                value_feedback: 0,
                dst: r(5),
                identifier: crate::bytecode::IdentifierTableIndex(0),
                cache: coordinate(hops, index),
            }
        } else {
            Instruction::GetBinding {
                value_feedback: 0,
                dst: r(5),
                identifier: crate::bytecode::IdentifierTableIndex(0),
                cache: coordinate(hops, index),
            }
        };
        let (result, _) = run_with_environments(
            vec![Instruction::Enter, get, Instruction::Return { value: r(5) }],
            &[],
            &inner,
            &outer,
        );
        result
    };
    assert_eq!(get(0, 0, false), returned(value::int32(9)));
    assert_eq!(get(1, 0, true), returned(value::int32(5)));
    assert_eq!(get(2, 3, false), returned(value::int32(4)));
    assert_eq!(
        get(2, 1, false).status,
        EXIT_INTERPRETER,
        "uninitialized bindings throw"
    );
    // Initialized bindings are never checked; the returned empty value becomes undefined.
    assert_eq!(get(2, 1, true), returned(value::UNDEFINED));

    // The callee and an undefined `this`.
    let callee_and_this = |hops, index, returned_operand| {
        run_with_environments(
            vec![
                Instruction::Enter,
                Instruction::GetCalleeAndThisFromEnvironment {
                    value_feedback: 0,
                    callee: r(5),
                    this_value: r(6),
                    identifier: crate::bytecode::IdentifierTableIndex(0),
                    cache: coordinate(hops, index),
                },
                Instruction::Return {
                    value: returned_operand,
                },
            ],
            &[],
            &inner,
            &outer,
        )
        .0
    };
    assert_eq!(callee_and_this(2, 2, r(5)), returned(value::int32(3)));
    assert_eq!(callee_and_this(2, 2, r(6)), returned(value::UNDEFINED));
    assert_eq!(callee_and_this(2, 1, r(5)).status, EXIT_INTERPRETER);

    // Sets of initialized mutable bindings, with their flags in the shape,
    // after the shape's in the rare data, or in rare data only.
    let set = |environment_kind: crate::ir::FrameField, hops, index| {
        let set = match environment_kind {
            crate::ir::FrameField::LexicalEnvironment => Instruction::SetLexicalBinding {
                identifier: crate::bytecode::IdentifierTableIndex(0),
                src: argument(0),
                cache: coordinate(hops, index),
            },
            _ => Instruction::SetVariableBinding {
                identifier: crate::bytecode::IdentifierTableIndex(0),
                src: argument(0),
                cache: coordinate(hops, index),
            },
        };
        run_with_environments(
            vec![Instruction::Enter, set, Instruction::Return { value: c(0) }],
            &[value::int32(77)],
            &inner,
            &outer,
        )
        .1
    };
    assert_eq!(set(crate::ir::FrameField::LexicalEnvironment, 0, 0), 0);
    assert_eq!(inner.values[0], value::int32(77));
    assert_eq!(set(crate::ir::FrameField::VariableEnvironment, 0, 3), 0);
    assert_eq!(outer.values[3], value::int32(77));
    assert_eq!(set(crate::ir::FrameField::LexicalEnvironment, 2, 0), 0);
    assert_eq!(outer.values[0], value::int32(77));
    // Uninitialized and immutable bindings are slow.
    assert_eq!(set(crate::ir::FrameField::VariableEnvironment, 0, 1), 1);
    assert_eq!(outer.values[1], value::EMPTY);
    assert_eq!(set(crate::ir::FrameField::LexicalEnvironment, 1, 0), 1);
    assert_eq!(middle.values[0], value::int32(5));

    // Initialization stores unconditionally.
    let (result, _) = run_with_environments(
        vec![
            Instruction::Enter,
            Instruction::InitializeVariableBinding {
                identifier: crate::bytecode::IdentifierTableIndex(0),
                src: argument(0),
                cache: coordinate(0, 1),
            },
            Instruction::InitializeLexicalBinding {
                identifier: crate::bytecode::IdentifierTableIndex(0),
                src: argument(1),
                cache: coordinate(1, 0),
            },
            Instruction::Return { value: c(0) },
        ],
        &[value::int32(8), value::TRUE],
        &inner,
        &outer,
    );
    assert_eq!(result, returned(value::int32(0)));
    assert_eq!(outer.values[1], value::int32(8));
    assert_eq!(middle.values[0], value::TRUE);
}

/// Word 0 of fake module environments, like the class of real ones.
const MODULE_ENVIRONMENT_CLASS: u64 = 0x7100;

#[test]
fn create_variable() {
    // The identifiers, which the environment's shape names its two
    // bindings, a mutable one and a strict immutable one.
    const NAMES: [u64; 2] = [0x100, 0x200];
    let flags = [BINDING_MUTABLE, 1];
    let create = |environment: &FakeEnvironment, identifier: u32, mode: u32, is_immutable: bool, is_strict: bool| {
        let program = assemble(|_| {
            vec![
                Instruction::Enter,
                Instruction::CreateVariable {
                    identifier: crate::bytecode::IdentifierTableIndex(identifier),
                    mode,
                    is_immutable,
                    is_global: false,
                    is_strict,
                },
                Instruction::Return { value: c(0) },
            ]
        });
        let (result, machine) = run_program_with_snapshot(
            &program,
            &[],
            |snapshot| snapshot.executables[0].identifiers = NAMES.to_vec(),
            |machine| {
                machine.frame[1] = environment.address();
                machine.frame[12] = environment.address();
            },
        );
        assert_eq!(result.status, RETURNED);
        machine.vm[VM_SLOW_PUTS_WORD]
    };
    // A declarative environment with room for `capacity` values, `size` of
    // them bindings already.
    let environment = |size: u64, capacity: usize| {
        let mut environment = FakeEnvironment::new(&vec![value::int32(7); capacity], &flags, Some(2), None);
        environment.words[1] = 1 << 40;
        environment.words[4] = size;
        environment.words[5] = capacity as u64;
        environment.shape[4] = 1;
        environment.shape[6] = NAMES.as_ptr() as u64;
        environment
    };

    // The bindings of the shape, in order.
    let lexical = environment(0, 2);
    assert_eq!(create(&lexical, 0, 0, false, false), 0);
    assert_eq!((lexical.words[4], lexical.values[0]), (1, value::EMPTY));
    assert_eq!(create(&lexical, 1, 1, true, true), 0);
    assert_eq!((lexical.words[4], lexical.values[1]), (2, value::EMPTY));
    // No more bindings in the shape.
    assert_eq!(create(&lexical, 1, 1, true, true), 1);

    // Other names, other flags, names the shape has twice, no room for the
    // value, module environments and environments that are not declarative
    // take the slow path.
    let slow = |environment: Box<FakeEnvironment>, identifier, is_immutable, is_strict| {
        let count = create(&environment, identifier, 0, is_immutable, is_strict);
        assert_eq!((count, environment.words[4]), (1, environment.words[4]));
        environment.words[4]
    };
    assert_eq!(slow(environment(0, 2), 1, true, true), 0);
    assert_eq!(slow(environment(0, 2), 0, true, false), 0);
    assert_eq!(slow(environment(0, 2), 0, false, true), 0);
    let mut duplicates = environment(0, 2);
    duplicates.shape[4] = 0;
    assert_eq!(slow(duplicates, 0, false, false), 0);
    assert_eq!(slow(environment(1, 1), 1, true, true), 1);
    let mut module = environment(0, 2);
    module.words[0] = MODULE_ENVIRONMENT_CLASS;
    assert_eq!(slow(module, 0, false, false), 0);
    let mut object_environment = environment(0, 2);
    object_environment.words[1] = 0;
    assert_eq!(slow(object_environment, 0, false, false), 0);
}

/// A realm with a global object and a global declarative environment, and
/// an executable with one global variable cache.
struct FakeGlobals {
    realm: Vec<u64>,
    object: Vec<u64>,
    named_properties: Vec<u64>,
    shape: Vec<u64>,
    environment: Box<FakeEnvironment>,
    executable: Vec<u64>,
    caches: Vec<u64>,
}

impl FakeGlobals {
    fn new() -> Box<Self> {
        let mut globals = Box::new(Self {
            realm: vec![0; 8],
            object: vec![0; 8],
            named_properties: vec![value::int32(10), u64::from(0xFFFC_u16) << 48 | 0x1000, value::int32(12)],
            shape: vec![0; 12],
            environment: FakeEnvironment::new(
                &[value::int32(20), value::EMPTY, value::int32(22)],
                &[BINDING_MUTABLE, BINDING_MUTABLE, 0],
                None,
                None,
            ),
            executable: vec![0; 4],
            caches: vec![0; 16],
        });
        globals.object[6] = globals.shape.as_ptr() as u64;
        globals.object[7] = globals.named_properties.as_ptr() as u64;
        globals.shape[10] = 7 << 32;
        globals.environment.words[8] = 99;
        globals.realm[3] = globals.object.as_ptr() as u64;
        globals.realm[4] = globals.environment.address();
        globals.executable[1] = globals.caches.as_ptr() as u64;
        globals
    }

    /// Points cache 1 at named property `offset` of the global object, or
    /// at global binding `binding` if `shape_matches` is false.
    fn set_cache(&mut self, shape_matches: bool, offset: u32, binding: Option<u32>, writes: bool) {
        let cache = &mut self.caches[8..16];
        cache.fill(0);
        cache[0] = u64::from(offset) << 32;
        cache[1] = 7 | u64::from(writes) << 40;
        cache[3] = if shape_matches { self.shape.as_ptr() as u64 } else { 1 };
        cache[6] = 99;
        if let Some(binding) = binding {
            cache[7] = u64::from(binding) | 1 << 32;
        }
    }

    fn run(&self, instruction: Instruction, arguments: &[u64]) -> (JitResult, u64) {
        let program = assemble(|_| {
            vec![
                Instruction::Enter,
                instruction.clone(),
                Instruction::Return { value: r(5) },
            ]
        });
        let (result, machine) = run_program(&program, arguments, |machine| {
            machine.frame[4] = self.executable.as_ptr() as u64;
            machine.frame[9] = self.realm.as_ptr() as u64;
        });
        (result, machine.vm[VM_SLOW_PUTS_WORD])
    }

    fn get(&self) -> u64 {
        let (result, _) = self.run(
            Instruction::GetGlobal {
                value_feedback: 0,
                dst: r(5),
                identifier: crate::bytecode::IdentifierTableIndex(0),
                cache: 1,
            },
            &[],
        );
        assert_eq!(result.status, RETURNED);
        result.value
    }

    /// Sets the global to `value`; returns how often the slow path ran.
    fn set(&self, value: u64) -> u64 {
        let (result, slow_paths) = self.run(
            Instruction::SetGlobal {
                identifier: crate::bytecode::IdentifierTableIndex(0),
                src: argument(0),
                cache: 1,
            },
            &[value],
        );
        assert_eq!(result.status, RETURNED);
        slow_paths
    }
}

#[test]
fn global_variables() {
    let mut globals = FakeGlobals::new();
    globals.set_cache(true, 2, None, true);
    assert_eq!(globals.get(), value::int32(12));
    assert_eq!(globals.set(value::TRUE), 0);
    assert_eq!(globals.named_properties[2], value::TRUE);
    // Accessors are slow.
    globals.set_cache(true, 1, None, true);
    assert_eq!(globals.get(), MARKER);
    assert_eq!(globals.set(value::TRUE), 1);
    // So are entries that do not write data properties.
    globals.set_cache(true, 0, None, false);
    assert_eq!(globals.get(), value::int32(10));
    assert_eq!(globals.set(value::TRUE), 1);
    assert_eq!(globals.named_properties[0], value::int32(10));
    // A different dictionary generation misses like a different shape.
    globals.set_cache(true, 0, Some(2), true);
    globals.shape[10] = 8 << 32;
    assert_eq!(globals.get(), value::int32(22));
    globals.shape[10] = 7 << 32;

    // Global declarative environment bindings.
    globals.set_cache(false, 0, Some(0), true);
    assert_eq!(globals.get(), value::int32(20));
    assert_eq!(globals.set(value::NULL), 0);
    assert_eq!(globals.environment.values[0], value::NULL);
    globals.set_cache(false, 0, Some(1), true);
    assert_eq!(globals.get(), MARKER);
    assert_eq!(globals.set(value::NULL), 1);
    globals.set_cache(false, 0, Some(2), true);
    assert_eq!(globals.set(value::NULL), 1, "immutable");
    assert_eq!(globals.environment.values[2], value::int32(22));
    globals.set_cache(false, 0, None, true);
    assert_eq!(globals.get(), MARKER);

    // Caches for another global declarative environment are slow.
    globals.set_cache(true, 2, None, true);
    globals.caches[14] = 98;
    assert_eq!(globals.get(), MARKER);
    assert_eq!(globals.set(value::NULL), 1);
}

/// Runs `instruction` with the global variable cache 1 the snapshot has
/// set to `cache`, for the realm of `globals` whose global declarative
/// environment has the serial number `serial`.
fn run_with_global_cache(
    globals: &FakeGlobals,
    instruction: &Instruction,
    arguments: &[u64],
    cache: crate::snapshot::GlobalCacheSnapshot,
    serial: u64,
) -> JitResult {
    let program = assemble(|_| {
        vec![
            Instruction::Enter,
            instruction.clone(),
            Instruction::Return { value: r(5) },
        ]
    });
    let snapshot_globals = crate::snapshot::GlobalsSnapshot {
        object: crate::snapshot::CellId(globals.object.as_ptr() as u64),
        declarative_environment: crate::snapshot::CellId(globals.environment.address()),
        environment_serial: serial,
        caches: vec![None, Some(cache)],
    };
    // NB: The code accesses the global variables directly, without the
    //     realm or the executable's caches.
    let (result, _) = run_program_with_snapshot(
        &program,
        arguments,
        |snapshot| snapshot.executables[0].globals = Some(snapshot_globals),
        |_| {},
    );
    result
}

#[test]
fn global_variables_the_snapshot_knows() {
    use crate::snapshot::CellId;
    use crate::snapshot::GlobalCacheSnapshot;
    use crate::snapshot::GlobalValueSnapshot;
    const EXITED: u64 = crate::code::JitStatus::Resume as u64;
    let mut globals = FakeGlobals::new();
    let get = Instruction::GetGlobal {
        value_feedback: 0,
        dst: r(5),
        identifier: crate::bytecode::IdentifierTableIndex(0),
        cache: 1,
    };
    let set = Instruction::SetGlobal {
        identifier: crate::bytecode::IdentifierTableIndex(0),
        src: argument(0),
        cache: 1,
    };
    let value_of = |bits: u64| GlobalValueSnapshot {
        bits,
        cell: None,
        intrinsic: None,
        has_instance: None,
    };
    let binding = |index: u32, mutable: bool, value: Option<u64>| GlobalCacheSnapshot::Binding {
        index,
        mutable,
        assigned: true,
        value: value.map(value_of),
    };
    let property = |shape: u64, offset: u32, value: u64| GlobalCacheSnapshot::Property {
        shape: CellId(shape),
        dictionary_generation: None,
        offset,
        writes_data_property: true,
        assigned: true,
        value: value_of(value),
    };

    // Initialized immutable bindings are constants, and so are mutable ones
    // that were never assigned.
    let result = run_with_global_cache(&globals, &get, &[], binding(2, false, Some(value::int32(42))), 99);
    assert_eq!((result.status, result.value), (RETURNED, value::int32(42)));
    let unassigned = GlobalCacheSnapshot::Binding {
        index: 0,
        mutable: true,
        assigned: false,
        value: Some(value_of(value::int32(43))),
    };
    let result = run_with_global_cache(&globals, &get, &[], unassigned, 99);
    assert_eq!((result.status, result.value), (RETURNED, value::int32(43)));

    // Mutable bindings are read and written where they are, unless they are
    // still uninitialized.
    let result = run_with_global_cache(&globals, &get, &[], binding(0, true, Some(value::int32(20))), 99);
    assert_eq!((result.status, result.value), (RETURNED, value::int32(20)));
    let result = run_with_global_cache(&globals, &set, &[value::TRUE], binding(0, true, None), 99);
    assert_eq!(result.status, RETURNED);
    assert_eq!(globals.environment.values[0], value::TRUE);
    let result = run_with_global_cache(&globals, &get, &[], binding(1, true, None), 99);
    assert_eq!(result.status, EXITED);
    let result = run_with_global_cache(&globals, &set, &[value::TRUE], binding(1, true, None), 99);
    assert_eq!(result.status, EXITED);
    assert_eq!(globals.environment.values[1], value::EMPTY);

    // Properties of the global object need its shape. (The code depends on
    // the global declarative environment getting no bindings.)
    let shape = globals.shape.as_ptr() as u64;
    let result = run_with_global_cache(&globals, &get, &[], property(shape, 2, value::int32(12)), 99);
    assert_eq!((result.status, result.value), (RETURNED, value::int32(12)));
    let result = run_with_global_cache(&globals, &get, &[], property(shape + 8, 2, value::int32(12)), 99);
    assert_eq!(result.status, EXITED);
    let result = run_with_global_cache(&globals, &set, &[value::NULL], property(shape, 2, value::int32(12)), 99);
    assert_eq!(result.status, RETURNED);
    assert_eq!(globals.named_properties[2], value::NULL);
    // Accessors are left to the interpreter.
    let result = run_with_global_cache(&globals, &set, &[value::NULL], property(shape, 1, value::int32(12)), 99);
    assert_eq!(result.status, EXITED);

    // Objects are speculated to stay where they are.
    let object = u64::from(value::OBJECT_TAG) << 48 | 0x1000;
    let result = run_with_global_cache(&globals, &get, &[], property(shape, 2, object), 99);
    assert_eq!(result.status, EXITED);
    globals.named_properties[2] = object;
    let result = run_with_global_cache(&globals, &get, &[], property(shape, 2, object), 99);
    assert_eq!((result.status, result.value), (RETURNED, object));
    globals.environment.values[0] = value::int32(1);
    let result = run_with_global_cache(&globals, &get, &[], binding(0, true, Some(object)), 99);
    assert_eq!(result.status, EXITED);

    // Objects in properties of a dictionary global object that were never
    // assigned are constants: the code reads neither the shape nor the
    // property.
    let unassigned = GlobalCacheSnapshot::Property {
        shape: CellId(shape + 8),
        dictionary_generation: Some(3),
        offset: 2,
        writes_data_property: true,
        assigned: false,
        value: value_of(object),
    };
    globals.named_properties[2] = value::NULL;
    let result = run_with_global_cache(&globals, &get, &[], unassigned, 99);
    assert_eq!((result.status, result.value), (RETURNED, object));
}

#[test]
fn instance_of_global_functions_walks_prototype_chains() {
    use crate::snapshot::CellId;
    use crate::snapshot::GlobalCacheSnapshot;
    use crate::snapshot::GlobalValueSnapshot;
    use crate::snapshot::OrdinaryHasInstanceSnapshot;
    const EXITED: u64 = crate::code::JitStatus::Resume as u64;
    // Classes of objects with the ordinary [[GetPrototypeOf]] and with
    // another one (see `class_object_methods`).
    let (ordinary_methods, exotic_methods) = (vec![0u64], vec![0x1234u64]);
    let ordinary_class = vec![0, ordinary_methods.as_ptr() as u64];
    let exotic_class = vec![0, exotic_methods.as_ptr() as u64];
    // Objects of the class whose shape has the prototype (at
    // `shape_prototype`, 0), with named properties.
    struct Fake {
        object: Vec<u64>,
        shape: Vec<u64>,
        properties: Vec<u64>,
    }
    let fake = |class: &[u64], prototype: Option<&Fake>, properties: Vec<u64>| {
        let mut fake = Box::new(Fake {
            object: vec![0; 8],
            shape: vec![prototype.map_or(0, |prototype| prototype.object.as_ptr() as u64)],
            properties,
        });
        fake.object[0] = class.as_ptr() as u64;
        fake.object[6] = fake.shape.as_ptr() as u64;
        fake.object[7] = fake.properties.as_ptr() as u64;
        fake
    };
    let base_prototype = fake(&ordinary_class, None, Vec::new());
    let prototype = fake(&ordinary_class, Some(&base_prototype), Vec::new());
    let instance = fake(&ordinary_class, Some(&prototype), Vec::new());
    let other = fake(&ordinary_class, Some(&base_prototype), Vec::new());
    let exotic = fake(&exotic_class, Some(&prototype), Vec::new());
    let behind_exotic = fake(&ordinary_class, Some(&exotic), Vec::new());
    let mut function = fake(
        &ordinary_class,
        None,
        vec![value::int32(1), object_value(&prototype.object)],
    );
    let (function_value, function_shape) = (object_value(&function.object), function.object[6]);

    let program = assemble(|_| {
        vec![
            Instruction::Enter,
            Instruction::GetGlobal {
                value_feedback: 0,
                dst: r(5),
                identifier: crate::bytecode::IdentifierTableIndex(0),
                cache: 1,
            },
            Instruction::InstanceOf {
                dst: r(6),
                lhs: argument(0),
                rhs: r(5),
            },
            Instruction::Return { value: r(6) },
        ]
    });
    let globals = FakeGlobals::new();
    let run = |value: u64, shape: u64| {
        let snapshot_globals = crate::snapshot::GlobalsSnapshot {
            object: CellId(globals.object.as_ptr() as u64),
            declarative_environment: CellId(globals.environment.address()),
            environment_serial: 99,
            caches: vec![
                None,
                Some(GlobalCacheSnapshot::Binding {
                    index: 2,
                    mutable: false,
                    assigned: false,
                    value: Some(GlobalValueSnapshot {
                        bits: function_value,
                        cell: None,
                        intrinsic: None,
                        has_instance: Some(OrdinaryHasInstanceSnapshot {
                            shape: CellId(shape),
                            dictionary_generation: None,
                            prototype_offset: 1,
                        }),
                    }),
                }),
            ],
        };
        let (result, _) = run_program_with_snapshot(
            &program,
            &[value],
            |snapshot| snapshot.executables[0].globals = Some(snapshot_globals),
            |_| {},
        );
        result
    };
    let returned = |value: u64, expected: u64| {
        let result = run(value, function_shape);
        assert_eq!((result.status, result.value), (RETURNED, expected), "{value:#x}");
    };
    returned(object_value(&instance.object), value::TRUE);
    returned(object_value(&prototype.object), value::FALSE);
    returned(object_value(&other.object), value::FALSE);
    returned(value::int32(1), value::FALSE);
    returned(value::NULL, value::FALSE);
    // Exotic objects in the chain before the prototype, functions with
    // other shapes, and prototypes that are no objects exit.
    assert_eq!(run(object_value(&exotic.object), function_shape).status, EXITED);
    assert_eq!(run(object_value(&behind_exotic.object), function_shape).status, EXITED);
    assert_eq!(run(object_value(&instance.object), function_shape + 8).status, EXITED);
    function.properties[1] = value::int32(2);
    assert_eq!(run(object_value(&instance.object), function_shape).status, EXITED);
}

#[test]
fn modulo_of_random_int32_values() {
    let program = assemble(|_| {
        vec![
            Instruction::Enter,
            binary_instruction(
                BinaryOp::Mod,
                OperationInput::Operand(argument(0)),
                OperationInput::Operand(argument(1)),
            ),
            Instruction::Return { value: r(5) },
        ]
    });
    let mut snapshot = crate::builder::tests::snapshot_for(&program, layout());
    snapshot.runtime = marker_runtime();
    snapshot.executables[0].feedback.arith = vec![INT32_RESULTS_FEEDBACK];
    let compiled = compile_for::<crate::asm::MacroAssembler>(&snapshot, &|_| true).unwrap();
    let mut random = crate::regalloc::tests::Random(0x1234_5678_9ABC_DEF1);
    for _ in 0..20000 {
        let pick = |random: &mut crate::regalloc::tests::Random| {
            let bits = random.next();
            match bits % 4 {
                0 => (bits >> 32) as i32,
                1 => ((bits >> 32) as i32) >> (bits % 31),
                2 => i32::MAX - (bits >> 40) as i32,
                _ => i32::MIN + (bits >> 40) as i32,
            }
        };
        let (lhs, rhs) = (value::int32(pick(&mut random)), value::int32(pick(&mut random)));
        let mut machine = Machine::new(layout(), &[lhs, rhs]);
        machine.vm.resize(8, 0);
        let result = machine.run(&snapshot, &compiled);
        let result = if result.status == crate::code::JitStatus::Resume as u64 {
            EXITED
        } else {
            assert_eq!(result.status, RETURNED);
            result.value
        };
        assert_eq!(
            result,
            binary(BinaryOp::Mod, lhs, rhs, INT32_RESULTS_FEEDBACK),
            "{lhs:#x} % {rhs:#x}"
        );
    }
}

#[test]
fn checks_take_the_slow_path_only_when_they_fail() {
    let object = u64::from(value::OBJECT_TAG) << 48 | 0x1000;
    let slow_paths = |instruction: Instruction, argument: u64| {
        let program = assemble(|_| {
            vec![
                Instruction::Enter,
                instruction.clone(),
                Instruction::Return { value: c(1) },
            ]
        });
        let (result, machine) = run_program(&program, &[argument], |machine| {
            // The this value register.
            machine.frame[FRAME_HEADER_WORDS + 2] = argument;
        });
        let calls = machine.vm[VM_SLOW_PUTS_WORD];
        // NB: The fake slow paths of the throwing checks throw.
        if calls != 0 && !matches!(instruction, Instruction::ResolveThisBinding) {
            assert_eq!(result.status, EXIT_INTERPRETER);
        } else {
            assert_eq!(result, returned(value::int32(10)));
        }
        calls
    };
    for (value, uninitialized, not_object, nullish) in [
        (value::EMPTY, 1, 1, 0),
        (value::int32(0), 0, 1, 0),
        (object, 0, 0, 0),
        (value::UNDEFINED, 0, 1, 1),
        (value::NULL, 0, 1, 1),
        (value::FALSE, 0, 1, 0),
    ] {
        let src = argument(0);
        assert_eq!(
            slow_paths(Instruction::ThrowIfTDZ { src }, value),
            uninitialized,
            "{value:#x}"
        );
        assert_eq!(
            slow_paths(Instruction::ThrowIfNotObject { src }, value),
            not_object,
            "{value:#x}"
        );
        assert_eq!(
            slow_paths(Instruction::ThrowIfNullish { src }, value),
            nullish,
            "{value:#x}"
        );
        assert_eq!(
            slow_paths(Instruction::ResolveThisBinding, value),
            uninitialized,
            "{value:#x}"
        );
    }
}

#[test]
fn every_fast_path_compiles_for_both_architectures() {
    let identifier = crate::bytecode::IdentifierTableIndex(0);
    let program = assemble(|label| {
        let mut instructions = vec![Instruction::Enter];
        for op in BINARY_OPS {
            instructions.push(binary_instruction(
                op,
                OperationInput::Operand(argument(0)),
                OperationInput::Operand(argument(1)),
            ));
            instructions.push(binary_instruction(
                op,
                OperationInput::Operand(r(5)),
                OperationInput::Int32(3),
            ));
        }
        for comparison in COMPARISONS {
            instructions.push(compare_instruction(
                comparison,
                r(6),
                argument(0),
                OperationInput::Operand(r(5)),
            ));
            instructions.push(compare_instruction(comparison, r(6), r(5), OperationInput::Int32(-1)));
        }
        let (dst, src) = (r(5), argument(2));
        instructions.extend([
            Instruction::UnaryPlus {
                arith_feedback: 0,
                dst,
                src,
            },
            Instruction::UnaryMinus {
                arith_feedback: 0,
                dst,
                src,
            },
            Instruction::BitwiseNot {
                arith_feedback: 0,
                dst,
                src,
            },
            Instruction::ToInt32 {
                arith_feedback: 0,
                dst,
                value: src,
            },
            Instruction::Increment { arith_feedback: 0, dst },
            Instruction::PostfixDecrement {
                arith_feedback: 0,
                dst: r(6),
                src: dst,
            },
            Instruction::GetByValue {
                value_feedback: 0,
                keyed_feedback: 0,
                dst,
                base: argument(0),
                property: argument(1),
                base_identifier: None,
                cache: 0,
            },
            Instruction::PutByValue {
                keyed_feedback: 0,
                base: argument(0),
                property: argument(1),
                src,
                kind: 0,
                base_identifier: None,
                cache: 0,
            },
            Instruction::GetLength {
                value_feedback: 0,
                dst,
                base: argument(0),
                base_identifier: None,
                cache: 0,
            },
            Instruction::GetBinding {
                value_feedback: 0,
                dst,
                identifier,
                cache: coordinate(2, 300),
            },
            Instruction::GetCalleeAndThisFromEnvironment {
                value_feedback: 0,
                callee: r(6),
                this_value: r(7),
                identifier,
                cache: coordinate(0, 1),
            },
            Instruction::SetLexicalBinding {
                identifier,
                src,
                cache: coordinate(1, 2),
            },
            Instruction::InitializeVariableBinding {
                identifier,
                src,
                cache: coordinate(0, 3),
            },
            Instruction::GetGlobal {
                value_feedback: 0,
                dst,
                identifier,
                cache: 7,
            },
            Instruction::SetGlobal {
                identifier,
                src: dst,
                cache: 7,
            },
            Instruction::ThrowIfNullish { src },
            Instruction::ResolveThisBinding,
            Instruction::ObjectPropertyIteratorNext {
                dst_value: r(5),
                dst_done: r(6),
                receiver: argument(0),
                keys: argument(1),
                cursor: r(7),
            },
        ]);
        let end = instructions.len() + 1;
        instructions.push(compare_jump_instruction(
            Comparison::LooselyEquals,
            r(5),
            OperationInput::Operand(r(6)),
            true,
            label(end),
            label(end),
        ));
        instructions.push(Instruction::Return { value: r(5) });
        instructions
    });
    let mut snapshot = crate::builder::tests::snapshot_for(&program, layout());
    snapshot.runtime = marker_runtime();
    for opcode in [OpCode::InitializeVariableBinding, OpCode::SetLexicalBinding] {
        snapshot.runtime.slow_paths[opcode as usize] = marker_put as *const () as u64;
    }
    let compiled = compile_for::<crate::asm::aarch64::MacroAssembler>(&snapshot, &|_| true).unwrap();
    let text = crate::asm::disassembler::aarch64(&compiled.code[..compiled.data_offset as usize]).join("\n");
    // NB: Float32Array stores convert to single precision.
    assert!(text.contains("fcvt s31, d"), "{text}");
    let compiled = compile_for::<crate::asm::x86_64::MacroAssembler>(&snapshot, &|_| true).unwrap();
    let text = crate::asm::disassembler::x86_64(&compiled.code[..compiled.data_offset as usize]).join("\n");
    assert!(text.contains("cvtsd2ss"), "{text}");
}

// Property lookup caches: executables have their caches at word 3. Entries
// are laid out like `PropertyLookupCache::Entry`.

/// An object with `properties`, of a shape with dictionary generation 5,
/// and an executable whose cache 1 has an entry for that shape. The
/// properties live in heap storage, whose word 0 is its capacity.
/// The prototypes `GetById` looks up the properties of primitives in.
#[derive(Default, Clone, Copy)]
struct PrimitivePrototypes {
    string: Option<u64>,
    number: Option<u64>,
    boolean: Option<u64>,
}

struct FakeCachedObject {
    object: Vec<u64>,
    properties: Vec<u64>,
    shape: Vec<u64>,
    entry: Vec<u64>,
    caches: Vec<u64>,
    executable: Vec<u64>,
    /// Whether the snapshot says cache 1 is megamorphic, which compiles
    /// lookups that start in its hash tables.
    megamorphic_in_snapshot: std::cell::Cell<bool>,
    /// The entries of the VM's keyed lookup cache the code looks in, or 0.
    keyed_lookup_cache: std::cell::Cell<u64>,
    /// The entries of the VM's keyed store cache the code looks in, or 0.
    keyed_store_cache: std::cell::Cell<u64>,
}

impl FakeCachedObject {
    fn new(properties: &[u64]) -> Box<Self> {
        let mut fake = Box::new(Self {
            object: vec![0; 11],
            properties: [properties.len() as u64]
                .into_iter()
                .chain(properties.iter().copied())
                .collect(),
            shape: vec![0; 12],
            entry: vec![0; 6],
            caches: vec![0; 2],
            executable: vec![0; 4],
            megamorphic_in_snapshot: std::cell::Cell::new(false),
            keyed_lookup_cache: std::cell::Cell::new(0),
            keyed_store_cache: std::cell::Cell::new(0),
        });
        fake.object[5] = 2;
        fake.object[6] = fake.shape.as_ptr() as u64;
        fake.object[7] = fake.properties[1..].as_ptr() as u64;
        fake.shape[10] = 5 << 32;
        fake.caches[1] = fake.entry.as_ptr() as u64 | 1;
        fake.executable[3] = fake.caches.as_ptr() as u64;
        fake.set_entry(2, 1, true);
        fake
    }

    /// Makes the entry one of `entry_type` for property `offset`.
    fn set_entry(&mut self, entry_type: u32, offset: u32, writes_data_property: bool) {
        self.entry.fill(0);
        self.entry[0] = u64::from(entry_type) | u64::from(offset) << 32;
        self.entry[1] = 5 | u64::from(writes_data_property) << 40;
        self.entry[3] = self.shape.as_ptr() as u64;
    }

    fn value(&self) -> u64 {
        object_value(&self.object)
    }

    /// Makes the entry an `AddOwnProperty` one from the object's shape to
    /// `new_shape`, for property `offset`.
    fn set_add_entry(&mut self, new_shape: &[u64], offset: u32) {
        self.set_entry(1, offset, false);
        self.entry[2] = self.shape.as_ptr() as u64;
        self.entry[3] = new_shape.as_ptr() as u64;
    }

    fn run(&self, instruction: Instruction, arguments: &[u64]) -> (JitResult, u64) {
        self.run_with_prototypes(instruction, arguments, PrimitivePrototypes::default())
    }

    fn run_with_prototypes(
        &self,
        instruction: Instruction,
        arguments: &[u64],
        prototypes: PrimitivePrototypes,
    ) -> (JitResult, u64) {
        let program = assemble(|_| {
            vec![
                Instruction::Enter,
                instruction.clone(),
                Instruction::Return { value: r(5) },
            ]
        });
        let (result, machine) = run_program_with_snapshot(
            &program,
            arguments,
            |snapshot| {
                snapshot.runtime.layout.keyed_lookup_cache_entries = self.keyed_lookup_cache.get();
                snapshot.runtime.layout.keyed_store_cache_entries = self.keyed_store_cache.get();
                let executable = &mut snapshot.executables[0];
                executable.string_prototype = prototypes.string.map(crate::snapshot::CellId);
                executable.number_prototype = prototypes.number.map(crate::snapshot::CellId);
                executable.boolean_prototype = prototypes.boolean.map(crate::snapshot::CellId);
                if self.megamorphic_in_snapshot.get() {
                    executable.property_caches.resize(2, Default::default());
                    executable.property_caches[1].kind = crate::snapshot::PropertyCacheKind::Megamorphic;
                }
            },
            |machine| machine.frame[4] = self.executable.as_ptr() as u64,
        );
        (result, machine.vm[VM_SLOW_PUTS_WORD])
    }

    fn get(&self, base: u64) -> u64 {
        let (result, _) = self.run(
            Instruction::GetById {
                value_feedback: 0,
                dst: r(5),
                base: argument(0),
                property: crate::bytecode::PropertyKeyTableIndex(0),
                base_identifier: None,
                cache: 1,
            },
            &[base],
        );
        assert_eq!(result.status, RETURNED);
        result.value
    }

    /// Puts `value`; returns how often the slow path ran.
    fn put(&self, base: u64, value: u64) -> u64 {
        let (result, slow_paths) = self.run(
            Instruction::PutById {
                base: argument(0),
                property: crate::bytecode::PropertyKeyTableIndex(0),
                src: argument(1),
                kind: 0,
                cache: 1,
                base_identifier: None,
            },
            &[base, value],
        );
        assert_eq!(result.status, RETURNED);
        slow_paths
    }

    fn get_length(&self) -> u64 {
        let (result, _) = self.run(
            Instruction::GetLength {
                value_feedback: 0,
                dst: r(5),
                base: argument(0),
                base_identifier: None,
                cache: 1,
            },
            &[self.value()],
        );
        assert_eq!(result.status, RETURNED);
        result.value
    }
}

#[test]
fn get_by_id_through_property_caches() {
    let accessor = u64::from(0xFFFC_u16) << 48 | 0x1000;
    let mut fake = FakeCachedObject::new(&[value::int32(10), value::int32(11), accessor]);
    assert_eq!(fake.get(fake.value()), value::int32(11));
    assert_eq!(fake.get_length(), value::int32(11));
    // Accessors are called by the slow path.
    fake.set_entry(2, 2, false);
    assert_eq!(fake.get(fake.value()), MARKER);
    assert_eq!(fake.get_length(), MARKER);
    // Missing properties, other shapes and generations go to the probe, and
    // to the slow path if it misses.
    fake.set_entry(6, 0, false);
    assert_eq!(fake.get(fake.value()), MARKER);
    fake.set_entry(2, 0, false);
    fake.entry[3] = 8;
    assert_eq!(fake.get(fake.value()), MARKER);
    assert_eq!(fake.get_length(), MARKER);
    fake.set_entry(2, 0, false);
    fake.shape[10] = 6 << 32;
    assert_eq!(fake.get(fake.value()), MARKER);
    fake.object[0] = 1;
    assert_eq!(fake.get(fake.value()), value::int32(42));
    // NB: GetLength probes the cache like GetById.
    assert_eq!(fake.get_length(), value::int32(42));
    fake.object[0] = 0;
    fake.shape[10] = 5 << 32;
    assert_eq!(fake.get(value::int32(1)), MARKER);

    // Properties of a prototype, while its chain is valid.
    let prototype = FakeCachedObject::new(&[value::TRUE]);
    let mut validity = vec![1u64];
    fake.set_entry(3, 0, false);
    fake.entry[4] = prototype.object.as_ptr() as u64;
    fake.entry[5] = validity.as_ptr() as u64;
    assert_eq!(fake.get(fake.value()), value::TRUE);
    assert_eq!(fake.get_length(), value::TRUE);
    validity[0] = 0;
    std::hint::black_box(&validity);
    assert_eq!(fake.get(fake.value()), MARKER);
}

#[test]
fn put_by_id_adds_properties_through_property_caches() {
    let mut fake = FakeCachedObject::new(&[value::int32(10), value::int32(11), value::EMPTY]);
    let old_shape = fake.shape.as_ptr() as u64;
    // A shape with three properties, and dictionary generation 5.
    let mut new_shape = vec![0u64; 12];
    new_shape[10] = 3 | 5 << 32;
    let new_shape_address = new_shape.as_ptr() as u64;
    fake.set_add_entry(&new_shape, 2);
    let add = |fake: &mut FakeCachedObject, flags: u16| {
        fake.object[1] = u64::from(flags);
        fake.object[6] = old_shape;
        let slow_paths = fake.put(fake.value(), value::TRUE);
        assert_eq!(fake.object[6] == new_shape_address, slow_paths == 0);
        slow_paths
    };
    assert_eq!(add(&mut fake, FLAG_IS_EXTENSIBLE), 0);
    assert_eq!(fake.properties[3], value::TRUE);
    // Objects that are not extensible, may interfere with indexed accesses,
    // require slow additions or have a magical length are left to the probe.
    for flags in [
        0,
        FLAG_IS_EXTENSIBLE | FLAG_MAY_INTERFERE,
        FLAG_IS_EXTENSIBLE | FLAG_REQUIRES_SLOW_ADD_OWN_PROPERTY,
        FLAG_IS_EXTENSIBLE | FLAG_HAS_MAGICAL_LENGTH,
    ] {
        assert_eq!(add(&mut fake, flags), 1);
    }
    // So are other shapes, generations, invalid prototype chains, missing
    // new shapes, and objects whose storage would have to grow.
    fake.entry[2] = 8;
    assert_eq!(add(&mut fake, FLAG_IS_EXTENSIBLE), 1);
    fake.set_add_entry(&new_shape, 2);
    fake.entry[1] = 6;
    assert_eq!(add(&mut fake, FLAG_IS_EXTENSIBLE), 1);
    fake.set_add_entry(&new_shape, 2);
    let mut validity = vec![0u64];
    fake.entry[5] = validity.as_ptr() as u64;
    assert_eq!(add(&mut fake, FLAG_IS_EXTENSIBLE), 1);
    validity[0] = 1;
    std::hint::black_box(&validity);
    assert_eq!(add(&mut fake, FLAG_IS_EXTENSIBLE), 0);
    fake.entry[5] = 0;
    fake.entry[3] = 0;
    assert_eq!(add(&mut fake, FLAG_IS_EXTENSIBLE), 1);
    fake.set_add_entry(&new_shape, 2);
    fake.properties[0] = 2;
    assert_eq!(add(&mut fake, FLAG_IS_EXTENSIBLE), 1);

    // Objects with their properties in inline storage, which holds as many
    // as their size class has room for.
    fake.object[7] = fake.object[8..].as_ptr() as u64;
    new_shape[10] = 2 | 5 << 32;
    fake.set_add_entry(&new_shape, 1);
    assert_eq!(add(&mut fake, FLAG_IS_EXTENSIBLE), 0);
    assert_eq!(fake.object[9], value::TRUE);
    new_shape[10] = 3 | 5 << 32;
    fake.set_add_entry(&new_shape, 2);
    assert_eq!(add(&mut fake, FLAG_IS_EXTENSIBLE), 1);
    fake.object[5] = 3;
    assert_eq!(add(&mut fake, FLAG_IS_EXTENSIBLE), 0);
    assert_eq!(fake.object[10], value::TRUE);
}

/// The primary and secondary table indices of the entry for `shape` and
/// `key` in a megamorphic cache, like `PropertyLookupCache::
/// megamorphic_primary_index()` and `megamorphic_secondary_index()`.
fn megamorphic_indices(shape: u64, key: u64) -> (usize, usize) {
    let hash = ((shape ^ key) as u32).wrapping_mul(0x9e37_79b9);
    ((hash >> 26) as usize, ((hash >> 20) & 63) as usize)
}

#[test]
fn get_by_id_through_megamorphic_property_caches() {
    const ENTRY_WORDS: usize = 8;
    let mut fake = FakeCachedObject::new(&[value::int32(10), value::int32(11)]);
    // Lookups compiled for caches that were megamorphic start in the tables.
    for megamorphic_in_snapshot in [false, true] {
        fake.megamorphic_in_snapshot.set(megamorphic_in_snapshot);
        let shape = fake.shape.as_ptr() as u64;
        let (primary_index, secondary_index) = megamorphic_indices(shape, 0);
        // The most recently used entry, then the primary and the secondary table.
        let mut data = vec![0u64; ENTRY_WORDS * (1 + 2 * 64)];
        let entry_at = |data: &mut Vec<u64>, index: usize, offset: u32| {
            let entry = &mut data[index * ENTRY_WORDS..(index + 1) * ENTRY_WORDS];
            entry[0] = 2 | u64::from(offset) << 32;
            entry[1] = 5;
            entry[3] = shape;
        };
        let primary = 1 + primary_index;
        let secondary = 1 + 64 + secondary_index;
        data[3] = 8;
        fake.caches[1] = data.as_ptr() as u64 | 2;
        // Not in either table: the probe, then the slow path.
        assert_eq!(fake.get(fake.value()), MARKER);
        entry_at(&mut data, secondary, 0);
        assert_eq!(fake.get(fake.value()), value::int32(10));
        assert_eq!(fake.get_length(), value::int32(10));
        entry_at(&mut data, primary, 1);
        assert_eq!(fake.get(fake.value()), value::int32(11));
        // Other dictionary generations miss.
        data[primary * ENTRY_WORDS + 1] = 6;
        data[secondary * ENTRY_WORDS + 1] = 6;
        assert_eq!(fake.get(fake.value()), MARKER);
        std::hint::black_box(&data);
    }
}

/// A fake string whose storage word, at `primitive_string_storage`, is
/// `identity`.
struct FakeKeyString(Box<[u64; 4]>);

impl FakeKeyString {
    fn new(identity: u64) -> Self {
        Self(Box::new([0, 0, 0, identity]))
    }

    fn value(&self) -> u64 {
        u64::from(STRING_TAG) << 48 | self.0.as_ptr() as u64
    }
}

/// Fake string memory for character reads: the storage word at
/// `primitive_string_storage` (24) and the deferred kind bits of byte 9.
fn fake_string_with_storage(storage: u64, deferred: bool) -> u64 {
    let memory: &'static mut [u64; 4] = Box::leak(Box::new([0, if deferred { 0b10 << 8 } else { 0 }, 0, storage]));
    u64::from(STRING_TAG) << 48 | memory.as_ptr() as u64
}

/// Fake `Utf16StringData` with `units` as ASCII bytes, or as UTF-16 code
/// units if `wide`, after the 24 byte header.
fn fake_string_data(units: &[u16], wide: bool) -> u64 {
    let mut words = vec![0u64; 3 + units.len().div_ceil(2)];
    words[0] = (units.len() as u64) << 32;
    words[2] = u64::from(wide);
    let bytes = words[3..].as_mut_ptr() as *mut u8;
    for (index, unit) in units.iter().enumerate() {
        // SAFETY: The words after the header have room for two bytes per unit.
        unsafe {
            if wide {
                (bytes as *mut u16).add(index).write_unaligned(*unit);
            } else {
                bytes.add(index).write(*unit as u8);
            }
        }
    }
    Box::leak(words.into_boxed_slice()).as_ptr() as u64
}

#[test]
fn get_by_value_of_string_characters() {
    // Fake strings of the ASCII characters, at their cell addresses.
    let characters: &'static [u64; 128] = Box::leak(Box::new(core::array::from_fn(|character| {
        0x10_0000 + 64 * character as u64
    })));
    let get = |base: u64, index: u64| {
        let program = assemble(|_| {
            vec![
                Instruction::Enter,
                Instruction::GetByValue {
                    value_feedback: 0,
                    keyed_feedback: 0,
                    dst: r(5),
                    base: argument(0),
                    property: argument(1),
                    base_identifier: None,
                    cache: 0,
                },
                Instruction::Return { value: r(5) },
            ]
        });
        let (result, _) = run_program_with_snapshot(
            &program,
            &[base, index],
            |snapshot| snapshot.runtime.layout.single_ascii_character_strings = characters.as_ptr() as u64,
            |_| {},
        );
        assert_eq!(result.status, RETURNED);
        result.value
    };
    let character = |character: u8| u64::from(STRING_TAG) << 48 | (0x10_0000 + 64 * u64::from(character));

    // A short string: the byte count and flag, then the bytes.
    let short = with_length(
        fake_string_with_storage(u64::from_le_bytes([3 << 2 | 1, b'a', b'b', b'c', 0, 0, 0, 0]), false),
        3,
    );
    assert_eq!(get(short, value::int32(0)), character(b'a'));
    assert_eq!(get(short, value::int32(2)), character(b'c'));
    assert_eq!(get(short, value::int32(3)), MARKER);
    assert_eq!(get(short, value::int32(-1)), MARKER);
    // Long strings with ASCII and UTF-16 storage.
    let units: Vec<u16> = "hello world".encode_utf16().collect();
    let ascii = with_length(
        fake_string_with_storage(fake_string_data(&units, false), false),
        units.len() as u32,
    );
    assert_eq!(get(ascii, value::int32(4)), character(b'o'));
    assert_eq!(get(ascii, value::int32(10)), character(b'd'));
    assert_eq!(get(ascii, value::int32(11)), MARKER);
    let units: Vec<u16> = "x\u{e9}y".encode_utf16().collect();
    let wide = with_length(
        fake_string_with_storage(fake_string_data(&units, true), false),
        units.len() as u32,
    );
    assert_eq!(get(wide, value::int32(0)), character(b'x'));
    assert_eq!(get(wide, value::int32(2)), character(b'y'));
    // Characters beyond ASCII take the slow path.
    assert_eq!(get(wide, value::int32(1)), MARKER);
    // So do strings without storage, deferred ones, and other keys.
    assert_eq!(
        get(with_length(fake_string_with_storage(0, false), 1), value::int32(0)),
        MARKER
    );
    assert_eq!(
        get(
            with_length(
                fake_string_with_storage(fake_string_data(&units, false), true),
                units.len() as u32
            ),
            value::int32(0)
        ),
        MARKER
    );
    assert_eq!(get(ascii, 1.0f64.to_bits()), MARKER);
    assert_eq!(get(value::int32(5), value::int32(0)), MARKER);
}

/// Fake function memory: its flags at byte 8, and the builtin it is (if
/// any) at bytes 24 and 25, see `function_object_builtin` in the test
/// layout.
fn fake_function(builtin: Option<u8>) -> u64 {
    let builtin = builtin.map_or(0, |builtin| u64::from(builtin) | 1 << 8);
    let memory: &'static mut [u64; 4] = Box::leak(Box::new([0, u64::from(FUNCTION_FLAG), 0, builtin]));
    u64::from(value::OBJECT_TAG) << 48 | memory.as_ptr() as u64
}

#[test]
fn string_character_builtin_calls() {
    let characters: &'static [u64; 128] = Box::leak(Box::new(core::array::from_fn(|character| {
        0x10_0000 + 64 * character as u64
    })));
    let call = |code_unit: bool, callee: u64, this_value: u64, index: u64| {
        let instruction = if code_unit {
            Instruction::CallBuiltinStringPrototypeCharCodeAt {
                call_feedback: 0,
                dst: r(5),
                callee: argument(0),
                this_value: argument(1),
                argument: argument(2),
                expression_string: None,
            }
        } else {
            Instruction::CallBuiltinStringPrototypeCharAt {
                call_feedback: 0,
                dst: r(5),
                callee: argument(0),
                this_value: argument(1),
                argument: argument(2),
                expression_string: None,
            }
        };
        let program = assemble(|_| {
            vec![
                Instruction::Enter,
                instruction.clone(),
                Instruction::Return { value: r(5) },
            ]
        });
        let (result, _) = run_program_with_snapshot(
            &program,
            &[callee, this_value, index],
            |snapshot| snapshot.runtime.layout.single_ascii_character_strings = characters.as_ptr() as u64,
            |_| {},
        );
        assert_eq!(result.status, RETURNED);
        result.value
    };
    let character = |character: u8| u64::from(STRING_TAG) << 48 | (0x10_0000 + 64 * u64::from(character));
    let (char_code_at, char_at) = (fake_function(Some(7)), fake_function(Some(8)));
    let units: Vec<u16> = "a\u{e9}".encode_utf16().collect();
    let string = with_length(fake_string_with_storage(fake_string_data(&units, true), false), 2);

    assert_eq!(call(true, char_code_at, string, value::int32(0)), value::int32(0x61));
    assert_eq!(call(true, char_code_at, string, value::int32(1)), value::int32(0xe9));
    assert_eq!(call(false, char_at, string, value::int32(0)), character(b'a'));
    // Code units beyond ASCII have no string of the VM's.
    assert_eq!(call(false, char_at, string, value::int32(1)), MARKER);
    // Indices out of bounds and of other types, other this values and
    // callees take the slow path.
    assert_eq!(call(true, char_code_at, string, value::int32(2)), MARKER);
    assert_eq!(call(true, char_code_at, string, value::int32(-1)), MARKER);
    assert_eq!(call(true, char_code_at, string, 0.0f64.to_bits()), MARKER);
    assert_eq!(call(true, char_code_at, value::int32(5), value::int32(0)), MARKER);
    assert_eq!(call(true, char_at, string, value::int32(0)), MARKER);
    assert_eq!(call(false, char_code_at, string, value::int32(0)), MARKER);
    assert_eq!(call(true, fake_function(None), string, value::int32(0)), MARKER);
    assert_eq!(call(true, value::UNDEFINED, string, value::int32(0)), MARKER);
}

/// `Math.round` like the spec defines it: the integer closest to the
/// number, halves rounded up, and -0 for numbers from -0.5 to -0.
fn js_round(number: f64) -> f64 {
    if !number.is_finite() {
        return number;
    }
    let floor = number.floor();
    let rounded = if number - floor >= 0.5 { floor + 1.0 } else { floor };
    if rounded == 0.0 && number.is_sign_negative() {
        -0.0
    } else {
        rounded
    }
}

#[test]
fn math_builtin_calls() {
    type Reference = fn(f64) -> f64;
    let functions: [(u8, Reference); 5] = [
        (9, f64::abs),
        (10, f64::floor),
        (11, f64::ceil),
        (12, js_round),
        (13, f64::sqrt),
    ];
    let call = |builtin: u8, callee: u64, argument_value: u64| {
        let instruction = match builtin {
            9 => Instruction::CallBuiltinMathAbs {
                call_feedback: 0,
                dst: r(5),
                callee: argument(0),
                this_value: argument(2),
                argument: argument(1),
                expression_string: None,
            },
            10 => Instruction::CallBuiltinMathFloor {
                call_feedback: 0,
                dst: r(5),
                callee: argument(0),
                this_value: argument(2),
                argument: argument(1),
                expression_string: None,
            },
            11 => Instruction::CallBuiltinMathCeil {
                call_feedback: 0,
                dst: r(5),
                callee: argument(0),
                this_value: argument(2),
                argument: argument(1),
                expression_string: None,
            },
            12 => Instruction::CallBuiltinMathRound {
                call_feedback: 0,
                dst: r(5),
                callee: argument(0),
                this_value: argument(2),
                argument: argument(1),
                expression_string: None,
            },
            _ => Instruction::CallBuiltinMathSqrt {
                call_feedback: 0,
                dst: r(5),
                callee: argument(0),
                this_value: argument(2),
                argument: argument(1),
                expression_string: None,
            },
        };
        let program = assemble(|_| {
            vec![
                Instruction::Enter,
                instruction.clone(),
                Instruction::Return { value: r(5) },
            ]
        });
        let (result, _) =
            run_program_with_snapshot(&program, &[callee, argument_value, value::UNDEFINED], |_| {}, |_| {});
        assert_eq!(result.status, RETURNED);
        result.value
    };
    let numbers = [
        0.0,
        -0.0,
        0.3,
        -0.3,
        0.5,
        -0.5,
        0.49999999999999994,
        -0.49999999999999994,
        1.5,
        -1.5,
        2.5,
        -2.5,
        4.7,
        -4.7,
        1e-300,
        -1e-300,
        4503599627370495.5,
        -4503599627370495.5,
        4503599627370496.0,
        9007199254740993.0,
        1e300,
        -1e300,
        f64::INFINITY,
        f64::NEG_INFINITY,
        f64::NAN,
        2147483647.5,
        -2147483648.5,
    ];
    let integers = [0, 1, -1, 5, -7, i32::MAX, i32::MIN];
    for (builtin, reference) in functions {
        let callee = fake_function(Some(builtin));
        for number in numbers {
            let expected = box_number(reference(number));
            assert_eq!(
                call(builtin, callee, number.to_bits()),
                expected,
                "builtin {builtin} of {number:?}"
            );
        }
        for integer in integers {
            // NB: The absolute value of the smallest int32 is no int32; the
            //     slow path makes it.
            let expected = if builtin == 9 && integer == i32::MIN {
                MARKER
            } else {
                box_number(reference(f64::from(integer)))
            };
            assert_eq!(
                call(builtin, callee, value::int32(integer)),
                expected,
                "builtin {builtin} of {integer}"
            );
        }
        // Other callees and arguments take the slow path.
        assert_eq!(call(builtin, fake_function(Some(builtin + 1)), value::int32(1)), MARKER);
        assert_eq!(call(builtin, fake_function(None), value::int32(1)), MARKER);
        assert_eq!(call(builtin, value::UNDEFINED, value::int32(1)), MARKER);
        assert_eq!(call(builtin, callee, value::UNDEFINED), MARKER);
        assert_eq!(call(builtin, callee, value::TRUE), MARKER);
    }
}

/// The fake string `string`, with its length set to `length`.
fn with_length(string: u64, length: u32) -> u64 {
    let address = (string & 0xFFFF_FFFF_FFFF) as *mut u64;
    // SAFETY: Fake strings are four leaked words, the third of which holds the length.
    unsafe { address.add(2).write(u64::from(length)) };
    string
}

/// A fake string of `length` code units with the storage word `storage`
/// (at `primitive_string_storage`), not interned.
fn fake_string_of_length(length: u32, storage: u64) -> u64 {
    let memory: &'static mut [u64; 4] = Box::leak(Box::new([0, 0, u64::from(length), storage]));
    u64::from(STRING_TAG) << 48 | memory.as_ptr() as u64
}

/// The storage word of a short string of ASCII `bytes`.
fn short_string_word(bytes: &[u8]) -> u64 {
    let mut word = [0u8; 8];
    word[0] = (bytes.len() as u8) << 2 | 1;
    word[1..=bytes.len()].copy_from_slice(bytes);
    u64::from_le_bytes(word)
}

#[test]
fn strict_equality_of_strings_by_length_and_short_storage() {
    // NB: The slow path makes no `true` of the `MARKER` it writes.
    let equals = |lhs: u64, rhs: u64| {
        let instruction = compare_instruction(
            Comparison::StrictlyEquals,
            r(5),
            argument(0),
            OperationInput::Operand(argument(1)),
        );
        match run_instruction_with(GENERAL_FEEDBACK, instruction, &[lhs, rhs]) {
            (_, 1) => MARKER,
            (result, _) => result,
        }
    };
    let short = |bytes: &[u8]| fake_string_of_length(bytes.len() as u32, short_string_word(bytes));
    // Long strings point at their data, which these tests never read.
    let long = |length: u32| fake_string_of_length(length, 0x10_0000);
    assert_eq!(equals(short(b"ab"), short(b"ab")), value::TRUE);
    assert_eq!(equals(short(b"ab"), short(b"ac")), value::FALSE);
    assert_eq!(equals(short(b"ab"), short(b"abc")), value::FALSE);
    assert_eq!(equals(short(b"abcdefg"), short(b"abcdefg")), value::TRUE);
    assert_eq!(equals(long(20), long(21)), value::FALSE);
    assert_eq!(equals(short(b"abc"), long(4)), value::FALSE);
    // Strings of the same length need their contents compared, unless both
    // are short.
    assert_eq!(equals(long(20), long(20)), MARKER);
    assert_eq!(equals(short(b"abc"), long(3)), MARKER);
    assert_eq!(equals(fake_string_of_length(3, 0), short(b"abc")), MARKER);
}

/// `u64_hash()` of the runtime: the MurmurHash3 64-bit finalizer.
fn murmur3_finalizer(mut key: u64) -> u64 {
    key ^= key >> 33;
    key = key.wrapping_mul(0xff51_afd7_ed55_8ccd);
    key ^= key >> 33;
    key = key.wrapping_mul(0xc4ce_b9fe_1a85_ec53);
    key ^= key >> 33;
    key
}

#[test]
fn short_string_concatenations_through_the_fly_string_cache() {
    let cache: &'static mut [u64; 1024] = Box::leak(Box::new([0; 1024]));
    let cache_address = cache.as_ptr() as u64;
    let ab = short_string_word(b"ab");
    let cached = fake_string_of_length(2, ab);
    cache[(murmur3_finalizer(ab) & 1023) as usize] = cached & ((1 << 48) - 1);
    let a7 = short_string_word(b"a7");
    let cached_a7 = fake_string_of_length(2, a7);
    cache[(murmur3_finalizer(a7) & 1023) as usize] = cached_a7 & ((1 << 48) - 1);
    // The VM's strings of the integers below 10: only 7 has one.
    let numbers: &'static mut [u64; 10] = Box::leak(Box::new([0; 10]));
    numbers[7] = fake_string_of_length(1, short_string_word(b"7")) & ((1 << 48) - 1);
    let numbers_address = numbers.as_ptr() as u64;
    let concatenate = |lhs: u64, rhs: u64| {
        let program = assemble(|_| {
            vec![
                Instruction::Enter,
                Instruction::Add {
                    arith_feedback: 0,
                    dst: r(5),
                    lhs: argument(0),
                    rhs: argument(1),
                },
                Instruction::Return { value: r(5) },
            ]
        });
        let (result, _) = run_program_with_snapshot(
            &program,
            &[lhs, rhs],
            |snapshot| {
                snapshot.runtime.rope_allocation = crate::snapshot::RopeAllocationInfo {
                    allocator: 1,
                    cell_size: 64,
                    template: vec![0; 8],
                    lhs_offset: 32,
                    rhs_offset: 40,
                    length_offset: 16,
                    min_length: 8,
                };
                snapshot.runtime.layout.fly_string_cache = cache_address;
                snapshot.runtime.layout.fly_string_cache_mask = 1023;
                snapshot.runtime.layout.numeric_string_cache = numbers_address;
                snapshot.runtime.layout.numeric_string_cache_size = 10;
                // ArithFeedback::String and ArithFeedback::Int32.
                snapshot.executables[0].feedback.arith = vec![1 << 3 | 1];
            },
            |_| {},
        );
        assert_eq!(result.status, RETURNED);
        result.value
    };
    let short = |bytes: &[u8]| fake_string_of_length(bytes.len() as u32, short_string_word(bytes));
    assert_eq!(concatenate(short(b"a"), short(b"b")), cached);
    // Results the cache does not have, and strings that are not short, take
    // the slow path.
    assert_eq!(concatenate(short(b"b"), short(b"a")), MARKER);
    assert_eq!(concatenate(short(b"ab"), short(b"c")), MARKER);
    assert_eq!(concatenate(fake_string_of_length(1, 0x10_0000), short(b"b")), MARKER);
    assert_eq!(concatenate(short(b"a"), fake_string_of_length(1, 0)), MARKER);
    // Integers the VM has strings of concatenate like their strings.
    assert_eq!(concatenate(short(b"a"), value::int32(7)), cached_a7);
    assert_eq!(concatenate(value::int32(7), short(b"a")), MARKER);
    assert_eq!(concatenate(short(b"a"), value::int32(6)), MARKER);
    assert_eq!(concatenate(short(b"a"), value::int32(-7)), MARKER);
    assert_eq!(concatenate(short(b"a"), value::int32(10)), MARKER);
}

#[test]
fn typeof_of_every_kind_of_value() {
    let strings = crate::snapshot::TypeofStrings {
        number: 1,
        undefined: 2,
        object: 3,
        string: 4,
        symbol: 5,
        boolean: 6,
        bigint: 7,
        function: 8,
    };
    let type_of = |value: u64| {
        let program = assemble(|_| {
            vec![
                Instruction::Enter,
                Instruction::Typeof {
                    dst: r(5),
                    src: argument(0),
                },
                Instruction::Return { value: r(5) },
            ]
        });
        let (result, _) = run_program_with_snapshot(
            &program,
            &[value],
            |snapshot| snapshot.runtime.layout.typeof_strings = strings,
            |_| {},
        );
        assert_eq!(result.status, RETURNED);
        result.value
    };
    assert_eq!(type_of(value::int32(1)), strings.number);
    assert_eq!(type_of(1.5f64.to_bits()), strings.number);
    assert_eq!(type_of(0x7FF8_0000_0000_0000), strings.number);
    assert_eq!(type_of(f64::INFINITY.to_bits()), strings.number);
    assert_eq!(type_of(value::UNDEFINED), strings.undefined);
    assert_eq!(type_of(value::NULL), strings.object);
    assert_eq!(type_of(value::TRUE), strings.boolean);
    assert_eq!(type_of(fake_string(false)), strings.string);
    assert_eq!(type_of(u64::from(SYMBOL_TAG) << 48 | 0x1000), strings.symbol);
    assert_eq!(type_of(u64::from(BIGINT_TAG) << 48 | 0x1000), strings.bigint);
    assert_eq!(type_of(object_value(&PLAIN_OBJECTS[0])), strings.object);
    assert_eq!(type_of(object_value(&HTMLDDA_OBJECT)), strings.undefined);
    assert_eq!(type_of(fake_function(None)), strings.function);
}

#[test]
fn get_by_value_through_the_vm_keyed_lookup_cache() {
    const ENTRY_WORDS: usize = 8;
    const NAME: u64 = 0x5001;
    let mut fake = FakeCachedObject::new(&[value::int32(10), value::int32(11)]);
    // The access's own cache has nothing.
    fake.caches[1] = 0;
    let mut cache = vec![0u64; ENTRY_WORDS << 11];
    fake.keyed_lookup_cache.set(cache.as_ptr() as u64);
    let shape = fake.shape.as_ptr() as u64;
    let index = ((shape as u32 ^ NAME as u32).wrapping_mul(0x9e37_79b9) >> 21) as usize;
    let (key, other_key) = (FakeKeyString::new(NAME), FakeKeyString::new(0x6001));
    let get = |fake: &FakeCachedObject, key: u64| {
        let (result, _) = fake.run(
            Instruction::GetByValue {
                value_feedback: 0,
                keyed_feedback: 0,
                dst: r(5),
                base: argument(0),
                property: argument(1),
                base_identifier: None,
                cache: 1,
            },
            &[fake.value(), key],
        );
        assert_eq!(result.status, RETURNED);
        result.value
    };
    assert_eq!(get(&fake, key.value()), MARKER);
    // An own property of the shape, with the shape's dictionary generation.
    let entry = &mut cache[index * ENTRY_WORDS..(index + 1) * ENTRY_WORDS];
    entry[0] = 3 | 1 << 32;
    entry[1] = 5;
    entry[2] = shape;
    entry[5] = NAME;
    assert_eq!(get(&fake, key.value()), value::int32(11));
    // Other names, keys that are not strings, other generations and other
    // kinds of entries miss.
    assert_eq!(get(&fake, other_key.value()), MARKER);
    assert_eq!(get(&fake, value::TRUE), MARKER);
    cache[index * ENTRY_WORDS + 1] = 6;
    assert_eq!(get(&fake, key.value()), MARKER);
    cache[index * ENTRY_WORDS + 1] = 5;
    cache[index * ENTRY_WORDS] = 5 | 1 << 32;
    assert_eq!(get(&fake, key.value()), MARKER);
    std::hint::black_box(&cache);
}

#[test]
fn put_by_value_through_the_vm_keyed_store_cache() {
    const ENTRY_WORDS: usize = 8;
    const NAME: u64 = 0x5001;
    let mut fake = FakeCachedObject::new(&[value::int32(10), value::int32(11)]);
    // The access's own cache has nothing.
    fake.caches[1] = 0;
    let mut cache = vec![0u64; ENTRY_WORDS << 11];
    fake.keyed_store_cache.set(cache.as_ptr() as u64);
    let shape = fake.shape.as_ptr() as u64;
    let index = ((shape as u32 ^ NAME as u32).wrapping_mul(0x9e37_79b9) >> 21) as usize;
    let (key, other_key) = (FakeKeyString::new(NAME), FakeKeyString::new(0x6001));
    // Returns how often the slow path ran.
    let put = |fake: &FakeCachedObject, key: u64, value: u64| {
        let (result, slow_paths) = fake.run(
            Instruction::PutByValue {
                keyed_feedback: 0,
                base: argument(0),
                property: argument(1),
                src: argument(2),
                kind: 0,
                base_identifier: None,
                cache: 1,
            },
            &[fake.value(), key, value],
        );
        assert_eq!(result.status, RETURNED);
        slow_paths
    };
    assert_eq!(put(&fake, key.value(), value::int32(20)), 1);
    assert_eq!(fake.properties[2], value::int32(11));
    // A writable own data property of the shape, with the shape's
    // dictionary generation.
    let entry = &mut cache[index * ENTRY_WORDS..(index + 1) * ENTRY_WORDS];
    entry[0] = 2 | 1 << 32;
    entry[1] = 5;
    entry[2] = shape;
    entry[5] = NAME;
    assert_eq!(put(&fake, key.value(), value::int32(21)), 0);
    assert_eq!(fake.properties[2], value::int32(21));
    // Other names, keys that are not strings, other generations, other
    // kinds of entries and accessors miss.
    assert_eq!(put(&fake, other_key.value(), value::int32(22)), 1);
    assert_eq!(put(&fake, value::TRUE, value::int32(22)), 1);
    cache[index * ENTRY_WORDS + 1] = 6;
    assert_eq!(put(&fake, key.value(), value::int32(22)), 1);
    cache[index * ENTRY_WORDS + 1] = 5;
    cache[index * ENTRY_WORDS] = 3 | 1 << 32;
    assert_eq!(put(&fake, key.value(), value::int32(22)), 1);
    cache[index * ENTRY_WORDS] = 2 | 1 << 32;
    let accessor = u64::from(0xFFFC_u16) << 48 | 0x1000;
    fake.properties[2] = accessor;
    assert_eq!(put(&fake, key.value(), value::int32(22)), 1);
    assert_eq!(fake.properties[2], accessor);
    std::hint::black_box(&cache);
}

#[test]
fn get_by_value_through_megamorphic_property_caches() {
    const ENTRY_WORDS: usize = 8;
    let mut fake = FakeCachedObject::new(&[value::int32(10), value::int32(11)]);
    // Lookups compiled for caches that were megamorphic start in the tables.
    for megamorphic_in_snapshot in [false, true] {
        fake.megamorphic_in_snapshot.set(megamorphic_in_snapshot);
        let shape = fake.shape.as_ptr() as u64;
        let key = u64::from(STRING_TAG) << 48 | 0x5000;
        let other_key = u64::from(STRING_TAG) << 48 | 0x6000;
        let (primary_index, secondary_index) = megamorphic_indices(shape, key);
        let mut data = vec![0u64; ENTRY_WORDS * (1 + 2 * 64)];
        let primary = 1 + primary_index;
        let secondary = 1 + 64 + secondary_index;
        let entry = &mut data[secondary * ENTRY_WORDS..(secondary + 1) * ENTRY_WORDS];
        entry[0] = 2 | 1 << 32;
        entry[1] = 5;
        entry[3] = shape;
        entry[6] = key;
        data[3] = 8;
        fake.caches[1] = data.as_ptr() as u64 | 2;
        let get = |fake: &FakeCachedObject, key: u64| {
            let (result, _) = fake.run(
                Instruction::GetByValue {
                    value_feedback: 0,
                    keyed_feedback: 0,
                    dst: r(5),
                    base: argument(0),
                    property: argument(1),
                    base_identifier: None,
                    cache: 1,
                },
                &[fake.value(), key],
            );
            assert_eq!(result.status, RETURNED);
            result.value
        };
        assert_eq!(get(&fake, key), value::int32(11));
        // Other keys, and entries in the slot of the primary table that are for
        // other keys, miss.
        assert_eq!(get(&fake, other_key), MARKER);
        data[primary * ENTRY_WORDS + 3] = shape;
        data[primary * ENTRY_WORDS + 6] = other_key;
        assert_eq!(get(&fake, key), value::int32(11));
        std::hint::black_box(&data);
    }
}

#[test]
fn get_by_id_of_missing_properties_through_property_caches() {
    let mut fake = FakeCachedObject::new(&[value::int32(10)]);
    fake.set_entry(6, 0, false);
    fake.object[0] = PLAIN_OBJECT_VTABLE;
    assert_eq!(fake.get(fake.value()), value::UNDEFINED);
    // Objects of other classes may have properties their shapes do not
    // describe: the probe, then the slow path. ECMAScript functions only
    // do for names whose absence is never cached.
    fake.object[0] = 0;
    assert_eq!(fake.get(fake.value()), MARKER);
    fake.object[1] = u64::from(FLAG_IS_ECMASCRIPT_FUNCTION);
    assert_eq!(fake.get(fake.value()), value::UNDEFINED);
    fake.object[1] = 0;
    // Shapes with a prototype need a valid prototype chain.
    fake.object[0] = PLAIN_OBJECT_VTABLE;
    fake.shape[0] = 0x8000;
    assert_eq!(fake.get(fake.value()), MARKER);
    let mut validity = vec![1u64];
    fake.entry[5] = validity.as_ptr() as u64;
    assert_eq!(fake.get(fake.value()), value::UNDEFINED);
    validity[0] = 0;
    std::hint::black_box(&validity);
    assert_eq!(fake.get(fake.value()), MARKER);
}

#[test]
fn get_by_id_of_strings_through_property_caches() {
    // The fake `%String.prototype%`, whose cache entry is for its property 1.
    let string_prototype = FakeCachedObject::new(&[value::int32(6), value::int32(7)]);
    let address = string_prototype.object.as_ptr() as u64;
    let string = fake_string(true);
    let get = |base: u64, with_string_prototype: bool| {
        let (result, _) = string_prototype.run_with_prototypes(
            Instruction::GetById {
                value_feedback: 0,
                dst: r(5),
                base: argument(0),
                property: crate::bytecode::PropertyKeyTableIndex(0),
                base_identifier: None,
                cache: 1,
            },
            &[base],
            PrimitivePrototypes {
                string: with_string_prototype.then_some(address),
                ..PrimitivePrototypes::default()
            },
        );
        assert_eq!(result.status, RETURNED);
        result.value
    };
    assert_eq!(get(string, true), value::int32(7));
    assert_eq!(get(string_prototype.value(), true), value::int32(7));
    // Without a `%String.prototype%`, strings take the slow path, like
    // other primitives always do.
    assert_eq!(get(string, false), MARKER);
    assert_eq!(get(value::int32(1), true), MARKER);
}

#[test]
fn get_by_id_of_numbers_and_booleans_through_property_caches() {
    // One fake prototype for both, whose cache entry is for its property 1.
    let prototype = FakeCachedObject::new(&[value::int32(6), value::int32(7)]);
    let address = prototype.object.as_ptr() as u64;
    let get = |base: u64, prototypes: PrimitivePrototypes| {
        let (result, _) = prototype.run_with_prototypes(
            Instruction::GetById {
                value_feedback: 0,
                dst: r(5),
                base: argument(0),
                property: crate::bytecode::PropertyKeyTableIndex(0),
                base_identifier: None,
                cache: 1,
            },
            &[base],
            prototypes,
        );
        assert_eq!(result.status, RETURNED);
        result.value
    };
    let both = PrimitivePrototypes {
        number: Some(address),
        boolean: Some(address),
        ..PrimitivePrototypes::default()
    };
    assert_eq!(get(value::int32(-3), both), value::int32(7));
    assert_eq!(get(1.5f64.to_bits(), both), value::int32(7));
    assert_eq!(get(value::TRUE, both), value::int32(7));
    assert_eq!(get(prototype.value(), both), value::int32(7));
    // Other primitives, and primitives without a prototype in the
    // snapshot, take the slow path.
    assert_eq!(get(value::UNDEFINED, both), MARKER);
    assert_eq!(get(value::NULL, both), MARKER);
    assert_eq!(get(fake_string(true), both), MARKER);
    assert_eq!(get(value::int32(1), PrimitivePrototypes::default()), MARKER);
    assert_eq!(get(value::FALSE, PrimitivePrototypes::default()), MARKER);
}

#[test]
fn get_by_id_of_strings_through_megamorphic_property_caches() {
    const ENTRY_WORDS: usize = 8;
    let mut string_prototype = FakeCachedObject::new(&[value::int32(6), value::int32(7)]);
    let address = string_prototype.object.as_ptr() as u64;
    let shape = string_prototype.shape.as_ptr() as u64;
    // An entry in the primary table, which the lookup finds after the most
    // recently used entry missed.
    let mut data = vec![0u64; ENTRY_WORDS * (1 + 2 * 64)];
    let primary = 1 + megamorphic_indices(shape, 0).0;
    data[3] = 8;
    data[primary * ENTRY_WORDS] = 2 | 1 << 32;
    data[primary * ENTRY_WORDS + 1] = 5;
    data[primary * ENTRY_WORDS + 3] = shape;
    string_prototype.caches[1] = data.as_ptr() as u64 | 2;
    let (result, _) = string_prototype.run_with_prototypes(
        Instruction::GetById {
            value_feedback: 0,
            dst: r(5),
            base: argument(0),
            property: crate::bytecode::PropertyKeyTableIndex(0),
            base_identifier: None,
            cache: 1,
        },
        &[fake_string(true)],
        PrimitivePrototypes {
            string: Some(address),
            ..PrimitivePrototypes::default()
        },
    );
    assert_eq!(result.status, RETURNED);
    assert_eq!(result.value, value::int32(7));
    std::hint::black_box(&data);
}

#[test]
fn keyed_accesses_of_missing_and_added_properties_through_property_caches() {
    const ENTRY_WORDS: usize = 8;
    let mut fake = FakeCachedObject::new(&[value::int32(10), value::int32(11), value::EMPTY]);
    fake.object[0] = PLAIN_OBJECT_VTABLE;
    fake.object[1] = u64::from(FLAG_IS_EXTENSIBLE);
    let shape = fake.shape.as_ptr() as u64;
    let key = u64::from(STRING_TAG) << 48 | 0x5000;
    let other_key = u64::from(STRING_TAG) << 48 | 0x6000;
    // A megamorphic cache whose most recently used entry is the one tested.
    let mut data = vec![0u64; ENTRY_WORDS * (1 + 2 * 64)];
    fake.caches[1] = data.as_ptr() as u64 | 2;
    let set_entry = |data: &mut Vec<u64>, entry_type: u64, offset: u64, from_shape: u64, shape: u64| {
        data[..ENTRY_WORDS].fill(0);
        data[0] = entry_type | offset << 32;
        data[1] = 5 | 1 << 40;
        data[2] = from_shape;
        data[3] = shape;
        data[6] = key;
    };
    let get = |fake: &FakeCachedObject, key: u64| {
        let (result, _) = fake.run(
            Instruction::GetByValue {
                value_feedback: 0,
                keyed_feedback: 0,
                dst: r(5),
                base: argument(0),
                property: argument(1),
                base_identifier: None,
                cache: 1,
            },
            &[fake.value(), key],
        );
        assert_eq!(result.status, RETURNED);
        result.value
    };
    let put = |fake: &FakeCachedObject, key: u64, value: u64| {
        let (result, slow_paths) = fake.run(
            Instruction::PutByValue {
                keyed_feedback: 0,
                base: argument(0),
                property: argument(1),
                src: argument(2),
                kind: 0,
                base_identifier: None,
                cache: 1,
            },
            &[fake.value(), key, value],
        );
        assert_eq!(result.status, RETURNED);
        slow_paths
    };

    // Missing properties of plain objects.
    set_entry(&mut data, 6, 0, 0, shape);
    assert_eq!(get(&fake, key), value::UNDEFINED);
    assert_eq!(get(&fake, other_key), MARKER);
    fake.object[0] = 0;
    assert_eq!(get(&fake, key), MARKER);
    fake.object[0] = PLAIN_OBJECT_VTABLE;

    // Changes of existing properties.
    set_entry(&mut data, 2, 1, 0, shape);
    assert_eq!(put(&fake, key, value::NULL), 0);
    assert_eq!(fake.properties[2], value::NULL);

    // Additions, from the shape the entry adds a property to.
    let mut new_shape = vec![0u64; 12];
    new_shape[10] = 3 | 5 << 32;
    let new_shape_address = new_shape.as_ptr() as u64;
    set_entry(&mut data, 1, 2, shape, new_shape_address);
    assert_eq!(put(&fake, other_key, value::TRUE), 1);
    assert_eq!(put(&fake, key, value::TRUE), 0);
    assert_eq!(fake.object[6], new_shape_address);
    assert_eq!(fake.properties[3], value::TRUE);
    // Objects that already have the new shape take the slow path.
    assert_eq!(put(&fake, key, value::FALSE), 1);
    assert_eq!(fake.properties[3], value::TRUE);
    // So do objects that may not cache additions.
    fake.object[6] = shape;
    fake.object[1] = u64::from(FLAG_IS_EXTENSIBLE | FLAG_HAS_MAGICAL_LENGTH);
    assert_eq!(put(&fake, key, value::FALSE), 1);
    assert_eq!(fake.object[6], shape);
    std::hint::black_box((&data, &new_shape));
}

#[test]
fn put_by_id_through_property_caches() {
    let accessor = u64::from(0xFFFC_u16) << 48 | 0x1000;
    let mut fake = FakeCachedObject::new(&[value::int32(10), value::int32(11), accessor]);
    assert_eq!(fake.put(fake.value(), value::NULL), 0);
    assert_eq!(fake.properties[2], value::NULL);
    // Entries that do not write data properties, accessors, prototypes,
    // other shapes and non-objects take the probe, then the slow path.
    fake.set_entry(1, 0, false);
    assert_eq!(fake.put(fake.value(), value::TRUE), 1);
    fake.set_entry(1, 2, true);
    assert_eq!(fake.put(fake.value(), value::TRUE), 1);
    fake.set_entry(1, 0, true);
    fake.entry[4] = 8;
    assert_eq!(fake.put(fake.value(), value::TRUE), 1);
    fake.set_entry(1, 0, true);
    fake.entry[3] = 8;
    assert_eq!(fake.put(fake.value(), value::TRUE), 1);
    assert_eq!(fake.properties[1], value::int32(10));
    assert_eq!(fake.put(value::UNDEFINED, value::TRUE), 1);
    // A probe that handles the store.
    fake.object[0] = 1;
    assert_eq!(fake.put(fake.value(), value::FALSE), 0);
    assert_eq!(fake.object[1], value::FALSE);
}

#[test]
fn in_of_array_elements() {
    let elements = [value::int32(10), value::EMPTY, value::int32(12)];
    let holey = FakeArray::new(FLAG_HAS_MAGICAL_LENGTH, HOLEY, 3, &elements, 3);
    let has = |key: u64, object: u64| {
        run_instruction(
            Instruction::In {
                dst: r(5),
                lhs: argument(0),
                rhs: argument(1),
            },
            &[key, object],
        )
    };
    assert_eq!(has(value::int32(0), holey.value()), value::TRUE);
    assert_eq!(has(value::int32(2), holey.value()), value::TRUE);
    // Holes, indices out of bounds, other keys and other values than
    // objects take the slow path, since tests have no runtime helper.
    assert_eq!(has(value::int32(1), holey.value()), MARKER);
    assert_eq!(has(value::int32(3), holey.value()), MARKER);
    assert_eq!(has(value::int32(-1), holey.value()), MARKER);
    assert_eq!(has(value::UNDEFINED, holey.value()), MARKER);
    assert_eq!(has(value::int32(0), value::int32(0)), MARKER);
}
