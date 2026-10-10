/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Compiles hand-assembled bytecode and runs it natively (on x86-64) against
//! a fake runtime: a fake VM, frame and executable laid out with made up
//! offsets, and Rust functions standing in for the slow paths and helpers.

use super::*;
use crate::bytecode::FrameLayout;
use crate::bytecode::Instruction;
use crate::bytecode::NUM_OPCODES;
use crate::bytecode::OpCode;
use crate::bytecode::Operand;
use crate::bytecode::test_support::*;
use crate::code::CompiledCode;
use crate::code::ExitKind;
use crate::code::ValueLocation;
use crate::compile_for;
use crate::snapshot::FunctionFrameFields;
use crate::snapshot::RuntimeLayout;
use crate::snapshot::RuntimeOffsets;
use crate::snapshot::Snapshot;
use std::cell::RefCell;

/// Words of the fake `ExecutionContext` before its slots.
const FRAME_HEADER_WORDS: usize = 24;
const FUNCTION_FLAG: u16 = 1 << 3;
const HTMLDDA_FLAG: u16 = 1 << 13;

fn offsets() -> RuntimeOffsets {
    RuntimeOffsets {
        execution_context_program_counter: 0,
        execution_context_lexical_environment: 8,
        execution_context_private_environment: 16,
        execution_context_frame_initialized: 24,
        execution_context_executable: 32,
        execution_context_slots: (8 * FRAME_HEADER_WORDS) as u32,
        vm_running_execution_context: 0,
        vm_jit_native_stack_limit: 8,
        private_environment_outer: 0,
        object_shape: 0,
        object_flags: 8,
        vm_interpreter_stack_top: 24,
        vm_interpreter_stack_limit: 32,
        object_named_properties: 16,
        shape_dictionary_generation: 0,
        prototype_chain_validity_valid: 0,
        accessor_getter: 8,
        accessor_setter: 16,
        execution_context_function: 64,
        execution_context_realm: 72,
        execution_context_script_or_module: 80,
        execution_context_variable_environment: 96,
        execution_context_frame_id: 104,
        execution_context_skip_when_determining_incumbent_counter: 4,
        execution_context_yield_continuation: 112,
        execution_context_yield_is_await: 116,
        execution_context_yield_value_is_iterator_result: 117,
        execution_context_caller_is_construct: 118,
        execution_context_this_value: 120,
        execution_context_caller_frame: (8 * FRAME_CALLER_WORD) as u32,
        execution_context_passed_argument_count: 136,
        execution_context_caller_return_pc: 140,
        execution_context_caller_dst_raw: 144,
        execution_context_slot_count: 148,
        execution_context_argument_count: 152,
        execution_context_returns_to_native_caller: 156,
        execution_context_runs_jit_code: 157,
        ecmascript_function_environment: 0,
        ecmascript_function_private_environment: 8,
        ecmascript_function_script_or_module: 16,
        vm_execution_generation: 8 * VM_EXECUTION_GENERATION_WORD as u32,
    }
}

/// Words of the fake VM.
const VM_EXECUTION_GENERATION_WORD: usize = 6;
const VM_WORDS: usize = 9;
#[cfg(target_arch = "x86_64")]
/// The word of the fake VM with its native function table.
const VM_NATIVE_FUNCTION_TABLE_WORD: usize = 8;

/// A fake heap block for code that pops local free lists: a link in a free
/// cell only leads within the block of the cell (see
/// `ObjectAllocationInfo`), so free cells must be in a block.
#[cfg(target_arch = "x86_64")]
#[repr(C, align(16384))]
struct FakeBlock([u64; 2048]);

#[cfg(target_arch = "x86_64")]
impl FakeBlock {
    const LINK_MASK: u32 = 16384 - 1;
    /// Where its one free cell is, in words.
    const CELL: usize = 8;

    /// A block with a free cell of `words` words, `filler` beyond the link.
    fn new(words: usize, filler: u64) -> Box<Self> {
        let mut block = Box::new(Self([0; 2048]));
        block.0[Self::CELL + 1..Self::CELL + words].fill(filler);
        block
    }

    fn cell(&self) -> &[u64] {
        &self.0[Self::CELL..]
    }

    fn address(&self) -> u64 {
        self.0.as_ptr() as u64
    }
}

/// One frame of an exit: executable, pc, how to resume, and slot values.
type ExitFrame = (u32, u32, crate::code::ResumeMode, Vec<(u32, u64)>);

/// What the fake slow paths need to know about the code being run. The
/// fake VM points at it.
#[derive(Default)]
struct TestState {
    /// The bytecode, constants and layout of each snapshot executable.
    bytecodes: Vec<Vec<u8>>,
    constants: Vec<Vec<u64>>,
    layouts: Vec<FrameLayout>,
    /// The `Executable` cell of each snapshot executable.
    cells: Vec<u64>,
    /// Every frame of every exit taken.
    exit_frames: Vec<Vec<ExitFrame>>,
    exits: Vec<Site>,
    exits_taken: Vec<u32>,
    /// Make the slow path of the instruction at this pc return this control word.
    fail: Option<(u32, i64)>,
    /// A word `libjs_jit_call` writes, as a call may change the heap.
    call_writes: Option<(*mut u64, u64)>,
    /// A word `libjs_jit_call` flips between two values.
    call_toggles: Option<(*mut u64, u64, u64)>,
    /// How often `libjs_jit_call` ran.
    generic_calls: u32,
    /// How often `libjs_jit_create_arguments` ran.
    arguments_created: u32,
    /// What direct calls did.
    direct: direct_calls::DirectCallState,
}

/// Where the fake VM keeps its pointer to the `TestState`.
const VM_TEST_STATE_WORD: usize = 2;

fn test_state<'a>(vm: *mut u64) -> &'a RefCell<TestState> {
    // SAFETY: The fake VM of a running test points at its machine's state.
    unsafe { &*(*vm.add(VM_TEST_STATE_WORD) as *const RefCell<TestState>) }
}

/// The header word of a fake frame with its executable.
const FRAME_EXECUTABLE_WORD: usize = 4;
/// The header word of a fake frame with its caller frame.
const FRAME_CALLER_WORD: usize = 5;

/// The snapshot index of the running frame's executable: the one whose cell
/// it runs, or the compiled one.
fn running_executable(vm: *mut u64) -> usize {
    // SAFETY: The fake VM's running frame is a live fake frame.
    let executable = unsafe { *(*vm as *const u64).add(FRAME_EXECUTABLE_WORD) };
    let state = test_state(vm).borrow();
    state.cells.iter().position(|cell| *cell == executable).unwrap_or(0)
}

fn continuation_after(vm: *mut u64, pc: u32) -> i64 {
    let state = test_state(vm).borrow();
    if let Some((fail_pc, control)) = state.fail
        && fail_pc == pc
    {
        return control;
    }
    let next_pc = decode_instruction(&state.bytecodes[running_executable(vm)], pc)
        .unwrap()
        .next_pc();
    (CONTINUATION_BIT | u64::from(next_pc)) as i64
}

fn decoded(vm: *mut u64, pc: u32) -> Instruction {
    decode_instruction(&test_state(vm).borrow().bytecodes[running_executable(vm)], pc)
        .unwrap()
        .instruction
}

fn int(value: u64) -> i32 {
    (value as u32).cast_signed()
}

#[cfg(target_arch = "x86_64")]
fn is_int32(bits: u64) -> bool {
    (bits >> 48) as u16 == value::INT32_TAG
}

#[cfg(target_arch = "x86_64")]
fn add(dst: Operand, lhs: Operand, rhs: Operand) -> Instruction {
    Instruction::Add {
        arith_feedback: 0,
        dst,
        lhs,
        rhs,
    }
}

#[cfg(target_arch = "x86_64")]
/// The number the interpreter's fast paths see in a value: int32 values and
/// doubles other than NaN.
fn fast_path_number(bits: u64) -> Option<f64> {
    if is_int32(bits) {
        Some(f64::from(int(bits)))
    } else if (bits >> 48) & 0x7FF8 != 0x7FF8 {
        Some(f64::from_bits(bits))
    } else {
        None
    }
}

#[cfg(target_arch = "x86_64")]
/// A number as `JS::Value(double)` boxes it.
fn box_number(number: f64) -> u64 {
    let integer = number as i32;
    if f64::from(integer) == number && !(integer == 0 && number.is_sign_negative()) {
        value::int32(integer)
    } else if number.is_nan() {
        0x7FF8_0000_0000_0000
    } else {
        number.to_bits()
    }
}

/// Two-register slow path result.
#[repr(C)]
struct SlowPathResult {
    control: i64,
    value: u64,
}

extern "C" fn add_values(vm: *mut u64, pc: u32, dst: *mut u64, lhs: u64, rhs: u64) -> i64 {
    let control = continuation_after(vm, pc);
    if control >= 0 && control as u64 & CONTINUATION_BIT != 0 {
        // SAFETY: Compiled code passes the address of a frame slot.
        unsafe { *dst = value::int32(int(lhs).wrapping_add(int(rhs))) };
    }
    control
}

extern "C" fn jump_less_than(vm: *mut u64, pc: u32, lhs: u64, rhs: u64, if_true: u32, if_false: u32) -> i64 {
    if let Some((fail_pc, control)) = test_state(vm).borrow().fail
        && fail_pc == pc
    {
        return control;
    }
    i64::from(if int(lhs) < int(rhs) { if_true } else { if_false })
}

/// A fake `ToInt32` or `ToLength` slow path (scalar convention) that
/// doubles its input.
extern "C" fn to_int32(vm: *mut u64, pc: u32, instruction: *const u8, value: u64) -> SlowPathResult {
    // SAFETY: Compiled code passes a pointer to the instruction in the snapshot's bytecode.
    let opcode = unsafe { *instruction };
    assert!(opcode == OpCode::ToInt32 as u8 || opcode == OpCode::ToLength as u8);
    SlowPathResult {
        control: continuation_after(vm, pc),
        value: value::int32(int(value).wrapping_mul(2)),
    }
}

/// A fake `Increment` slow path (scalar inputs, outputs in the record).
extern "C" fn increment(vm: *mut u64, pc: u32, instruction: *const u8, outputs: *mut u64, dst: u64) -> i64 {
    // SAFETY: See `to_int32`; `outputs` is the record in the JIT frame.
    unsafe {
        assert_eq!(*instruction, OpCode::Increment as u8);
        *outputs = value::int32(int(dst).wrapping_add(1));
    }
    continuation_after(vm, pc)
}

/// A fake `ConcatString` slow path (scalar inputs, outputs in the record)
/// that adds its operands.
extern "C" fn concat_string(
    vm: *mut u64,
    pc: u32,
    instruction: *const u8,
    outputs: *mut u64,
    dst: u64,
    src: u64,
) -> i64 {
    // SAFETY: See `to_int32`; `outputs` is the record in the JIT frame.
    unsafe {
        assert_eq!(*instruction, OpCode::ConcatString as u8);
        *outputs = value::int32(int(dst).wrapping_add(int(src)));
    }
    continuation_after(vm, pc)
}

/// A fake `NewArray` slow path (record convention) that sums its elements.
extern "C" fn new_array(vm: *mut u64, pc: u32, instruction: *const u8, values: *mut u64) -> i64 {
    let Instruction::NewArray { elements, .. } = decoded(vm, pc) else {
        panic!("not a NewArray");
    };
    // SAFETY: See `to_int32`; `values` has the destination then the elements.
    unsafe {
        assert_eq!(*instruction, OpCode::NewArray as u8);
        let sum = (0..elements.len()).fold(0i32, |sum, index| sum.wrapping_add(int(*values.add(1 + index))));
        *values = value::int32(sum);
    }
    continuation_after(vm, pc)
}

/// A fake `libjs_jit_call` that "calls" an int32 callee by adding the arguments to it.
extern "C" fn jit_call(vm: *mut u64, frame: *mut u64, pc: u32) -> i64 {
    let Instruction::Call {
        dst, callee, arguments, ..
    } = decoded(vm, pc)
    else {
        panic!("not a Call");
    };
    test_state(vm).borrow_mut().generic_calls += 1;
    let slot = |operand: Operand| FRAME_HEADER_WORDS + operand.raw() as usize;
    // SAFETY: The frame is the fake frame of the running test.
    unsafe {
        let mut result = int(*frame.add(slot(callee)));
        for argument in arguments {
            result = result.wrapping_add(int(*frame.add(slot(argument))));
        }
        *frame.add(slot(dst)) = value::int32(result);
        if let Some((address, value)) = test_state(vm).borrow().call_writes {
            *address = value;
        }
        if let Some((address, first, second)) = test_state(vm).borrow().call_toggles {
            *address = if *address == first { second } else { first };
        }
    }
    continuation_after(vm, pc)
}

/// The value fake runtimes give the arguments objects they create.
fn fake_arguments_object(mapped: bool) -> u64 {
    (u64::from(value::OBJECT_TAG) << 48) | 0xa000 | u64::from(mapped)
}

/// A fake `libjs_jit_create_arguments`.
extern "C" fn create_arguments(vm: *mut u64, _frame: *mut u64, mapped: u32) -> u64 {
    test_state(vm).borrow_mut().arguments_created += 1;
    fake_arguments_object(mapped != 0)
}

/// A fake `libjs_jit_call_forwarding_arguments` that "calls" its function, an
/// int32, by adding the frame's passed arguments to it.
extern "C" fn call_forwarding_arguments(vm: *mut u64, frame: *mut u64, pc: u32) -> i64 {
    let Instruction::Call { dst, this_value, .. } = decoded(vm, pc) else {
        panic!("not a Call");
    };
    let layout = test_state(vm).borrow().layouts[0];
    let slot = |index: u32| FRAME_HEADER_WORDS + index as usize;
    // SAFETY: The frame is the fake frame of the running test.
    unsafe {
        let passed = *frame.cast::<u8>().add(136).cast::<u32>();
        let mut result = int(*frame.add(slot(this_value.raw())));
        for index in 0..passed {
            result = result.wrapping_add(int(*frame.add(slot(layout.arguments_base() + index))));
        }
        *frame.add(slot(dst.raw())) = value::int32(result);
    }
    continuation_after(vm, pc)
}

/// A fake `asm_helper_to_boolean`, for values that are no strings, symbols
/// or bigints: doubles by their value, and other values truthy unless they
/// are undefined or null.
extern "C" fn to_boolean(value: u64) -> u64 {
    const NAN_BASE_TAG: u64 = 0x7FF8;
    if (value >> 48) & NAN_BASE_TAG != NAN_BASE_TAG {
        return u64::from(value << 1 != 0);
    }
    u64::from(value != value::UNDEFINED && value != value::NULL && value != NAN_BASE_TAG << 48)
}

/// A fake `libjs_jit_exit` that writes the exit's values into the frame,
/// like the runtime does. Leaves neither count as exits nor move the pc.
extern "C" fn jit_exit(vm: *mut u64, frame: *mut u64, exit_index: u32, dump: *const u64) {
    if test_state(vm).borrow().exits[exit_index as usize].kind == SiteKind::Publish {
        publish_frames(vm, exit_index);
        return;
    }
    // NB: Unlike the runtime, the fake pushes no frames for slow paths of
    //     fast paths in inlined callees.
    let is_leave = matches!(test_state(vm).borrow().exits[exit_index as usize].kind, SiteKind::Leave);
    if is_leave {
        write_frame_states(vm, frame, exit_index, dump, Leaving::Yes);
        return;
    }
    test_state(vm).borrow_mut().exits_taken.push(exit_index);
    write_frame_states(vm, frame, exit_index, dump, Leaving::No);
}

/// Like the runtime's Header translation, pushes the frames of the inlined
/// calls of a publish site on the fake interpreter stack, linked to their
/// callers, uninitialized, with their pc.
fn publish_frames(vm: *mut u64, exit_index: u32) {
    let (site, layouts, cells) = {
        let state = test_state(vm).borrow();
        (
            state.exits[exit_index as usize].clone(),
            state.layouts.clone(),
            state.cells.clone(),
        )
    };
    for frame_state in site.frames.iter().rev().skip(1) {
        let layout = layouts[frame_state.executable as usize];
        // SAFETY: The fake VM and interpreter stack of the running test are alive.
        unsafe {
            let frame = *vm.add(3) as *mut u64;
            *frame.add(FRAME_CALLER_WORD) = *vm;
            *frame.cast::<u32>() = frame_state.pc;
            *frame.add(3) &= !0xff;
            *frame.add(FRAME_EXECUTABLE_WORD) = cells[frame_state.executable as usize];
            *vm = frame as u64;
            *vm.add(3) =
                frame.add(FRAME_HEADER_WORDS + (layout.arguments_base() + layout.number_of_arguments) as usize) as u64;
        }
    }
}

#[derive(PartialEq)]
enum Leaving {
    No,
    Yes,
}

fn write_frame_states(vm: *mut u64, frame: *mut u64, exit_index: u32, dump: *const u64, leaving: Leaving) {
    let descriptor = test_state(vm).borrow().exits[exit_index as usize].clone();
    assert_eq!(descriptor.kind == SiteKind::Leave, leaving == Leaving::Yes);
    let frame_pointer_index = <crate::asm::MacroAssembler as PortableMacroAssembler>::FRAME_POINTER.0 as usize;
    // SAFETY: The dump and the JIT frame it points into are alive during the call.
    let frames = unsafe {
        let frame_pointer = *dump.add(frame_pointer_index);
        descriptor
            .frames
            .iter()
            .map(|frame_state| {
                // NB: Like the runtime, create one arguments object per frame.
                if frame_state
                    .values
                    .iter()
                    .any(|(_, location)| matches!(location, ValueLocation::ArgumentsObject { .. }))
                {
                    test_state(vm).borrow_mut().arguments_created += 1;
                }
                let values = frame_state
                    .values
                    .iter()
                    .map(|(slot, location)| {
                        // NB: Like the runtime, box unboxed values.
                        let value = match *location {
                            ValueLocation::Register(register, repr) => {
                                value::boxed_constant(*dump.add(register as usize), repr)
                            }
                            ValueLocation::Stack(offset, repr) => {
                                value::boxed_constant(*((frame_pointer as i64 + i64::from(offset)) as *const u64), repr)
                            }
                            ValueLocation::Constant(bits) => bits,
                            ValueLocation::ArgumentsObject { mapped } => fake_arguments_object(mapped),
                            ValueLocation::VirtualObject(_) => unreachable!("tests make no virtual objects"),
                        };
                        (*slot, value)
                    })
                    .collect::<Vec<_>>();
                (frame_state.executable, frame_state.pc, frame_state.mode, values)
            })
            .collect::<Vec<_>>()
    };
    // Like the runtime, write the values of the compiled function's own frame.
    let (_, pc, _, values) = frames.last().expect("exits describe at least one frame").clone();
    // SAFETY: `frame` is the fake frame of the running test.
    unsafe {
        // NB: Like the runtime, initialize frames compiled code did not.
        if *frame.add(3) & 0xff == 0 {
            let state = test_state(vm).borrow();
            let layout = state.layouts[0];
            for index in crate::bytecode::RESERVED_REGISTER_COUNT..layout.registers_and_locals_count {
                *frame.add(FRAME_HEADER_WORDS + index as usize) = value::EMPTY;
            }
            for (index, constant) in state.constants[0].iter().enumerate() {
                *frame.add(FRAME_HEADER_WORDS + layout.constants_base() as usize + index) = *constant;
            }
            *frame.add(3) |= 1;
        }
        for (slot, value) in values {
            *frame.add(FRAME_HEADER_WORDS + slot as usize) = value;
        }
        if leaving == Leaving::No {
            *frame.cast::<u32>() = pc;
        }
    }
    if leaving == Leaving::No {
        test_state(vm).borrow_mut().exit_frames.push(frames);
        return;
    }
    if descriptor.frames.len() < 2 {
        return;
    }
    // NB: Like the runtime, write the frames compiled code pushed, which are
    //     right above the compiled function's, as far as they are left.
    // SAFETY: The fake frames of the running test are alive.
    unsafe {
        let mut pushed_frames = Vec::new();
        let mut running = *vm as *mut u64;
        while !running.is_null() && running != frame {
            pushed_frames.push(running);
            running = *running.add(FRAME_CALLER_WORD) as *mut u64;
        }
        let pushed = pushed_frames.len().min(frames.len() - 1);
        for index in 0..pushed {
            let pushed_frame = pushed_frames[pushed_frames.len() - 1 - index];
            let (_, _, _, values) = &frames[frames.len() - 2 - index];
            for (slot, value) in values {
                *pushed_frame.add(FRAME_HEADER_WORDS + *slot as usize) = *value;
            }
        }
    }
}

fn runtime() -> RuntimeInfo {
    let mut slow_paths = vec![0; NUM_OPCODES as usize];
    let mut set = |opcode: OpCode, address: u64| slow_paths[opcode as usize] = address;
    set(OpCode::Add, add_values as *const () as u64);
    set(OpCode::AddRhsInt32, add_values as *const () as u64);
    set(OpCode::Exp, add_values as *const () as u64);
    set(OpCode::ExpRhsInt32, add_values as *const () as u64);
    set(OpCode::JumpLessThan, jump_less_than as *const () as u64);
    set(OpCode::JumpLessThanRhsInt32, jump_less_than as *const () as u64);
    set(OpCode::JumpLessThanLoop, jump_less_than as *const () as u64);
    set(OpCode::ToInt32, to_int32 as *const () as u64);
    set(OpCode::ToLength, to_int32 as *const () as u64);
    set(OpCode::Increment, increment as *const () as u64);
    set(OpCode::ConcatString, concat_string as *const () as u64);
    set(OpCode::NewArray, new_array as *const () as u64);
    RuntimeInfo {
        slow_paths,
        jit_call: jit_call as *const () as u64,
        jit_exit: jit_exit as *const () as u64,
        to_boolean: to_boolean as *const () as u64,
        create_arguments: create_arguments as *const () as u64,
        call_forwarding_arguments: call_forwarding_arguments as *const () as u64,
        finish_direct_call: direct_calls::finish_direct_call as *const () as u64,
        no_yield_continuation: u32::MAX,
        heap_region_offset_mask: (1 << 48) - 1,
        shifted_is_cell_pattern: 0xFFF8 << 48,
        object_flag_is_function: FUNCTION_FLAG,
        offsets: offsets(),
        layout: RuntimeLayout {
            object_flag_is_htmldda: HTMLDDA_FLAG,
            primitive_string_length: 16,
            ..RuntimeLayout::default()
        },
        ..RuntimeInfo::default()
    }
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct JitResult {
    value: u64,
    status: u64,
}

const RETURNED: u64 = JitStatus::Returned as u64;
#[cfg(target_arch = "x86_64")]
const RESUME: u64 = JitStatus::Resume as u64;
#[cfg(target_arch = "x86_64")]
const EXIT_INTERPRETER: u64 = JitStatus::ExitInterpreter as u64;

/// A fake VM, frame and executable for running compiled code.
struct Machine {
    state: Box<RefCell<TestState>>,
    vm: Vec<u64>,
    frame: Vec<u64>,
    executable: Vec<u64>,
    /// The fake interpreter stack, for the frames compiled code pushes.
    interpreter_stack: Vec<u64>,
    layout: FrameLayout,
}

impl Machine {
    fn new(layout: FrameLayout, arguments: &[u64]) -> Self {
        let slot_count = (layout.arguments_base() + layout.number_of_arguments) as usize;
        let mut machine = Self {
            state: Box::default(),
            vm: vec![0; VM_WORDS],
            frame: vec![0; FRAME_HEADER_WORDS + slot_count],
            executable: vec![0],
            interpreter_stack: vec![0xdead_beef_dead_beef; 1024],
            layout,
        };
        machine.frame[4] = machine.executable.as_ptr() as u64;
        for (index, argument) in arguments.iter().enumerate() {
            let slot = layout.arguments_base() as usize + index;
            machine.frame[FRAME_HEADER_WORDS + slot] = *argument;
        }
        machine.vm[0] = machine.frame.as_ptr() as u64;
        machine.vm[3] = machine.interpreter_stack.as_ptr() as u64;
        machine.vm[4] = machine.vm[3] + 8 * machine.interpreter_stack.len() as u64;
        machine.vm[VM_TEST_STATE_WORD] = std::ptr::from_ref::<RefCell<TestState>>(&machine.state) as u64;
        machine
    }

    fn slot(&self, operand: Operand) -> u64 {
        self.frame[FRAME_HEADER_WORDS + operand.raw() as usize]
    }

    #[cfg(target_arch = "x86_64")]
    fn program_counter(&self) -> u32 {
        self.frame[0] as u32
    }

    #[cfg(target_arch = "x86_64")]
    fn run(&mut self, snapshot: &Snapshot, compiled: &CompiledCode) -> JitResult {
        self.run_at(snapshot, compiled, compiled.entry_offset)
    }

    /// Runs the code from `offset` on, like the entry point.
    #[cfg(target_arch = "x86_64")]
    fn run_at(&mut self, snapshot: &Snapshot, compiled: &CompiledCode, offset: u32) -> JitResult {
        assert_eq!(snapshot.executables[0].layout, self.layout);
        {
            let mut state = self.state.borrow_mut();
            state.bytecodes = snapshot
                .executables
                .iter()
                .map(|executable| executable.bytecode.clone())
                .collect();
            state.constants = snapshot
                .executables
                .iter()
                .map(|executable| executable.constants.clone())
                .collect();
            state.layouts = snapshot
                .executables
                .iter()
                .map(|executable| executable.layout)
                .collect();
            state.cells = snapshot
                .executables
                .iter()
                .map(|executable| executable.cell.0)
                .collect();
            state.exits = compiled.sites.clone();
            state.exits_taken.clear();
        }
        let code = crate::asm::executable_code::ExecutableCode::new(&compiled.code);
        // NB: Native code enters JIT code through the entry trampoline.
        let trampoline = crate::asm::executable_code::ExecutableCode::new(
            &generate_entry_trampoline::<crate::asm::MacroAssembler>().expect("the trampoline assembles"),
        );
        type Trampoline = extern "C" fn(*mut u64, *mut u64, u64) -> JitResult;
        // SAFETY: The trampoline implements `Trampoline`, and compiled code
        // implements the JIT entry ABI.
        let enter: Trampoline = unsafe { trampoline.function() };
        enter(
            self.vm.as_mut_ptr(),
            self.frame.as_mut_ptr(),
            code.address() + u64::from(offset),
        )
    }
}

/// The `ArithFeedback` of arithmetic and comparisons that saw every kind of
/// number and strings, and int32 results that did not fit: they take their
/// paths for those, and never exit.
const NUMBERS_AND_STRINGS_FEEDBACK: u8 = 0b1111;

fn compile_program(program: &Program, layout: FrameLayout, never_ran: &[usize]) -> (Snapshot, CompiledCode) {
    compile_program_with(program, layout, never_ran, |_| {})
}

fn compile_program_with(
    program: &Program,
    layout: FrameLayout,
    never_ran: &[usize],
    configure: impl FnOnce(&mut Snapshot),
) -> (Snapshot, CompiledCode) {
    let mut snapshot = crate::builder::tests::snapshot_for(program, layout);
    snapshot.runtime = runtime();
    snapshot.executables[0].feedback.arith = vec![NUMBERS_AND_STRINGS_FEEDBACK];
    configure(&mut snapshot);
    let never_ran_pcs = never_ran
        .iter()
        .map(|index| program.offsets[*index])
        .collect::<Vec<_>>();
    let compiled = compile_for::<crate::asm::MacroAssembler>(&snapshot, &|pc| !never_ran_pcs.contains(&pc))
        .unwrap_or_else(|failure| panic!("compilation failed: {failure:?}"));
    (snapshot, compiled)
}

#[cfg(target_arch = "x86_64")]
/// The pc of the (only) loop back edge of `program`, which on-stack
/// replacement entries start at.
fn back_edge_pc(program: &Program) -> u32 {
    crate::bytecode::decode_all(&program.bytes)
        .unwrap()
        .iter()
        .find(|decoded| crate::builder::is_loop_back_edge(decoded.instruction.opcode()))
        .expect("the program has a loop")
        .pc
}

/// Compiles and runs `program` with `test_layout()` and the given arguments.
#[cfg(target_arch = "x86_64")]
fn run(program: &Program, arguments: &[u64]) -> (JitResult, Machine) {
    run_with(program, test_layout(), arguments, &[], |_| {})
}

#[cfg(target_arch = "x86_64")]
fn run_with(
    program: &Program,
    layout: FrameLayout,
    arguments: &[u64],
    never_ran: &[usize],
    prepare: impl FnOnce(&mut Machine),
) -> (JitResult, Machine) {
    let (snapshot, compiled) = compile_program(program, layout, never_ran);
    let mut machine = Machine::new(layout, arguments);
    prepare(&mut machine);
    let result = machine.run(&snapshot, &compiled);
    (result, machine)
}

#[cfg(target_arch = "x86_64")]
fn returned(value: u64) -> JitResult {
    JitResult {
        value,
        status: RETURNED,
    }
}

#[cfg(target_arch = "x86_64")]
mod execution {
    use super::*;

    /// A generic instruction whose fake slow path adds.
    fn add(dst: Operand, lhs: Operand, rhs: Operand) -> Instruction {
        Instruction::Exp {
            arith_feedback: 0,
            dst,
            lhs,
            rhs,
        }
    }

    #[test]
    fn enter_initializes_the_frame_and_slow_paths_get_values() {
        let program = assemble(|_| {
            vec![
                Instruction::Enter,
                Instruction::Mov { dst: r(5), src: c(1) },
                add(r(6), r(5), a(0)),
                Instruction::Return { value: r(6) },
            ]
        });
        let (result, machine) = run(&program, &[value::int32(5)]);
        assert_eq!(result, returned(value::int32(15)));
        // The slow path got the Mov's value as its operand, so the frame
        // holds nothing but empty values.
        assert_eq!(machine.slot(r(5)), value::EMPTY);
        assert_eq!(machine.slot(r(7)), value::EMPTY);
        assert_eq!(machine.slot(l(1)), value::EMPTY);
        assert_eq!(machine.slot(c(0)), value::int32(0));
        assert_eq!(machine.slot(c(1)), value::int32(10));
        assert_eq!(machine.frame[3] & 0xff, 1, "frame_initialized");
        assert_eq!(machine.program_counter(), program.offsets[2]);
    }

    #[test]
    fn loops_with_generic_comparisons_and_arithmetic() {
        // sum = 0; for (i = 0; i < a0; i++) sum += i; return sum;
        let program = assemble(|label| {
            vec![
                Instruction::Enter,
                Instruction::Mov { dst: l(0), src: c(0) },
                Instruction::Mov { dst: r(5), src: c(0) },
                Instruction::JumpLessThan {
                    arith_feedback: 0,
                    lhs: r(5),
                    rhs: a(0),
                    true_target: label(4),
                    false_target: label(7),
                },
                add(l(0), l(0), r(5)),
                Instruction::ExpRhsInt32 {
                    arith_feedback: 0,
                    dst: r(5),
                    lhs: r(5),
                    rhs: 1,
                },
                Instruction::Jump { target: label(3) },
                Instruction::Return { value: l(0) },
            ]
        });
        for (count, sum) in [(0, 0), (1, 0), (10, 45), (100, 4950)] {
            let (result, _) = run(&program, &[value::int32(count)]);
            assert_eq!(result, returned(value::int32(sum)));
        }
    }

    #[test]
    fn osr_entries_continue_loops_with_the_values_in_the_frame() {
        // sum = 0; i = 0; while (i < a0) { sum += i; i++; } return sum;
        let while_loop = assemble(|label| {
            vec![
                Instruction::Enter,
                Instruction::Mov { dst: l(0), src: c(0) },
                Instruction::Mov { dst: r(5), src: c(0) },
                Instruction::JumpLessThan {
                    arith_feedback: 0,
                    lhs: r(5),
                    rhs: a(0),
                    true_target: label(4),
                    false_target: label(7),
                },
                add(l(0), l(0), r(5)),
                Instruction::ExpRhsInt32 {
                    arith_feedback: 0,
                    dst: r(5),
                    lhs: r(5),
                    rhs: 1,
                },
                Instruction::JumpLoop { target: label(3) },
                Instruction::Return { value: l(0) },
            ]
        });
        // The same as a do-while loop, whose back edge is a comparison.
        let do_while_loop = assemble(|label| {
            vec![
                Instruction::Enter,
                Instruction::Mov { dst: l(0), src: c(0) },
                Instruction::Mov { dst: r(5), src: c(0) },
                add(l(0), l(0), r(5)),
                Instruction::ExpRhsInt32 {
                    arith_feedback: 0,
                    dst: r(5),
                    lhs: r(5),
                    rhs: 1,
                },
                Instruction::JumpLessThanLoop {
                    arith_feedback: 0,
                    lhs: r(5),
                    rhs: a(0),
                    true_target: label(3),
                    false_target: label(6),
                },
                Instruction::Return { value: l(0) },
            ]
        });
        // A loop whose condition comes first, and whose forward entry never
        // ran, so that compiled code only reaches it from the OSR entry.
        let entered_loop = assemble(|label| {
            vec![
                Instruction::Enter,
                Instruction::Mov { dst: l(0), src: c(0) },
                Instruction::Mov { dst: r(5), src: c(0) },
                Instruction::Jump { target: label(6) },
                add(l(0), l(0), r(5)),
                Instruction::ExpRhsInt32 {
                    arith_feedback: 0,
                    dst: r(5),
                    lhs: r(5),
                    rhs: 1,
                },
                Instruction::JumpLessThanLoop {
                    arith_feedback: 0,
                    lhs: r(5),
                    rhs: a(0),
                    true_target: label(4),
                    false_target: label(7),
                },
                Instruction::Return { value: l(0) },
            ]
        });
        for (program, back_edge, never_ran) in [
            (&while_loop, 6, &[][..]),
            (&do_while_loop, 5, &[]),
            (&entered_loop, 6, &[1]),
        ] {
            let (snapshot, compiled) = compile_program_with(program, test_layout(), never_ran, |snapshot| {
                snapshot.options.osr_pc = Some(back_edge_pc(program));
            });
            assert_eq!(compiled.osr_entries.len(), 1);
            let (pc, offset) = compiled.osr_entries[0];
            assert_eq!(pc, program.offsets[back_edge]);
            // The interpreter ran Enter and five iterations of the loop.
            let mut machine = Machine::new(test_layout(), &[value::int32(10)]);
            for (index, constant) in snapshot.executables[0].constants.iter().enumerate() {
                machine.frame[FRAME_HEADER_WORDS + c(index as u32).raw() as usize] = *constant;
            }
            for index in crate::bytecode::RESERVED_REGISTER_COUNT..test_layout().registers_and_locals_count {
                machine.frame[FRAME_HEADER_WORDS + index as usize] = value::EMPTY;
            }
            machine.frame[3] = 1;
            machine.frame[FRAME_HEADER_WORDS + l(0).raw() as usize] = value::int32(10);
            machine.frame[FRAME_HEADER_WORDS + r(5).raw() as usize] = value::int32(5);
            let result = machine.run_at(&snapshot, &compiled, offset);
            assert_eq!(result, returned(value::int32(45)));
        }
    }

    #[test]
    fn osr_entries_whose_targets_reach_the_loop_header_over_back_edges() {
        // n = a0; sum = 0; i = 0; while (i < n) { sum += i; i++; } return sum;
        // with the condition at the end, so that the OSR entry at the
        // condition leads into the body and back to the loop header, which
        // the frame values meet in phis even for n, which the loop does not
        // assign.
        let program = assemble(|label| {
            vec![
                Instruction::Enter,
                Instruction::Mov { dst: l(1), src: a(0) },
                Instruction::Mov { dst: l(0), src: c(0) },
                Instruction::Mov { dst: r(5), src: c(0) },
                Instruction::Jump { target: label(7) },
                add(l(0), l(0), r(5)),
                Instruction::ExpRhsInt32 {
                    arith_feedback: 0,
                    dst: r(5),
                    lhs: r(5),
                    rhs: 1,
                },
                Instruction::JumpLessThanLoop {
                    arith_feedback: 0,
                    lhs: r(5),
                    rhs: l(1),
                    true_target: label(5),
                    false_target: label(8),
                },
                Instruction::Return { value: l(0) },
            ]
        });
        let (snapshot, compiled) = compile_program_with(&program, test_layout(), &[], |snapshot| {
            snapshot.options.osr_pc = Some(back_edge_pc(&program));
        });
        assert_eq!(compiled.osr_entries.len(), 1);
        let (pc, offset) = compiled.osr_entries[0];
        assert_eq!(pc, program.offsets[7]);
        // The interpreter ran five iterations.
        let mut machine = Machine::new(test_layout(), &[value::int32(10)]);
        for (index, constant) in snapshot.executables[0].constants.iter().enumerate() {
            machine.frame[FRAME_HEADER_WORDS + c(index as u32).raw() as usize] = *constant;
        }
        for index in crate::bytecode::RESERVED_REGISTER_COUNT..test_layout().registers_and_locals_count {
            machine.frame[FRAME_HEADER_WORDS + index as usize] = value::EMPTY;
        }
        machine.frame[3] = 1;
        machine.frame[FRAME_HEADER_WORDS + l(0).raw() as usize] = value::int32(10);
        machine.frame[FRAME_HEADER_WORDS + l(1).raw() as usize] = value::int32(10);
        machine.frame[FRAME_HEADER_WORDS + r(5).raw() as usize] = value::int32(5);
        let result = machine.run_at(&snapshot, &compiled, offset);
        assert_eq!(result, returned(value::int32(45)));
    }

    #[test]
    fn osr_entries_into_loops_bring_no_frame_fields() {
        // The function reads its lexical environment before the loop and in
        // it, which the OSR entry at the back edge does not know.
        let program = assemble(|label| {
            vec![
                Instruction::Enter,
                Instruction::GetLexicalEnvironment { dst: r(6) },
                Instruction::Mov { dst: r(5), src: c(0) },
                Instruction::Jump { target: label(6) },
                Instruction::GetLexicalEnvironment { dst: r(6) },
                Instruction::ExpRhsInt32 {
                    arith_feedback: 0,
                    dst: r(5),
                    lhs: r(5),
                    rhs: 1,
                },
                Instruction::JumpLessThanLoop {
                    arith_feedback: 0,
                    lhs: r(5),
                    rhs: a(0),
                    true_target: label(4),
                    false_target: label(7),
                },
                Instruction::Return { value: r(6) },
            ]
        });
        let (_, compiled) = compile_program_with(&program, test_layout(), &[], |snapshot| {
            snapshot.options.osr_pc = Some(back_edge_pc(&program));
        });
        assert_eq!(compiled.osr_entries.len(), 1);
    }

    #[test]
    fn osr_entries_in_loops_that_only_end_in_exits() {
        // sum = 0; i = 0; while (i < n) { if (n) { j = 0; do { sum += j; j++; } while (j < n); x = 0; } i++; }
        // return sum; compiled at the inner loop's back edge before the
        // function's entry and the inner loop's end ran. The outer loop is
        // built for the OSR entry inside it, but no entry reaches it, since
        // every path out of the inner loop exits.
        let program = assemble(|label| {
            vec![
                Instruction::Enter,
                Instruction::Mov { dst: l(0), src: c(0) },
                Instruction::Mov { dst: r(6), src: c(0) },
                Instruction::Jump { target: label(11) },
                Instruction::JumpIf {
                    condition: a(0),
                    true_target: label(5),
                    false_target: label(10),
                },
                Instruction::Mov { dst: r(5), src: c(0) },
                add(l(0), l(0), r(5)),
                Instruction::ExpRhsInt32 {
                    arith_feedback: 0,
                    dst: r(5),
                    lhs: r(5),
                    rhs: 1,
                },
                Instruction::JumpLessThanLoop {
                    arith_feedback: 0,
                    lhs: r(5),
                    rhs: a(0),
                    true_target: label(6),
                    false_target: label(9),
                },
                Instruction::Mov { dst: l(1), src: c(0) },
                Instruction::ExpRhsInt32 {
                    arith_feedback: 0,
                    dst: r(6),
                    lhs: r(6),
                    rhs: 1,
                },
                Instruction::JumpLessThanLoop {
                    arith_feedback: 0,
                    lhs: r(6),
                    rhs: a(0),
                    true_target: label(4),
                    false_target: label(12),
                },
                Instruction::Return { value: l(0) },
            ]
        });
        let (snapshot, compiled) = compile_program_with(&program, test_layout(), &[1, 9], |snapshot| {
            snapshot.options.osr_pc = Some(back_edge_pc(&program));
            snapshot.options.verify_ir = true;
        });
        assert_eq!(compiled.osr_entries.len(), 1);
        let (pc, offset) = compiled.osr_entries[0];
        assert_eq!(pc, program.offsets[8]);
        // The interpreter ran five iterations of the inner loop.
        let mut machine = Machine::new(test_layout(), &[value::int32(10)]);
        for (index, constant) in snapshot.executables[0].constants.iter().enumerate() {
            machine.frame[FRAME_HEADER_WORDS + c(index as u32).raw() as usize] = *constant;
        }
        for index in crate::bytecode::RESERVED_REGISTER_COUNT..test_layout().registers_and_locals_count {
            machine.frame[FRAME_HEADER_WORDS + index as usize] = value::EMPTY;
        }
        machine.frame[3] = 1;
        machine.frame[FRAME_HEADER_WORDS + l(0).raw() as usize] = value::int32(10);
        machine.frame[FRAME_HEADER_WORDS + r(5).raw() as usize] = value::int32(5);
        machine.frame[FRAME_HEADER_WORDS + r(6).raw() as usize] = value::int32(0);
        // The inner loop finishes, and exits where it ends.
        let result = machine.run_at(&snapshot, &compiled, offset);
        assert_eq!(result.status, RESUME);
        assert_eq!(machine.program_counter(), program.offsets[9]);
        assert_eq!(machine.slot(l(0)), value::int32(45));
    }

    #[test]
    fn truthiness_branches_use_the_fast_paths_and_the_helper() {
        let program = assemble(|label| {
            vec![
                Instruction::JumpIf {
                    condition: a(0),
                    true_target: label(1),
                    false_target: label(2),
                },
                Instruction::Return { value: c(1) },
                Instruction::Return { value: c(0) },
            ]
        });
        // Objects are truthy unless they are [[IsHTMLDDA]].
        let object = [0, 0u64];
        let htmldda_object = [0, u64::from(HTMLDDA_FLAG)];
        let object_tag = u64::from(value::OBJECT_TAG) << 48;
        // Strings are truthy unless they are empty, by their length.
        let string_tag = 0xFFFA_u64 << 48;
        let empty_string = [0, 0, 0, 0u64];
        let string = [0, 0, 3, 0u64];
        for (argument, expected) in [
            (value::TRUE, 10),
            (value::FALSE, 0),
            (value::int32(0), 0),
            (value::int32(-3), 10),
            (value::UNDEFINED, 0),
            (value::NULL, 0),
            (0x4000_0000_0000_0000, 10),
            (0.5f64.to_bits(), 10),
            ((-1.5f64).to_bits(), 10),
            (0.0f64.to_bits(), 0),
            ((-0.0f64).to_bits(), 0),
            (0x7FF8_0000_0000_0000, 0),
            (object_tag | object.as_ptr() as u64, 10),
            (object_tag | htmldda_object.as_ptr() as u64, 0),
            (string_tag | string.as_ptr() as u64, 10),
            (string_tag | empty_string.as_ptr() as u64, 0),
        ] {
            let (result, _) = run(&program, &[argument]);
            assert_eq!(result, returned(value::int32(expected)), "{argument:#x}");
        }
    }

    #[test]
    fn not_and_tag_branches() {
        let program = assemble(|label| {
            vec![
                Instruction::JumpNullish {
                    condition: a(0),
                    true_target: label(1),
                    false_target: label(2),
                },
                Instruction::Return { value: c(1) },
                Instruction::JumpUndefined {
                    condition: a(0),
                    true_target: label(1),
                    false_target: label(3),
                },
                Instruction::Not { dst: r(5), src: a(0) },
                Instruction::ToBoolean { dst: r(6), value: r(5) },
                Instruction::Return { value: r(6) },
            ]
        });
        for (argument, expected) in [
            (value::UNDEFINED, value::int32(10)),
            (value::NULL, value::int32(10)),
            (value::TRUE, value::FALSE),
            (value::int32(0), value::TRUE),
            (0x4000_0000_0000_0000, value::FALSE),
        ] {
            let (result, _) = run(&program, &[argument]);
            assert_eq!(result, returned(expected), "{argument:#x}");
        }
    }

    #[test]
    fn return_turns_empty_into_undefined_but_end_does_not() {
        let program = assemble(|_| vec![Instruction::Enter, Instruction::Return { value: r(5) }]);
        assert_eq!(run(&program, &[0]).0, returned(value::UNDEFINED));
        let program = assemble(|_| vec![Instruction::End { value: a(0) }]);
        assert_eq!(run(&program, &[value::EMPTY]).0, returned(value::EMPTY));
    }

    #[test]
    fn calls_and_values_live_across_them() {
        let program = assemble(|_| {
            vec![
                Instruction::Enter,
                Instruction::Mov { dst: r(6), src: a(0) },
                Instruction::Call {
                    value_feedback: 0,
                    call_feedback: 0,
                    dst: r(7),
                    callee: c(1),
                    this_value: c(0),
                    argument_count: 2,
                    expression_string: None,
                    arguments: vec![r(6), c(1)],
                },
                // r6 still holds the argument, which lived across the call.
                Instruction::IsCallable { dst: l(1), value: r(6) },
                add(l(0), r(7), r(6)),
                Instruction::Return { value: l(0) },
            ]
        });
        let (result, _) = run(&program, &[value::int32(7)]);
        // callee 10 + arguments 7 + 10, plus 7.
        assert_eq!(result, returned(value::int32(34)));
    }

    #[test]
    fn record_conventions() {
        let program = assemble(|_| {
            vec![
                Instruction::Enter,
                Instruction::ToLength { dst: r(5), value: a(0) },
                Instruction::ConcatString { dst: r(5), src: c(1) },
                Instruction::NewArray {
                    dst: r(6),
                    element_count: 3,
                    elements: vec![r(5), c(1), a(0)],
                },
                Instruction::Return { value: r(6) },
            ]
        });
        // NB: ToLength's fast path passes int32 values that are not negative
        //     through, so a negative one takes the slow path.
        let (result, machine) = run(&program, &[value::int32(-4)]);
        // ToLength doubles (-8), ConcatString adds 10 (2), NewArray sums 2 + 10 + -4.
        assert_eq!(result, returned(value::int32(8)));
        // NB: The outputs are values, which the frame never gets.
        assert_eq!(machine.slot(r(5)), value::EMPTY);
    }

    #[test]
    fn slow_paths_that_do_not_continue_return_to_the_interpreter() {
        let program = assemble(|_| {
            vec![
                Instruction::Enter,
                add(r(5), a(0), a(0)),
                add(r(6), r(5), r(5)),
                Instruction::Return { value: r(6) },
            ]
        });
        let add_pc = program.offsets[2];
        let run_failing = |control: i64| {
            run_with(&program, test_layout(), &[value::int32(1)], &[], |machine| {
                machine.state.borrow_mut().fail = Some((add_pc, control));
            })
        };
        // An exception propagating out of the executable.
        let (result, machine) = run_failing(-1);
        assert_eq!(result.status, EXIT_INTERPRETER);
        assert_eq!(machine.program_counter(), add_pc);
        // An exception caught by a handler at pc 4: the frame's pc stays as the slow path left it.
        let (result, machine) = run_failing(4);
        assert_eq!(result.status, RESUME);
        assert_eq!(machine.program_counter(), add_pc);
        // A continuation somewhere else in the frame.
        let (result, machine) = run_failing((CONTINUATION_BIT | 8) as i64);
        assert_eq!(result.status, RESUME);
        assert_eq!(machine.program_counter(), 8);
    }

    #[test]
    fn slow_paths_that_do_not_continue_write_their_frame_state() {
        // l0 is no operand of the first Exp, so compiled code keeps it out
        // of the frame unless the slow path leaves.
        let program = assemble(|_| {
            vec![
                Instruction::Enter,
                Instruction::Mov { dst: l(0), src: a(0) },
                add(r(5), a(0), a(0)),
                add(r(6), r(5), l(0)),
                Instruction::Return { value: r(6) },
            ]
        });
        let add_pc = program.offsets[2];
        let run_failing = |control: Option<i64>| {
            run_with(&program, test_layout(), &[value::int32(7)], &[], |machine| {
                machine.state.borrow_mut().fail = control.map(|control| (add_pc, control));
            })
        };
        let (result, _) = run_failing(None);
        assert_eq!(result, returned(value::int32(21)));
        // A continuation somewhere else in the frame, and an exception
        // caught by a handler.
        for control in [(CONTINUATION_BIT | 8) as i64, 4] {
            let (result, machine) = run_failing(Some(control));
            assert_eq!(result.status, RESUME);
            assert_eq!(machine.slot(l(0)), value::int32(7));
            assert!(machine.state.borrow().exits_taken.is_empty(), "leaves are no exits");
        }
        // An exception propagating out of the executable needs no frame state.
        let (result, machine) = run_failing(Some(-1));
        assert_eq!(result.status, EXIT_INTERPRETER);
        assert_ne!(machine.slot(l(0)), value::int32(7));
    }

    #[test]
    fn instructions_that_never_ran_exit_with_their_frame_state() {
        let program = assemble(|_| {
            vec![
                Instruction::Enter,
                Instruction::Mov { dst: r(5), src: c(1) },
                Instruction::Mov { dst: r(6), src: a(0) },
                Instruction::Mov { dst: l(0), src: r(6) },
                add(r(7), r(5), r(6)),
                Instruction::Return { value: l(0) },
            ]
        });
        let (result, machine) = run_with(&program, test_layout(), &[value::int32(42)], &[4], |_| {});
        assert_eq!(result.status, RESUME);
        assert_eq!(machine.state.borrow().exits_taken, [0]);
        assert_eq!(machine.program_counter(), program.offsets[4]);
        assert_eq!(machine.slot(r(5)), value::int32(10));
        assert_eq!(machine.slot(r(6)), value::int32(42));
        assert_eq!(machine.slot(l(0)), value::int32(42));
    }

    #[test]
    fn exit_values_survive_in_spill_slots_across_calls() {
        // r6 lives across a call in a spill slot, then the next instruction exits.
        let program = assemble(|_| {
            vec![
                Instruction::Enter,
                Instruction::Mov { dst: r(6), src: a(0) },
                Instruction::ConcatString { dst: r(5), src: c(1) },
                Instruction::Mov { dst: l(0), src: r(6) },
                Instruction::Mov { dst: l(1), src: r(5) },
                add(r(7), l(0), l(1)),
                Instruction::Return { value: r(7) },
            ]
        });
        let (result, machine) = run_with(&program, test_layout(), &[value::int32(5)], &[5], |_| {});
        assert_eq!(result.status, RESUME);
        assert_eq!(machine.slot(l(0)), value::int32(5));
        // Enter emptied r5, which the fake ConcatString treats as 0.
        assert_eq!(machine.slot(l(1)), value::int32(10));
    }

    #[test]
    fn frame_fields() {
        // Environments and objects are cells; the fake heap region starts at 0.
        let outer_private_environment = [0u64; 2];
        let private_environment = [outer_private_environment.as_ptr() as u64, 0];
        let lexical_environment = [0u64; 2];
        let function_object = [0, u64::from(FUNCTION_FLAG)];
        let plain_object = [0u64, 0];
        let object_tag = u64::from(value::OBJECT_TAG) << 48;
        let program = assemble(|_| {
            vec![
                Instruction::GetLexicalEnvironment { dst: r(5) },
                Instruction::LeavePrivateEnvironment,
                Instruction::IsCallable { dst: r(6), value: a(0) },
                Instruction::SetLexicalEnvironment { environment: c(0) },
                Instruction::IsCallable { dst: r(7), value: r(5) },
                Instruction::SetLexicalEnvironment { environment: r(5) },
                Instruction::Return { value: r(6) },
            ]
        });
        for (argument, expected) in [
            (function_object.as_ptr() as u64 | object_tag, value::TRUE),
            (plain_object.as_ptr() as u64 | object_tag, value::FALSE),
            (value::int32(1), value::FALSE),
        ] {
            let (result, machine) = run_with(&program, test_layout(), &[argument], &[], |machine| {
                machine.frame[1] = lexical_environment.as_ptr() as u64;
                machine.frame[2] = private_environment.as_ptr() as u64;
            });
            assert_eq!(result, returned(expected));
            assert_eq!(machine.frame[2], outer_private_environment.as_ptr() as u64);
            assert_eq!(machine.frame[1], lexical_environment.as_ptr() as u64);
        }
    }

    #[test]
    fn many_simultaneously_live_values() {
        // Ten arguments are loaded in reverse order for a slow path call that
        // reads all of them, so all ten are live at once.
        let layout = FrameLayout {
            number_of_registers: 16,
            registers_and_locals_count: 16,
            number_of_constants: 2,
            number_of_arguments: 10,
        };
        let register = |index: u32| Operand::from_raw(index);
        let argument = |index: u32| Operand::from_raw(layout.arguments_base() + index);
        let program = assemble(|_| {
            let mut instructions = vec![Instruction::Enter];
            for index in 0..10 {
                instructions.push(Instruction::Mov {
                    dst: register(5 + index),
                    src: argument(9 - index),
                });
            }
            instructions.push(Instruction::NewArray {
                dst: register(15),
                element_count: 10,
                elements: (5..15).map(register).collect(),
            });
            instructions.push(Instruction::Return { value: register(15) });
            instructions
        });
        let arguments = (0..10).map(|index| value::int32(100 + index)).collect::<Vec<_>>();
        let (result, machine) = run_with(&program, layout, &arguments, &[], |_| {});
        assert_eq!(result, returned(value::int32((100..110).sum())));
        // NB: The slow path got them as values, not from the frame.
        for index in 0..10 {
            assert_eq!(machine.slot(register(5 + index)), value::EMPTY);
        }
    }

    /// How a reference run of a program ended.
    #[derive(Debug, PartialEq, Eq)]
    enum ReferenceEnd {
        Returned(u64),
        /// Reached an instruction that never ran, with the frame slots then.
        Exited {
            pc: u32,
            slots: Vec<u64>,
        },
        /// Took too many steps.
        OutOfFuel,
    }

    fn truthy(bits: u64) -> bool {
        match (bits >> 48) as u16 {
            value::BOOLEAN_TAG => bits & 1 != 0,
            value::INT32_TAG => bits as u32 != 0,
            _ => to_boolean(bits) != 0,
        }
    }

    /// Objects for the reference interpreter's property accesses, which end
    /// in an exit wherever compiled code would exit.
    #[derive(Debug, Clone, Default, PartialEq, Eq)]
    struct ReferenceHeap {
        /// Address, shape and named property values of each object.
        objects: Vec<(u64, u64, Vec<u64>)>,
        /// The (shape, offset) entries of each property cache.
        caches: Vec<Vec<(u64, u32)>>,
        /// Every call flips the shape of the first object between these.
        call_toggles: Option<(u64, u64)>,
    }

    impl ReferenceHeap {
        /// The object and property offset `value` accesses through `cache`,
        /// or `None` where compiled code exits.
        fn access(&self, value: u64, cache: u32) -> Option<(usize, usize)> {
            if (value >> 48) as u16 != value::OBJECT_TAG {
                return None;
            }
            let address = value & ((1 << 48) - 1);
            let object = self.objects.iter().position(|(object, _, _)| *object == address)?;
            let shape = self.objects[object].1;
            let (_, offset) = self.caches[cache as usize]
                .iter()
                .find(|(cached, _)| *cached == shape)?;
            Some((object, *offset as usize))
        }
    }

    /// `Add` like its fast path for numbers, and like the fake slow path
    /// otherwise.
    fn reference_add(lhs: u64, rhs: u64) -> u64 {
        if is_int32(lhs) && is_int32(rhs) {
            return match int(lhs).checked_add(int(rhs)) {
                Some(sum) => value::int32(sum),
                None => (f64::from(int(lhs)) + f64::from(int(rhs))).to_bits(),
            };
        }
        match (fast_path_number(lhs), fast_path_number(rhs)) {
            (Some(lhs), Some(rhs)) => box_number(lhs + rhs),
            _ => value::int32(int(lhs).wrapping_add(int(rhs))),
        }
    }

    /// `Increment` like its fast path for int32 values, and like the fake
    /// slow path otherwise.
    fn reference_increment(value: u64) -> u64 {
        if !is_int32(value) {
            return value::int32(int(value).wrapping_add(1));
        }
        match int(value).checked_add(1) {
            Some(result) => value::int32(result),
            None => (f64::from(int(value)) + 1.0).to_bits(),
        }
    }

    /// Runs `program` the way the interpreter would with the fake slow paths
    /// of these tests, starting from `machine`'s frame.
    fn run_reference(program: &Program, machine: &Machine, constants: &[u64], never_ran: &[u32]) -> ReferenceEnd {
        run_reference_with_heap(program, machine, constants, never_ran, &mut ReferenceHeap::default())
    }

    fn run_reference_with_heap(
        program: &Program,
        machine: &Machine,
        constants: &[u64],
        never_ran: &[u32],
        heap: &mut ReferenceHeap,
    ) -> ReferenceEnd {
        run_reference_from(program, machine, 0, constants, never_ran, heap)
    }

    /// Runs `program` in the reference interpreter from `pc` on, with the
    /// slots and environment of `machine`'s frame.
    fn run_reference_from(
        program: &Program,
        machine: &Machine,
        mut pc: u32,
        constants: &[u64],
        never_ran: &[u32],
        heap: &mut ReferenceHeap,
    ) -> ReferenceEnd {
        let layout = machine.layout;
        let mut slots = machine.frame[FRAME_HEADER_WORDS..].to_vec();
        let mut environment = machine.frame[1];
        let mask = (1u64 << 48) - 1;
        for _ in 0..5000 {
            if never_ran.contains(&pc) {
                return ReferenceEnd::Exited { pc, slots };
            }
            let decoded = decode_instruction(&program.bytes, pc).unwrap();
            let read = |slots: &[u64], operand: Operand| slots[operand.raw() as usize];
            let mut next_pc = decoded.next_pc();
            match decoded.instruction {
                Instruction::Enter => {
                    slots[5..layout.registers_and_locals_count as usize].fill(value::EMPTY);
                    for (index, constant) in constants.iter().enumerate() {
                        slots[layout.constants_base() as usize + index] = *constant;
                    }
                }
                Instruction::Mov { dst, src } => slots[dst.raw() as usize] = read(&slots, src),
                Instruction::Mov2 {
                    c0_dst,
                    c0_src,
                    c1_dst,
                    c1_src,
                } => {
                    slots[c0_dst.raw() as usize] = read(&slots, c0_src);
                    slots[c1_dst.raw() as usize] = read(&slots, c1_src);
                }
                Instruction::MovSrcUndefined { dst } => slots[dst.raw() as usize] = value::UNDEFINED,
                Instruction::Add { dst, lhs, rhs, .. } => {
                    slots[dst.raw() as usize] = reference_add(read(&slots, lhs), read(&slots, rhs));
                }
                Instruction::Increment { dst, .. } => {
                    slots[dst.raw() as usize] = reference_increment(read(&slots, dst));
                }
                Instruction::Not { dst, src } => {
                    let result = if truthy(read(&slots, src)) {
                        value::FALSE
                    } else {
                        value::TRUE
                    };
                    slots[dst.raw() as usize] = result;
                }
                Instruction::ToBoolean { dst, value } => {
                    let result = if truthy(read(&slots, value)) {
                        value::TRUE
                    } else {
                        value::FALSE
                    };
                    slots[dst.raw() as usize] = result;
                }
                Instruction::GetLexicalEnvironment { dst } => {
                    slots[dst.raw() as usize] = (environment & mask) | (0xFFF8 << 48);
                }
                Instruction::SetLexicalEnvironment { environment: source } => {
                    environment = read(&slots, source) & mask;
                }
                Instruction::IsCallable { dst, .. } => {
                    // The only objects are the fake ones, which are not functions.
                    slots[dst.raw() as usize] = value::FALSE;
                }
                Instruction::Call {
                    dst, callee, arguments, ..
                } => {
                    let mut result = int(read(&slots, callee));
                    for argument in arguments {
                        result = result.wrapping_add(int(read(&slots, argument)));
                    }
                    slots[dst.raw() as usize] = value::int32(result);
                    if let Some((first, second)) = heap.call_toggles {
                        let shape = &mut heap.objects[0].1;
                        *shape = if *shape == first { second } else { first };
                    }
                }
                Instruction::GetById { dst, base, cache, .. } => {
                    let Some((object, offset)) = heap.access(read(&slots, base), cache) else {
                        return ReferenceEnd::Exited { pc, slots };
                    };
                    slots[dst.raw() as usize] = heap.objects[object].2[offset];
                }
                Instruction::PutById { base, src, cache, .. } => {
                    let Some((object, offset)) = heap.access(read(&slots, base), cache) else {
                        return ReferenceEnd::Exited { pc, slots };
                    };
                    heap.objects[object].2[offset] = read(&slots, src);
                }
                Instruction::Return { value } => {
                    let value = read(&slots, value);
                    return ReferenceEnd::Returned(if value == value::EMPTY { value::UNDEFINED } else { value });
                }
                Instruction::Jump { target } => next_pc = target.0,
                Instruction::JumpIf {
                    condition,
                    true_target,
                    false_target,
                } => {
                    next_pc = if truthy(read(&slots, condition)) {
                        true_target.0
                    } else {
                        false_target.0
                    };
                }
                Instruction::JumpTrue { condition, target } => {
                    if truthy(read(&slots, condition)) {
                        next_pc = target.0;
                    }
                }
                Instruction::JumpFalse { condition, target } => {
                    if !truthy(read(&slots, condition)) {
                        next_pc = target.0;
                    }
                }
                Instruction::JumpNullish {
                    condition,
                    true_target,
                    false_target,
                } => {
                    let tag = (read(&slots, condition) >> 48) as u16;
                    let nullish = tag == value::UNDEFINED_TAG || tag == value::NULL_TAG;
                    next_pc = if nullish { true_target.0 } else { false_target.0 };
                }
                Instruction::JumpLessThan {
                    lhs,
                    rhs,
                    true_target,
                    false_target,
                    ..
                } => {
                    let (lhs, rhs) = (read(&slots, lhs), read(&slots, rhs));
                    // The fast path compares numbers; the fake slow path int32 payloads.
                    let less = match (fast_path_number(lhs), fast_path_number(rhs)) {
                        (Some(lhs), Some(rhs)) => lhs < rhs,
                        _ => int(lhs) < int(rhs),
                    };
                    next_pc = if less { true_target.0 } else { false_target.0 };
                }
                other => panic!("the reference interpreter does not know {other:?}"),
            }
            pc = next_pc;
        }
        ReferenceEnd::OutOfFuel
    }

    #[test]
    fn random_structured_programs_match_a_reference_interpreter() {
        use crate::regalloc::tests::ProgramGenerator;
        use crate::regalloc::tests::Random;

        let layout = FrameLayout {
            number_of_registers: 12,
            registers_and_locals_count: 16,
            number_of_constants: 2,
            number_of_arguments: 2,
        };
        let mut checked = 0;
        let mut exits = 0;
        for seed in 1..=1500u64 {
            let program = ProgramGenerator::generate(seed.wrapping_mul(0x9E37_79B9_7F4A_7C15), layout);
            let mut random = Random(seed);
            let never_ran = (0..program.offsets.len())
                .filter(|index| *index != 0 && random.chance(3))
                .collect::<Vec<_>>();
            let never_ran_pcs = never_ran
                .iter()
                .map(|index| program.offsets[*index])
                .collect::<Vec<_>>();
            let arguments = [value::int32(random.below(5) as i32), value::int32(3)];
            let (snapshot, compiled) = compile_program(&program, layout, &never_ran);

            let mut machine = Machine::new(layout, &arguments);
            let environment = [0u64; 2];
            machine.frame[1] = environment.as_ptr() as u64;
            let expected = run_reference(&program, &machine, &snapshot.executables[0].constants, &never_ran_pcs);
            if expected == ReferenceEnd::OutOfFuel {
                continue;
            }
            let result = machine.run(&snapshot, &compiled);
            match expected {
                ReferenceEnd::Returned(value) => {
                    assert_eq!(result, returned(value), "seed {seed}");
                }
                ReferenceEnd::Exited { pc, slots } => {
                    exits += 1;
                    assert_eq!(result.status, RESUME, "seed {seed}");
                    assert_eq!(machine.program_counter(), pc, "seed {seed}");
                    // Every slot the interpreter can still read must be right.
                    let instructions = crate::bytecode::decode_all(&program.bytes).unwrap();
                    let cfg = crate::bytecode::cfg::Cfg::new(&instructions, &[], &layout).unwrap();
                    let liveness = crate::bytecode::liveness::Liveness::compute(&instructions, &cfg, &layout);
                    let index = instructions
                        .iter()
                        .position(|instruction| instruction.pc == pc)
                        .unwrap();
                    for slot in liveness.live_in(index).iter() {
                        let operand = layout.operand_for_tracked_index(slot);
                        assert_eq!(
                            machine.slot(operand),
                            slots[operand.raw() as usize],
                            "seed {seed}: slot {} at pc {pc}",
                            operand.raw()
                        );
                    }
                }
                ReferenceEnd::OutOfFuel => unreachable!(),
            }
            checked += 1;
        }
        assert!(checked > 1000, "only {checked} programs terminated");
        assert!(exits > 100, "only {exits} programs exited");
    }

    #[test]
    fn random_programs_with_property_access_match_a_reference_interpreter() {
        use crate::builder::tests::property_access::cache;
        use crate::builder::tests::property_access::own;
        use crate::regalloc::tests::ProgramGenerator;
        use crate::regalloc::tests::Random;
        use crate::snapshot::PropertyCacheEntryType::ChangeOwnProperty;
        use crate::snapshot::PropertyCacheEntryType::GetOwnProperty;

        let layout = FrameLayout {
            number_of_registers: 12,
            registers_and_locals_count: 16,
            number_of_constants: 2,
            number_of_arguments: 2,
        };
        // Three shapes; the caches know the first two.
        let shapes = [[0u64; 1], [0u64; 1], [0u64; 1]];
        let shape = |index: usize| shapes[index].as_ptr() as u64;
        let cache_entries = [
            vec![(shape(0), 0), (shape(1), 0)],
            vec![(shape(0), 1), (shape(1), 2)],
            vec![(shape(0), 1), (shape(1), 2)],
            vec![(shape(0), 0), (shape(1), 0)],
        ];
        let caches = cache_entries
            .iter()
            .enumerate()
            .map(|(index, entries)| {
                let kind = if index < 2 { GetOwnProperty } else { ChangeOwnProperty };
                cache(
                    entries
                        .iter()
                        .map(|(shape, offset)| own(kind, *shape, *offset))
                        .collect(),
                )
            })
            .collect::<Vec<_>>();

        let mut checked = 0;
        let mut exits = 0;
        for seed in 1..=1500u64 {
            let program = ProgramGenerator::generate_with(seed.wrapping_mul(0x9E37_79B9_7F4A_7C15), layout, true);
            let mut random = Random(seed);
            let toggles = random.chance(50);
            // Objects: [shape word, flags, storage pointer] and their storage.
            let mut storages = (0..3)
                .map(|index| {
                    (0..3)
                        .map(|offset| value::int32(10 * index + offset))
                        .collect::<Vec<_>>()
                })
                .collect::<Vec<_>>();
            let mut objects = (0..3)
                .map(|index| [shape(index), 0, storages[index].as_mut_ptr() as u64])
                .collect::<Vec<_>>();
            let object_value = |objects: &[[u64; 3]], index: usize| {
                (u64::from(value::OBJECT_TAG) << 48) | objects[index].as_ptr() as u64
            };
            let mut argument = || match random.below(4) {
                3 => value::int32(1),
                index => object_value(&objects, index),
            };
            let arguments = [argument(), argument()];

            let (snapshot, compiled) = compile_program_with(&program, layout, &[], |snapshot| {
                snapshot.executables[0].property_caches = caches.clone();
            });
            let mut machine = Machine::new(layout, &arguments);
            let environment = [0u64; 2];
            machine.frame[1] = environment.as_ptr() as u64;
            let mut heap = ReferenceHeap {
                objects: (0..3)
                    .map(|index| (objects[index].as_ptr() as u64, shape(index), storages[index].clone()))
                    .collect(),
                caches: cache_entries.to_vec(),
                // Calls change the first object to a shape the caches do not
                // know, so code relying on stale shape facts would misbehave.
                call_toggles: toggles.then(|| (shape(0), shape(2))),
            };
            let expected =
                run_reference_with_heap(&program, &machine, &snapshot.executables[0].constants, &[], &mut heap);
            if expected == ReferenceEnd::OutOfFuel {
                continue;
            }
            if toggles {
                machine.state.borrow_mut().call_toggles = Some((objects[0].as_mut_ptr(), shape(0), shape(2)));
            }
            let result = machine.run(&snapshot, &compiled);
            // NB: Compiled code may exit before the reference interpreter
            //     does, where checks moved up (to loop preheaders, or to
            //     where values enter merges). The interpreter then continues
            //     from there, and so does the reference interpreter here,
            //     with the heap the compiled code left.
            let exited_early = match &expected {
                ReferenceEnd::Exited { pc, .. } => result.status == RESUME && machine.program_counter() != *pc,
                ReferenceEnd::Returned(_) => result.status == RESUME,
                ReferenceEnd::OutOfFuel => false,
            };
            if exited_early {
                let mut resumed_heap = ReferenceHeap {
                    objects: (0..3)
                        .map(|index| {
                            (
                                objects[index].as_ptr() as u64,
                                objects[index][0],
                                storages[index].clone(),
                            )
                        })
                        .collect(),
                    caches: heap.caches.clone(),
                    call_toggles: heap.call_toggles,
                };
                let resumed = run_reference_from(
                    &program,
                    &machine,
                    machine.program_counter(),
                    &snapshot.executables[0].constants,
                    &[],
                    &mut resumed_heap,
                );
                match (&expected, resumed) {
                    (ReferenceEnd::Returned(value), ReferenceEnd::Returned(resumed)) => {
                        assert_eq!(resumed, *value, "seed {seed}: resumed result");
                    }
                    (
                        ReferenceEnd::Exited { pc, slots },
                        ReferenceEnd::Exited {
                            pc: resumed_pc,
                            slots: resumed_slots,
                        },
                    ) => {
                        assert_eq!(resumed_pc, *pc, "seed {seed}: resumed exit");
                        let instructions = crate::bytecode::decode_all(&program.bytes).unwrap();
                        let cfg = crate::bytecode::cfg::Cfg::new(&instructions, &[], &layout).unwrap();
                        let liveness = crate::bytecode::liveness::Liveness::compute(&instructions, &cfg, &layout);
                        let index = instructions
                            .iter()
                            .position(|instruction| instruction.pc == *pc)
                            .unwrap();
                        for slot in liveness.live_in(index).iter() {
                            let slot = layout.operand_for_tracked_index(slot).raw() as usize;
                            assert_eq!(resumed_slots[slot], slots[slot], "seed {seed}: slot {slot} at pc {pc}");
                        }
                    }
                    (expected, resumed) => panic!(
                        "seed {seed}: resumed at {} and ended as {resumed:?}, not as {expected:?}",
                        machine.program_counter()
                    ),
                }
                assert_eq!(resumed_heap.objects, heap.objects, "seed {seed}: heap");
                exits += 1;
                checked += 1;
                continue;
            }
            match expected {
                ReferenceEnd::Returned(value) => assert_eq!(result, returned(value), "seed {seed}"),
                ReferenceEnd::Exited { pc, slots } => {
                    exits += 1;
                    assert_eq!(result.status, RESUME, "seed {seed}");
                    assert_eq!(machine.program_counter(), pc, "seed {seed}");
                    let instructions = crate::bytecode::decode_all(&program.bytes).unwrap();
                    let cfg = crate::bytecode::cfg::Cfg::new(&instructions, &[], &layout).unwrap();
                    let liveness = crate::bytecode::liveness::Liveness::compute(&instructions, &cfg, &layout);
                    let index = instructions
                        .iter()
                        .position(|instruction| instruction.pc == pc)
                        .unwrap();
                    for slot in liveness.live_in(index).iter() {
                        let operand = layout.operand_for_tracked_index(slot);
                        assert_eq!(
                            machine.slot(operand),
                            slots[operand.raw() as usize],
                            "seed {seed}: slot {} at pc {pc}",
                            operand.raw()
                        );
                    }
                }
                ReferenceEnd::OutOfFuel => unreachable!(),
            }
            for (index, (_, shape, values)) in heap.objects.iter().enumerate() {
                assert_eq!(&storages[index], values, "seed {seed}: object {index}");
                assert_eq!(objects[index][0], *shape, "seed {seed}: object {index}");
            }
            checked += 1;
        }
        assert!(checked > 1000, "only {checked} programs terminated");
        assert!(exits > 300, "only {exits} programs exited");
    }

    #[test]
    fn stack_overflow_at_entry_resumes_in_the_interpreter() {
        let program = assemble(|_| vec![Instruction::Enter, Instruction::Return { value: a(0) }]);
        let (result, machine) = run_with(&program, test_layout(), &[value::int32(1)], &[], |machine| {
            machine.vm[1] = u64::MAX - 1;
        });
        assert_eq!(result.status, RESUME);
        assert_eq!(machine.frame[3], 0, "the frame was not initialized");
    }
}

#[test]
fn jumps_skip_blocks_that_only_jump_on() {
    // The builder splits the edges of a truthiness branch into blocks of their own, which only jump on.
    let program = assemble(|label| {
        vec![
            Instruction::JumpIf {
                condition: a(0),
                true_target: label(1),
                false_target: label(2),
            },
            Instruction::Return { value: c(1) },
            Instruction::Return { value: c(0) },
        ]
    });
    let mut snapshot = crate::builder::tests::snapshot_for(&program, test_layout());
    snapshot.runtime = runtime();
    let compiled = compile_for::<crate::asm::x86_64::MacroAssembler>(&snapshot, &|_| true).unwrap();
    let listing = crate::asm::disassembler::listing_with_data(
        crate::asm::Architecture::X86_64,
        &compiled.code,
        compiled.data_offset as usize,
    );
    let target_of = |text: &str| {
        let (mnemonic, operand) = text.split_once(' ')?;
        mnemonic
            .starts_with('j')
            .then(|| usize::from_str_radix(operand.strip_prefix("0x")?, 16).ok())?
    };
    for instruction in &listing {
        let Some(target) = target_of(&instruction.text) else {
            continue;
        };
        let landing = listing
            .iter()
            .find(|candidate| candidate.offset == target)
            .unwrap_or_else(|| panic!("no instruction at {target:#x}"));
        assert!(
            !landing.text.starts_with("jmp "),
            "{} at {:#x} jumps to a jump: {}",
            instruction.text,
            instruction.offset,
            landing.text
        );
    }
}

#[test]
fn aarch64_code_disassembles() {
    let program = assemble(|label| {
        vec![
            Instruction::Enter,
            Instruction::Mov { dst: l(0), src: c(0) },
            Instruction::JumpLessThan {
                arith_feedback: 0,
                lhs: l(0),
                rhs: a(0),
                true_target: label(3),
                false_target: label(6),
            },
            Instruction::NewArray {
                dst: r(6),
                element_count: 2,
                elements: vec![l(0), a(0)],
            },
            Instruction::JumpIf {
                condition: r(6),
                true_target: label(5),
                false_target: label(6),
            },
            Instruction::Jump { target: label(2) },
            Instruction::Return { value: l(0) },
        ]
    });
    let mut snapshot = crate::builder::tests::snapshot_for(&program, test_layout());
    snapshot.runtime = runtime();
    let never_ran = program.offsets[5];
    let compiled = compile_for::<crate::asm::aarch64::MacroAssembler>(&snapshot, &|pc| pc != never_ran).unwrap();
    let lines = crate::asm::disassembler::aarch64(&compiled.code[..compiled.data_offset as usize]);
    let text = lines.join("\n");
    // The prologue links the frame, slow paths and helpers are called through registers.
    assert!(lines[0].starts_with("stp x29, x30, [sp"), "{text}");
    assert!(text.contains("blr"), "{text}");
    assert!(text.contains("ret"), "{text}");
    // NB: The slow paths of JumpLessThan and NewArray leave through sites.
    let kinds = compiled.sites.iter().map(|site| site.kind).collect::<Vec<_>>();
    assert_eq!(
        kinds,
        [SiteKind::Exit(ExitKind::NoFeedback), SiteKind::Leave, SiteKind::Leave]
    );

    let compiled = compile_for::<crate::asm::x86_64::MacroAssembler>(&snapshot, &|pc| pc != never_ran).unwrap();
    let lines = crate::asm::disassembler::x86_64(&compiled.code[..compiled.data_offset as usize]);
    assert_eq!(lines[0], "push rbp");

    // The dump names exits, slow paths and the shared tails on both architectures.
    snapshot.options.dump_asm = true;
    let x86_64 = compile_for::<crate::asm::x86_64::MacroAssembler>(&snapshot, &|pc| pc != never_ran).unwrap();
    let aarch64 = compile_for::<crate::asm::aarch64::MacroAssembler>(&snapshot, &|pc| pc != never_ran).unwrap();
    for dump in [x86_64.dump.unwrap(), aarch64.dump.unwrap()] {
        assert!(dump.contains("Exit NoFeedback [fs"), "{dump}");
        assert!(dump.contains("exit #0 (NoFeedback) of v"), "{dump}");
        assert!(dump.contains("exit stub (shared by all exits):\n"), "{dump}");
        assert!(dump.contains("; asm_slow_path_jump_less_than_values\n"), "{dump}");
        assert!(dump.contains("; libjs_jit_exit\n"), "{dump}");
        assert!(dump.contains("leave stub (shared by all leaves):\n"), "{dump}");
        assert!(
            dump.contains(", 1 exits, 2 leaves, 0 publish sites, 0 call sites\n"),
            "{dump}"
        );
    }
}

#[test]
fn dumps_ir_and_code_on_request() {
    let program = assemble(|_| vec![Instruction::Enter, Instruction::Return { value: a(0) }]);
    let mut snapshot = crate::builder::tests::snapshot_for(&program, test_layout());
    snapshot.runtime = runtime();
    snapshot.options.dump_ir = true;
    snapshot.options.dump_asm = true;
    let compiled = compile_for::<crate::asm::MacroAssembler>(&snapshot, &|_| true).unwrap();
    let dump = compiled.dump.unwrap();
    assert!(dump.contains("LoadSlot a0"), "{dump}");
    assert!(dump.contains("prologue:"), "{dump}");
    assert!(dump.contains("Return (v"), "{dump}");
    assert!(dump.contains(" ret\n"), "{dump}");
}

#[test]
fn missing_slow_paths_fail_compilation_cleanly() {
    let program = assemble(|_| {
        vec![
            Instruction::Mod {
                arith_feedback: 0,
                dst: r(5),
                lhs: a(0),
                rhs: a(0),
            },
            Instruction::Return { value: r(5) },
        ]
    });
    let mut snapshot = crate::builder::tests::snapshot_for(&program, test_layout());
    snapshot.runtime = runtime();
    let failure = compile_for::<crate::asm::MacroAssembler>(&snapshot, &|_| true).unwrap_err();
    assert_eq!(
        failure,
        CompileFailure::MissingRuntimeHelper {
            opcode: Some(OpCode::Mod)
        }
    );
}

/// Property access against fake objects: `[shape, flags, named property
/// storage]`, shapes `[dictionary generation]` and validity cells `[valid]`,
/// all in a heap region starting at address 0.
#[cfg(target_arch = "x86_64")]
mod property_access {
    use super::*;
    use crate::builder::tests::property_access::cache;
    use crate::builder::tests::property_access::get_by_id;
    use crate::builder::tests::property_access::own;
    use crate::builder::tests::property_access::put_by_id;
    use crate::code::ExitKind;
    use crate::snapshot::CellId;
    use crate::snapshot::PropertyCacheEntryType::ChangeOwnProperty;
    use crate::snapshot::PropertyCacheEntryType::GetOwnProperty;
    use crate::snapshot::PropertyCacheEntryType::GetPropertyInPrototypeChain;
    use crate::snapshot::PropertyCacheSnapshot;

    struct Cell(Box<[u64; 3]>);

    impl Cell {
        fn new(words: [u64; 3]) -> Self {
            Self(Box::new(words))
        }

        fn address(&self) -> u64 {
            self.0.as_ptr() as u64
        }

        fn id(&self) -> CellId {
            CellId(self.address())
        }
    }

    struct Object {
        cell: Cell,
        storage: Box<[u64]>,
    }

    impl Object {
        fn new(shape: &Cell, values: &[u64]) -> Self {
            let storage = values.to_vec().into_boxed_slice();
            let cell = Cell::new([shape.address(), 0, storage.as_ptr() as u64]);
            Self { cell, storage }
        }

        fn value(&self) -> u64 {
            (u64::from(value::OBJECT_TAG) << 48) | self.cell.address()
        }

        fn shape_word(&mut self) -> *mut u64 {
            self.cell.0.as_mut_ptr()
        }
    }

    fn accessor() -> u64 {
        u64::from(value::ACCESSOR_TAG) << 48
    }

    fn run_with_caches(
        program: &Program,
        caches: Vec<PropertyCacheSnapshot>,
        argument: u64,
        call_writes: Option<(*mut u64, u64)>,
    ) -> (JitResult, Machine, CompiledCode) {
        let layout = test_layout();
        let (snapshot, compiled) = compile_program_with(program, layout, &[], |snapshot| {
            snapshot.executables[0].property_caches = caches;
        });
        let mut machine = Machine::new(layout, &[argument]);
        machine.state.borrow_mut().call_writes = call_writes;
        let result = machine.run(&snapshot, &compiled);
        (result, machine, compiled)
    }

    fn exit_kinds_taken(machine: &Machine, compiled: &CompiledCode) -> Vec<ExitKind> {
        let taken = machine.state.borrow().exits_taken.clone();
        taken
            .iter()
            .map(|index| match compiled.sites[*index as usize].kind {
                SiteKind::Exit(kind) => kind,
                kind => panic!("{kind:?} is no exit"),
            })
            .collect()
    }

    #[test]
    fn monomorphic_gets_hit_and_exit_on_misses() {
        let program = assemble(|_| {
            vec![
                Instruction::Enter,
                Instruction::Mov { dst: r(6), src: c(1) },
                get_by_id(r(5), a(0), 0),
                add(r(7), r(5), r(6)),
                Instruction::Return { value: r(7) },
            ]
        });
        // The entry records generation 0, but the shape is not a dictionary,
        // so its generation word is never checked.
        let shape = Cell::new([5, 0, 0]);
        let other_shape = Cell::new([0; 3]);
        let caches = || vec![cache(vec![own(GetOwnProperty, shape.address(), 1)])];
        let get_pc = program.offsets[2];

        let hit = Object::new(&shape, &[0, value::int32(5)]);
        let (result, _, compiled) = run_with_caches(&program, caches(), hit.value(), None);
        assert_eq!(result, returned(value::int32(15)));
        assert_eq!(compiled.embedded_cells, [shape.id()]);

        let miss = Object::new(&other_shape, &[0, value::int32(5)]);
        let (result, machine, compiled) = run_with_caches(&program, caches(), miss.value(), None);
        assert_eq!(result.status, RESUME);
        assert_eq!(exit_kinds_taken(&machine, &compiled), [ExitKind::BadShape]);
        assert_eq!(machine.program_counter(), get_pc);
        assert_eq!(machine.slot(r(6)), value::int32(10), "the exit wrote back the live Mov");

        let (result, machine, compiled) = run_with_caches(&program, caches(), value::int32(3), None);
        assert_eq!(result.status, RESUME);
        assert_eq!(exit_kinds_taken(&machine, &compiled), [ExitKind::NotObject]);

        let holds_accessor = Object::new(&shape, &[0, accessor()]);
        let (result, machine, compiled) = run_with_caches(&program, caches(), holds_accessor.value(), None);
        assert_eq!(result.status, RESUME);
        assert_eq!(exit_kinds_taken(&machine, &compiled), [ExitKind::BadShape]);
        assert_eq!(machine.program_counter(), get_pc);
    }

    #[test]
    fn polymorphic_gets_dispatch_on_the_shape() {
        let program = assemble(|_| vec![get_by_id(r(5), a(0), 0), Instruction::Return { value: r(5) }]);
        let shapes = [Cell::new([0; 3]), Cell::new([0; 3]), Cell::new([0; 3])];
        let caches = || {
            vec![cache(vec![
                own(GetOwnProperty, shapes[0].address(), 0),
                own(GetOwnProperty, shapes[1].address(), 2),
            ])]
        };
        let first = Object::new(&shapes[0], &[value::int32(1), 0, 0]);
        let second = Object::new(&shapes[1], &[0, 0, value::int32(2)]);
        let third = Object::new(&shapes[2], &[0, 0, 0]);
        assert_eq!(
            run_with_caches(&program, caches(), first.value(), None).0,
            returned(value::int32(1))
        );
        assert_eq!(
            run_with_caches(&program, caches(), second.value(), None).0,
            returned(value::int32(2))
        );
        let (result, machine, compiled) = run_with_caches(&program, caches(), third.value(), None);
        assert_eq!(result.status, RESUME);
        assert_eq!(exit_kinds_taken(&machine, &compiled), [ExitKind::BadShape]);
    }

    #[test]
    fn prototype_chain_gets_and_dictionary_shapes() {
        let program = assemble(|_| vec![get_by_id(r(5), a(0), 0), Instruction::Return { value: r(5) }]);
        let receiver_shape = Cell::new([7, 0, 0]);
        let holder_shape = Cell::new([0; 3]);
        let holder = Object::new(&holder_shape, &[0, value::int32(42)]);
        let mut validity = Cell::new([1, 0, 0]);
        let mut entry = own(GetPropertyInPrototypeChain, receiver_shape.address(), 1);
        entry.prototype = Some(holder.cell.id());
        entry.prototype_chain_validity = Some(validity.id());
        entry.shape_dictionary_generation = 7;
        entry.shape_is_dictionary = true;
        let receiver = Object::new(&receiver_shape, &[]);

        let (result, _, compiled) = run_with_caches(&program, vec![cache(vec![entry])], receiver.value(), None);
        assert_eq!(result, returned(value::int32(42)));
        assert_eq!(compiled.embedded_cells.len(), 3);

        // A changed dictionary generation means the shape changed in place.
        let changed_receiver_shape = Cell::new([8, 0, 0]);
        let mut changed_entry = entry;
        changed_entry.shape = Some(changed_receiver_shape.id());
        let changed = Object::new(&changed_receiver_shape, &[]);
        let (result, machine, compiled) =
            run_with_caches(&program, vec![cache(vec![changed_entry])], changed.value(), None);
        assert_eq!(result.status, RESUME);
        assert_eq!(exit_kinds_taken(&machine, &compiled), [ExitKind::BadShape]);

        validity.0[0] = 0;
        let (result, machine, compiled) = run_with_caches(&program, vec![cache(vec![entry])], receiver.value(), None);
        assert_eq!(result.status, RESUME);
        assert_eq!(exit_kinds_taken(&machine, &compiled), [ExitKind::BadShape]);
    }

    /// `Array.prototype.push` against fake arrays: `[shape, flags, named
    /// property storage, elements, storage kind (byte 0) and size (bytes
    /// 4-7), length writable (byte 0) and proxy target (byte 1)]`, with
    /// shapes `[dictionary generation, prototype]`.
    mod array_push {
        use super::*;
        use crate::snapshot::Intrinsic;

        const EXTENSIBLE: u64 = 1;
        const MAGICAL_LENGTH: u64 = 4;
        const MAY_INTERFERE: u16 = 16;
        const PACKED: u64 = 1;

        /// The fake growing helper, which leaves the value in the array's
        /// unused named property storage word.
        extern "C" fn grow(array: *mut u64, value: u64) -> u64 {
            // SAFETY: Compiled code passes a fake array of six words.
            unsafe { *array.add(2) = value };
            value::int32(99)
        }

        struct FakeObject(Box<[u64; 6]>);

        impl FakeObject {
            fn new(shape: &Cell, flags: u64, elements: &[u64], size: u32) -> Self {
                Self(Box::new([
                    shape.address(),
                    flags,
                    0,
                    elements.as_ptr() as u64,
                    PACKED | u64::from(size) << 32,
                    1,
                ]))
            }

            fn address(&self) -> u64 {
                self.0.as_ptr() as u64
            }

            fn value(&self) -> u64 {
                (u64::from(value::OBJECT_TAG) << 48) | self.address()
            }
        }

        struct World {
            array_shape: Cell,
            array_prototype: FakeObject,
            object_prototype: FakeObject,
            validity: Cell,
            push: Cell,
            /// `%Array.prototype%`'s named properties: `push`.
            _methods: Box<[u64]>,
            _shapes: [Cell; 2],
        }

        impl World {
            fn new() -> Self {
                let object_prototype_shape = Cell::new([0, 0, 0]);
                let object_prototype = FakeObject::new(&object_prototype_shape, EXTENSIBLE, &[], 0);
                let array_prototype_shape = Cell::new([0, object_prototype.address(), 0]);
                let mut array_prototype = FakeObject::new(&array_prototype_shape, EXTENSIBLE, &[], 0);
                let array_shape = Cell::new([0, array_prototype.address(), 0]);
                let push = Cell::new([0; 3]);
                let methods = vec![(u64::from(value::OBJECT_TAG) << 48) | push.address()].into_boxed_slice();
                array_prototype.0[2] = methods.as_ptr() as u64;
                Self {
                    array_shape,
                    array_prototype,
                    object_prototype,
                    validity: Cell::new([1, 0, 0]),
                    push,
                    _methods: methods,
                    _shapes: [object_prototype_shape, array_prototype_shape],
                }
            }

            /// Runs `a0.push(10)`, and returns its result and the exits taken.
            fn push(&self, array: &FakeObject) -> (JitResult, Vec<ExitKind>) {
                let program = assemble(|_| {
                    vec![
                        Instruction::Enter,
                        get_by_id(r(6), a(0), 0),
                        Instruction::Call {
                            value_feedback: 0,
                            call_feedback: 0,
                            dst: r(5),
                            callee: r(6),
                            this_value: a(0),
                            argument_count: 1,
                            expression_string: None,
                            arguments: vec![c(1)],
                        },
                        Instruction::Return { value: r(5) },
                    ]
                });
                let mut entry = own(GetPropertyInPrototypeChain, self.array_shape.address(), 0);
                entry.prototype = Some(CellId(self.array_prototype.address()));
                entry.prototype_chain_validity = Some(self.validity.id());
                entry.prototype_property = Some(self.push.id());
                entry.prototype_property_intrinsic = Some(Intrinsic::ArrayPrototypePush);
                let layout = test_layout();
                let (snapshot, compiled) = compile_program_with(&program, layout, &[], |snapshot| {
                    snapshot.executables[0].property_caches = vec![cache(vec![entry])];
                    let runtime = &mut snapshot.runtime;
                    runtime.array_push = grow as *const () as u64;
                    runtime.array_prototype = CellId(self.array_prototype.address());
                    runtime.object_prototype = CellId(self.object_prototype.address());
                    runtime.layout = RuntimeLayout {
                        object_indexed_elements: 24,
                        object_indexed_storage_kind: 32,
                        object_indexed_array_like_size: 36,
                        indexed_elements_capacity: -8,
                        indexed_storage_kind_packed: PACKED as u8,
                        object_flag_is_extensible: EXTENSIBLE as u16,
                        object_flag_has_magical_length: MAGICAL_LENGTH as u16,
                        object_flag_may_interfere: MAY_INTERFERE,
                        array_length_writable: 40,
                        array_is_proxy_target: 41,
                        shape_prototype: 8,
                        ..runtime.layout
                    };
                });
                let mut machine = Machine::new(layout, &[array.value()]);
                let result = machine.run(&snapshot, &compiled);
                (result, exit_kinds_taken(&machine, &compiled))
            }
        }

        /// A packed array of `size` elements with room for `capacity`.
        fn buffer(size: usize, capacity: usize) -> Vec<u64> {
            let mut buffer = vec![capacity as u64];
            buffer.extend((0..size).map(|index| value::int32(index as i32)));
            buffer.resize(capacity + 1, value::EMPTY);
            buffer
        }

        fn array(world: &World, buffer: &[u64], size: u32) -> FakeObject {
            FakeObject::new(&world.array_shape, EXTENSIBLE | MAGICAL_LENGTH, &buffer[1..], size)
        }

        #[test]
        fn pushes_append_inline_or_grow_out_of_line() {
            let world = World::new();
            let elements = buffer(2, 4);
            let pushed = array(&world, &elements, 2);
            assert_eq!(world.push(&pushed), (returned(value::int32(3)), vec![]));
            assert_eq!(elements[3], value::int32(10));
            assert_eq!(pushed.0[4] >> 32, 3);

            let full = buffer(2, 2);
            let grown = array(&world, &full, 2);
            assert_eq!(world.push(&grown), (returned(value::int32(99)), vec![]));
            assert_eq!(grown.0[2], value::int32(10));
            assert_eq!(grown.0[4] >> 32, 2);

            // Arrays without indexed properties yet.
            let mut empty = array(&world, &buffer(0, 0), 0);
            empty.0[3] = 0;
            empty.0[4] = 0;
            assert_eq!(world.push(&empty), (returned(value::int32(99)), vec![]));
            assert_eq!(empty.0[2], value::int32(10));
        }

        #[test]
        fn pushes_exit_for_arrays_they_cannot_append_to_unobservably() {
            let mut world = World::new();
            let elements = buffer(1, 4);
            let exits = |world: &World, change: &dyn Fn(&mut FakeObject)| {
                let mut pushed = array(world, &elements, 1);
                change(&mut pushed);
                let (result, exits) = world.push(&pushed);
                assert_eq!(result.status, RESUME);
                assert_eq!(pushed.0[4] >> 32, 1);
                exits
            };
            for change in [
                &(|array: &mut FakeObject| array.0[1] = MAGICAL_LENGTH) as &dyn Fn(&mut FakeObject),
                &|array| array.0[1] = EXTENSIBLE,
                &|array| array.0[1] |= u64::from(MAY_INTERFERE),
                &|array| array.0[4] = 2 | 1 << 32,
                &|array| array.0[5] = 0,
                &|array| array.0[5] = 1 | 1 << 8,
            ] {
                assert_eq!(exits(&world, change), [ExitKind::SlowPath]);
            }
            // Indexed properties on the prototype chain.
            world.array_prototype.0[4] = PACKED | 1 << 32;
            assert_eq!(exits(&world, &|_| {}), [ExitKind::SlowPath]);
            world.array_prototype.0[4] = PACKED;
            world.object_prototype.0[1] |= u64::from(MAY_INTERFERE);
            assert_eq!(exits(&world, &|_| {}), [ExitKind::SlowPath]);
        }
    }

    #[test]
    fn stores_forward_to_loads_and_calls_force_shape_checks_again() {
        let program = assemble(|_| {
            vec![
                Instruction::Enter,
                put_by_id(a(0), c(1), 0),
                get_by_id(r(5), a(0), 1),
                get_by_id(r(6), a(0), 1),
                Instruction::Call {
                    value_feedback: 0,
                    call_feedback: 0,
                    dst: l(1),
                    callee: c(0),
                    this_value: c(0),
                    argument_count: 0,
                    expression_string: None,
                    arguments: Vec::new(),
                },
                get_by_id(r(7), a(0), 1),
                add(l(0), r(5), r(7)),
                add(l(0), l(0), r(6)),
                Instruction::Return { value: l(0) },
            ]
        });
        let shape = Cell::new([0; 3]);
        let other_shape = Cell::new([0; 3]);
        let caches = || {
            vec![
                cache(vec![own(ChangeOwnProperty, shape.address(), 1)]),
                cache(vec![own(GetOwnProperty, shape.address(), 1)]),
            ]
        };

        let object = Object::new(&shape, &[0, value::int32(1)]);
        let (result, _, _) = run_with_caches(&program, caches(), object.value(), None);
        assert_eq!(result, returned(value::int32(30)));
        assert_eq!(object.storage[1], value::int32(10));

        // The call changes the object's shape; the next get notices.
        let mut object = Object::new(&shape, &[0, value::int32(1)]);
        let shape_word = object.shape_word();
        let (result, machine, compiled) = run_with_caches(
            &program,
            caches(),
            object.value(),
            Some((shape_word, other_shape.address())),
        );
        assert_eq!(result.status, RESUME);
        assert_eq!(exit_kinds_taken(&machine, &compiled), [ExitKind::BadShape]);
        assert_eq!(machine.program_counter(), program.offsets[5]);
        assert_eq!(machine.slot(r(5)), value::int32(10));
        assert_eq!(machine.slot(r(6)), value::int32(10));
    }

    #[test]
    fn stores_exit_on_accessors_and_unknown_shapes() {
        let program = assemble(|_| vec![put_by_id(a(0), c(1), 0), Instruction::End { value: a(0) }]);
        let shape = Cell::new([0; 3]);
        let caches = || vec![cache(vec![own(ChangeOwnProperty, shape.address(), 0)])];
        let object = Object::new(&shape, &[accessor()]);
        let (result, machine, compiled) = run_with_caches(&program, caches(), object.value(), None);
        assert_eq!(result.status, RESUME);
        assert_eq!(exit_kinds_taken(&machine, &compiled), [ExitKind::BadShape]);
        assert_eq!(object.storage[0], accessor(), "nothing was stored");
    }
}

#[test]
fn aarch64_property_access_disassembles() {
    use crate::builder::tests::property_access::cache;
    use crate::builder::tests::property_access::get_by_id;
    use crate::builder::tests::property_access::own;
    use crate::builder::tests::property_access::put_by_id;
    use crate::snapshot::CellId;
    use crate::snapshot::PropertyCacheEntryType::ChangeOwnProperty;
    use crate::snapshot::PropertyCacheEntryType::GetOwnProperty;
    use crate::snapshot::PropertyCacheEntryType::GetPropertyInPrototypeChain;

    let program = assemble(|_| {
        vec![
            get_by_id(r(5), a(0), 0),
            get_by_id(r(6), r(5), 1),
            put_by_id(r(6), r(5), 2),
            Instruction::Return { value: r(6) },
        ]
    });
    let mut snapshot = crate::builder::tests::snapshot_for(&program, test_layout());
    snapshot.runtime = runtime();
    let mut prototype_entry = own(GetPropertyInPrototypeChain, 0x1_2345_6780, 3);
    prototype_entry.prototype = Some(CellId(0x2_0000_1000));
    prototype_entry.prototype_chain_validity = Some(CellId(0x2_0000_2000));
    prototype_entry.shape_dictionary_generation = 9;
    prototype_entry.shape_is_dictionary = true;
    snapshot.executables[0].property_caches = vec![
        cache(vec![own(GetOwnProperty, 0x1000, 1), own(GetOwnProperty, 0x2000, 4)]),
        cache(vec![prototype_entry]),
        cache(vec![own(ChangeOwnProperty, 0x3000, 2)]),
    ];
    let compiled = compile_for::<crate::asm::aarch64::MacroAssembler>(&snapshot, &|_| true).unwrap();
    let text = crate::asm::disassembler::aarch64(&compiled.code[..compiled.data_offset as usize]).join("\n");
    assert!(text.contains("ldrb"), "{text}");
    // Object check, shape switch and two loads; object, shape, chain and
    // holder load checks; object, shape and store checks.
    assert_eq!(compiled.sites.len(), 11, "{text}");
    let compiled = compile_for::<crate::asm::x86_64::MacroAssembler>(&snapshot, &|_| true).unwrap();
    assert!(!crate::asm::disassembler::x86_64(&compiled.code[..compiled.data_offset as usize]).is_empty());
}

/// Inlined calls, whose frames compiled code pushes on the fake interpreter
/// stack.
#[cfg(target_arch = "x86_64")]
mod inlining {
    use super::*;
    use crate::builder::tests::inlining::call;
    use crate::builder::tests::inlining::function;
    use crate::builder::tests::inlining::snapshot_with_callee;
    use crate::code::ExitKind;
    use crate::code::ResumeMode;
    use crate::snapshot::CellId;

    /// A fake function object: environment, private environment and script
    /// or module.
    const FUNCTION_WORDS: [u64; 4] = [0xe0, 0xe1, 0x50, 0x51];

    fn boxed(function: &[u64]) -> u64 {
        (u64::from(value::OBJECT_TAG) << 48) | function.as_ptr() as u64
    }

    /// `return callee(10) + r6` with the callee in a0.
    fn caller() -> Program {
        assemble(|_| {
            vec![
                Instruction::Enter,
                Instruction::Mov { dst: r(6), src: c(1) },
                call(r(5), a(0), c(0), vec![c(1)]),
                Instruction::Add {
                    arith_feedback: 0,
                    dst: r(7),
                    lhs: r(5),
                    rhs: r(6),
                },
                Instruction::Return { value: r(7) },
            ]
        })
    }

    /// `function twice(v) { let x = v + v; return x; }`
    fn twice() -> Program {
        assemble(|_| {
            vec![
                Instruction::Enter,
                Instruction::Mov { dst: l(0), src: a(0) },
                Instruction::Exp {
                    arith_feedback: 0,
                    dst: r(5),
                    lhs: l(0),
                    rhs: l(0),
                },
                Instruction::Return { value: r(5) },
            ]
        })
    }

    /// Runs `caller` with `callee` inlined, passing `callee_value(function)`
    /// as the callee, where `function` is the inlined function's value.
    fn run_inlined(
        caller: &Program,
        callee: &Program,
        callee_value: impl FnOnce(u64) -> u64,
        configure: impl FnOnce(&mut Snapshot),
        prepare: impl FnOnce(&mut Machine),
    ) -> (JitResult, Machine, CompiledCode) {
        let function_object = FUNCTION_WORDS.to_vec();
        let mut function = function(true, false);
        function.function = CellId(function_object.as_ptr() as u64);
        let mut snapshot = snapshot_with_callee(caller, callee, function);
        snapshot.runtime = runtime();
        for executable in &mut snapshot.executables {
            executable.feedback.arith = vec![NUMBERS_AND_STRINGS_FEEDBACK];
        }
        configure(&mut snapshot);
        let compiled = compile_for::<crate::asm::MacroAssembler>(&snapshot, &|_| true).unwrap();
        let mut machine = Machine::new(test_layout(), &[callee_value(boxed(&function_object))]);
        prepare(&mut machine);
        let result = machine.run(&snapshot, &compiled);
        (result, machine, compiled)
    }

    #[test]
    fn slow_paths_in_inlined_callees_run_in_published_frames() {
        let (result, machine, compiled) = run_inlined(&caller(), &twice(), |function| function, |_| {}, |_| {});
        assert_eq!(result, returned(value::int32(30)));
        // Compiled code published the callee's frame for its Exp, linked to
        // the caller like an interpreter inline call and uninitialized, and
        // popped it again.
        assert!(!compiled.sites.iter().any(|site| site.kind == SiteKind::Publish));
        assert_eq!(machine.vm[0], machine.frame.as_ptr() as u64);
        assert_eq!(machine.vm[3], machine.interpreter_stack.as_ptr() as u64);
        let pushed = &machine.interpreter_stack;
        assert_eq!(pushed[FRAME_CALLER_WORD], machine.frame.as_ptr() as u64);
        assert_eq!(pushed[3] & 0xff, 0, "uninitialized");
        // Its operands were values: compiled code wrote only the reserved
        // registers and the live arguments, which the garbage collector
        // visits.
        let slots = &pushed[FRAME_HEADER_WORDS..];
        assert_eq!(slots[..5], [value::EMPTY; 5]);
        assert_eq!(slots[l(0).raw() as usize], 0xdead_beef_dead_beef);
    }

    #[test]
    fn slow_paths_in_inlined_callees_write_their_frames_only_if_they_leave() {
        // l1 is no operand of the Exp, which reads a0.
        let callee = assemble(|_| {
            vec![
                Instruction::Enter,
                Instruction::Mov { dst: l(1), src: a(0) },
                Instruction::Exp {
                    arith_feedback: 0,
                    dst: r(5),
                    lhs: a(0),
                    rhs: a(0),
                },
                Instruction::Add {
                    arith_feedback: 0,
                    dst: r(6),
                    lhs: r(5),
                    rhs: l(1),
                },
                Instruction::Return { value: r(6) },
            ]
        });
        let exp_pc = callee.offsets[2];
        let slot = |machine: &Machine, operand: Operand| {
            machine.interpreter_stack[FRAME_HEADER_WORDS + operand.raw() as usize]
        };
        // Continuing, the slow path needs nothing in the frames.
        let (result, machine, _) = run_inlined(&caller(), &callee, |function| function, |_| {}, |_| {});
        assert_eq!(result.status, RETURNED);
        assert_eq!(slot(&machine, l(1)), 0xdead_beef_dead_beef);
        assert_eq!(
            machine.frame[3] & 0xff,
            0,
            "the compiled function's frame stays uninitialized"
        );
        // Not continuing, the interpreter finds every frame complete.
        let (result, machine, compiled) = run_inlined(
            &caller(),
            &callee,
            |function| function,
            |_| {},
            |machine| machine.state.borrow_mut().fail = Some((exp_pc, (CONTINUATION_BIT | 0x40) as i64)),
        );
        assert_eq!(result.status, RESUME);
        assert!(machine.state.borrow().exits_taken.is_empty(), "leaves are no exits");
        assert!(
            compiled
                .sites
                .iter()
                .any(|site| site.kind == SiteKind::Leave && site.frames.len() > 1)
        );
        assert_eq!(slot(&machine, l(1)), value::int32(10));
        assert_eq!(machine.slot(r(6)), machine.slot(c(1)));
    }

    /// `function plus(v) { return v + other; }`, with `other` from `src`.
    fn plus(src: Instruction) -> Program {
        assemble(|_| {
            vec![
                Instruction::Enter,
                src.clone(),
                Instruction::Add {
                    arith_feedback: 0,
                    dst: r(5),
                    lhs: a(0),
                    rhs: r(6),
                },
                Instruction::Return { value: r(5) },
            ]
        })
    }

    #[test]
    fn slow_paths_of_arithmetic_in_inlined_callees_run_in_published_frames() {
        // 10 + 5 in the callee, then 10 more in the caller, without pushing a frame.
        let callee = plus(Instruction::Mov { dst: r(6), src: c(0) });
        let (result, machine, compiled) = run_inlined(
            &caller(),
            &callee,
            |function| function,
            |snapshot| snapshot.executables[1].constants[0] = value::int32(5),
            |_| {},
        );
        assert_eq!(result, returned(value::int32(25)));
        assert_eq!(machine.vm[3], machine.interpreter_stack.as_ptr() as u64);
        assert!(!compiled.sites.iter().any(|site| site.kind == SiteKind::Publish));

        // The slow path of an Add of undefined runs in the callee's frame,
        // which compiled code publishes, and continues after popping it: 10
        // + undefined (as an int32, 0) in the callee, then 10 more in the
        // caller.
        let callee = plus(Instruction::MovSrcUndefined { dst: r(6) });
        let (result, machine, compiled) = run_inlined(&caller(), &callee, |function| function, |_| {}, |_| {});
        assert_eq!(result, returned(value::int32(int(value::UNDEFINED).wrapping_add(20))));
        assert!(machine.state.borrow().exits_taken.is_empty());
        assert_eq!(machine.vm[0], machine.frame.as_ptr() as u64);
        assert_eq!(machine.vm[3], machine.interpreter_stack.as_ptr() as u64);
        // NB: Compiled code publishes the frames, not the runtime.
        assert!(!compiled.sites.iter().any(|site| site.kind == SiteKind::Publish));

        // A slow path that does not continue leaves the interpreter in the
        // callee's frame.
        let add_pc = callee.offsets[2];
        let (result, machine, _) = run_inlined(
            &caller(),
            &callee,
            |function| function,
            |_| {},
            |machine| machine.state.borrow_mut().fail = Some((add_pc, (CONTINUATION_BIT | 0x40) as i64)),
        );
        assert_eq!(result.status, RESUME);
        assert!(machine.state.borrow().exits_taken.is_empty());
        let pushed = machine.interpreter_stack.as_ptr() as u64;
        assert_eq!(machine.vm[0], pushed);
        assert_eq!(machine.interpreter_stack[0] as u32, 0x40, "the callee's pc");
        assert_eq!(
            machine.interpreter_stack[FRAME_CALLER_WORD],
            machine.frame.as_ptr() as u64
        );
    }

    #[test]
    fn calls_of_other_functions_exit() {
        let other = (u64::from(value::OBJECT_TAG) << 48) | 0x7777;
        let (result, machine, compiled) = run_inlined(&caller(), &twice(), |_| other, |_| {}, |_| {});
        assert_eq!(result.status, RESUME);
        let state = machine.state.borrow();
        assert_eq!(
            compiled.sites[state.exits_taken[0] as usize].kind,
            SiteKind::Exit(ExitKind::BadCallTarget)
        );
        assert_eq!(state.exit_frames[0].len(), 1);
        assert_eq!(machine.program_counter(), caller().offsets[2]);
        assert_eq!(machine.slot(r(6)), value::int32(10));
    }

    #[test]
    fn exits_in_inlined_callees_describe_both_frames() {
        let (result, machine, _) = run_inlined(
            &caller(),
            &twice(),
            |function| function,
            |snapshot| snapshot.executables[1].feedback.arith = vec![0],
            |_| {},
        );
        assert_eq!(result.status, RESUME);
        let state = machine.state.borrow();
        let frames = &state.exit_frames[0];
        assert_eq!(frames.len(), 2);
        let (executable, pc, mode, values) = &frames[0];
        assert_eq!((*executable, *pc, *mode), (1, twice().offsets[2], ResumeMode::ResumeAt));
        assert!(values.contains(&(l(0).raw(), value::int32(10))));
        let (executable, pc, mode, values) = &frames[1];
        assert_eq!(
            (*executable, *pc, *mode),
            (0, caller().offsets[2], ResumeMode::ResumeAfter { dst: r(5).raw() })
        );
        assert!(values.contains(&(r(6).raw(), value::int32(10))));
        assert_eq!(
            machine.vm[3],
            machine.interpreter_stack.as_ptr() as u64,
            "exits leave materializing to the runtime"
        );
    }

    #[test]
    fn code_that_may_not_fit_on_the_interpreter_stack_exits_at_entry() {
        let (result, machine, _) = run_inlined(
            &caller(),
            &twice(),
            |function| function,
            |_| {},
            |machine| {
                machine.vm[3] = 1000;
                machine.vm[4] = 1010;
            },
        );
        assert_eq!(result.status, RESUME);
        assert_eq!(machine.frame[3] & 0xff, 0, "nothing ran");
    }
}

/// Arguments objects the code never creates.
#[cfg(target_arch = "x86_64")]
mod virtual_arguments {
    use super::*;
    use crate::code::ExitKind;

    fn layout() -> FrameLayout {
        FrameLayout {
            number_of_arguments: 3,
            ..test_layout()
        }
    }

    fn argument(index: u32) -> Operand {
        Operand::from_raw(layout().arguments_base() + index)
    }

    /// `arguments.length + arguments[a2] + arguments.length`, with an
    /// unmapped arguments object.
    fn program() -> Program {
        assemble(|_| {
            vec![
                Instruction::Enter,
                Instruction::CreateArguments {
                    dst: Some(l(0)),
                    kind: 1,
                    is_immutable: false,
                    creates_parameter_bindings: false,
                },
                Instruction::GetLength {
                    value_feedback: 0,
                    dst: r(5),
                    base: l(0),
                    base_identifier: None,
                    cache: 0,
                },
                Instruction::GetByValue {
                    value_feedback: 0,
                    keyed_feedback: 0,
                    dst: r(6),
                    base: l(0),
                    property: argument(2),
                    base_identifier: None,
                    cache: 0,
                },
                add(r(7), r(5), r(6)),
                Instruction::GetLength {
                    value_feedback: 0,
                    dst: r(6),
                    base: l(0),
                    base_identifier: None,
                    cache: 0,
                },
                add(r(7), r(7), r(6)),
                Instruction::Return { value: r(7) },
            ]
        })
    }

    /// Runs `program()` with two passed arguments and `index` as the third.
    fn run_arguments(index: u64, fail: Option<i64>) -> (JitResult, Machine, CompiledCode) {
        run_arguments_with(value::int32(20), index, fail)
    }

    /// Like `run_arguments()`, with `second` as the second argument.
    fn run_arguments_with(second: u64, index: u64, fail: Option<i64>) -> (JitResult, Machine, CompiledCode) {
        let program = program();
        let (snapshot, compiled) = compile_program(&program, layout(), &[]);
        let mut machine = Machine::new(layout(), &[value::int32(10), second, index]);
        // The passed argument count (see `offsets()`).
        machine.frame[17] = 2;
        if let Some(control) = fail {
            machine.state.borrow_mut().fail = Some((program.offsets[4], control));
        }
        let result = machine.run(&snapshot, &compiled);
        (result, machine, compiled)
    }

    #[test]
    fn length_and_indices_read_the_frame() {
        let (result, machine, compiled) = run_arguments(value::int32(1), None);
        assert_eq!(result, returned(value::int32(2 + 20 + 2)));
        assert_eq!(machine.state.borrow().arguments_created, 0);
        // NB: Nothing here needed the frame initialized, let alone the object.
        assert_eq!(machine.frame[3] & 0xff, 0, "frame_initialized");
        assert_ne!(
            machine.slot(l(0)),
            fake_arguments_object(false),
            "the object was never created"
        );
        let text = crate::asm::disassembler::x86_64(&compiled.code[..compiled.data_offset as usize]).join("\n");
        assert!(!text.is_empty());
    }

    #[test]
    fn indices_past_the_passed_arguments_exit_with_the_object() {
        for index in [value::int32(2), value::int32(-1), value::UNDEFINED] {
            let (result, machine, compiled) = run_arguments(index, None);
            assert_eq!(result.status, RESUME);
            let state = machine.state.borrow();
            assert_eq!(
                compiled.sites[state.exits_taken[0] as usize].kind,
                SiteKind::Exit(ExitKind::ArgumentsIndex)
            );
            let (_, _, _, values) = &state.exit_frames[0][0];
            assert!(
                values.contains(&(l(0).raw(), fake_arguments_object(false))),
                "{values:?}"
            );
        }
    }

    #[test]
    fn slow_paths_that_leave_create_the_object() {
        // NB: Adding a boolean takes the slow path of Add.
        let (result, machine, _) = run_arguments_with(value::TRUE, value::int32(1), Some(4));
        assert_eq!(result.status, RESUME);
        assert_eq!(machine.state.borrow().arguments_created, 1);
        assert_eq!(machine.slot(l(0)), fake_arguments_object(false));
        // Exceptions leaving the function need no object.
        let (result, machine, _) = run_arguments_with(value::TRUE, value::int32(1), Some(-1));
        assert_eq!(result.status, EXIT_INTERPRETER);
        assert_eq!(machine.state.borrow().arguments_created, 0);
    }
}

/// Direct calls against fake callees, function objects and executables.
mod direct_calls {
    use super::*;
    use crate::builder::tests::inlining::call;
    use crate::snapshot::CallFeedbackSnapshot;
    use crate::snapshot::CellId;
    use crate::snapshot::DirectCallTarget;
    use crate::snapshot::InlinedFunctionSnapshot;

    const REALM: u64 = 0x5eed_0000;
    const GLOBAL_THIS: u64 = 0x6000;
    /// The fake function object: environment, private environment and two
    /// words of script or module.
    const FUNCTION_WORDS: [u64; 4] = [0xe0, 0xe1, 0x50, 0x51];

    /// Words of a fake `Executable` (see `offsets()`), and the test state
    /// pointer.
    #[cfg(target_arch = "x86_64")]
    const EXECUTABLE_TEST_STATE_WORD: usize = 3;
    const EXECUTABLE_JIT_ENTRY_SLOT_WORD: usize = 5;
    /// The fake JIT entry table has 4 slots: entries, then owners.
    const ENTRY_TABLE_SLOT_MASK: u32 = 3;
    const ENTRY_TABLE_OWNERS_WORD: usize = 4;
    /// The callee's slot in the fake JIT entry table.
    const CALLEE_SLOT: usize = 1;

    #[derive(Default)]
    pub(super) struct DirectCallState {
        /// Every callee frame a fake callee entry saw, header and slots.
        callee_frames: Vec<Vec<u64>>,
        /// What fake callee entries return.
        callee_result: Option<(u64, u64)>,
        /// The status of every `finish_direct_call`.
        finishes: Vec<u64>,
        /// What `finish_direct_call` writes as the call's result.
        finish_result: u64,
        /// The function and `this` argument of every fake
        /// `prepare_call_environment`, and what it returns.
        prepared: Vec<(u64, u64)>,
        prepared_environment: (u64, u64),
    }

    /// The JIT entry of a fake callee: records its frame and returns. Like
    /// compiled code, it publishes the frame (which direct calls do not)
    /// unless it returns.
    extern "C" fn callee_entry(vm: *mut u64, frame: *mut u64) -> JitResult {
        // SAFETY: The callee frame is on the fake interpreter stack, with its
        // slot count in the header.
        unsafe {
            let slot_count = *frame.cast::<u8>().add(148).cast::<u32>() as usize;
            let copy = std::slice::from_raw_parts(frame, FRAME_HEADER_WORDS + slot_count).to_vec();
            let mut state = test_state(vm).borrow_mut();
            state.direct.callee_frames.push(copy);
            let (value, status) = state.direct.callee_result.expect("a callee result");
            if status != RETURNED {
                *vm = frame as u64;
            }
            JitResult { value, status }
        }
    }

    /// A fake `libjs_jit_finish_direct_call` that "runs" the callee to
    /// completion like the interpreter: it returns into the caller's
    /// destination and pops its frame.
    pub(super) extern "C" fn finish_direct_call(vm: *mut u64, frame: *mut u64, pc: u32, status: u64) -> i64 {
        // SAFETY: The fake VM and frames are live.
        unsafe {
            *vm.add(3) = *vm;
            *vm = frame as u64;
            let Instruction::Call { dst, .. } = decoded(vm, pc) else {
                panic!("not a Call");
            };
            let result = {
                let mut state = test_state(vm).borrow_mut();
                state.direct.finishes.push(status);
                state.direct.finish_result
            };
            *frame.add(FRAME_HEADER_WORDS + dst.raw() as usize) = result;
        }
        continuation_after(vm, pc)
    }

    /// The fake heap of one direct call test.
    struct Callee {
        function: Vec<u64>,
        executable: Vec<u64>,
        entry_table: Vec<u64>,
        interpreter_stack: Vec<u64>,
    }

    impl Callee {
        fn new() -> Self {
            let mut callee = Self {
                function: FUNCTION_WORDS.to_vec(),
                executable: vec![0; 7],
                entry_table: vec![0; 2 * ENTRY_TABLE_OWNERS_WORD],
                interpreter_stack: vec![0xdead_beef_dead_beef; 512],
            };
            callee.executable[EXECUTABLE_JIT_ENTRY_SLOT_WORD] = CALLEE_SLOT as u64;
            callee.entry_table[ENTRY_TABLE_OWNERS_WORD + CALLEE_SLOT] = callee.executable.as_ptr() as u64;
            callee.set_entry(callee_entry as *const () as u64);
            callee
        }

        /// Makes calls of the callee enter `entry`.
        fn set_entry(&mut self, entry: u64) {
            self.entry_table[CALLEE_SLOT] = entry;
        }

        /// Makes JIT code find its dynamic callees' entries in the callee's
        /// fake JIT entry table.
        #[cfg(target_arch = "x86_64")]
        fn use_entry_table(&self, runtime: &mut crate::snapshot::RuntimeInfo) {
            let layout = &mut runtime.dynamic_calls;
            layout.executable_jit_entry_slot = (8 * EXECUTABLE_JIT_ENTRY_SLOT_WORD) as u32;
            layout.jit_entry_table = self.entry_table.as_ptr() as u64;
            layout.jit_entry_slot_mask = ENTRY_TABLE_SLOT_MASK;
            layout.jit_entry_table_owners = (8 * ENTRY_TABLE_OWNERS_WORD) as u32;
        }

        #[cfg(target_arch = "x86_64")]
        fn function_value(&self) -> u64 {
            (u64::from(value::OBJECT_TAG) << 48) | self.function.as_ptr() as u64
        }

        fn target(&self, strict: bool, uses_this: bool) -> DirectCallTarget {
            DirectCallTarget {
                function: InlinedFunctionSnapshot {
                    function: CellId(self.function.as_ptr() as u64),
                    formal_parameter_count: 3,
                    strict,
                    uses_this,
                    global_this: CellId(GLOBAL_THIS),
                    realm: CellId(REALM),
                    shared_data: crate::snapshot::CellId(0x5d00),
                },
                executable: CellId(self.executable.as_ptr() as u64),
                entry: self.entry_table[CALLEE_SLOT..].as_ptr() as u64,
                registers_and_locals_count: 7,
                registers_and_locals_and_constants_count: 9,
                // NB: The fake function's words (see `offsets()`).
                function_fields: FunctionFrameFields {
                    script_or_module: [self.function[2], self.function[3]],
                    environment: self.function[0],
                    private_environment: self.function[1],
                },
                environment: None,
                closures: false,
            }
        }
    }

    /// The caller's layout, with three arguments.
    fn caller_layout() -> FrameLayout {
        FrameLayout {
            number_of_arguments: 3,
            ..test_layout()
        }
    }

    fn argument(index: u32) -> Operand {
        Operand::from_raw(caller_layout().arguments_base() + index)
    }

    /// `return callee.call(a2, a1, 10)` with the callee in a0.
    fn caller() -> Program {
        assemble(|_| {
            vec![
                Instruction::Enter,
                call(r(5), argument(0), argument(2), vec![argument(1), c(1)]),
                Instruction::Return { value: r(5) },
            ]
        })
    }

    fn compile_caller(target: DirectCallTarget) -> (Snapshot, CompiledCode) {
        compile_program_with(&caller(), caller_layout(), &[], |snapshot| {
            snapshot.executables[0].feedback.call = vec![CallFeedbackSnapshot {
                target: Some(target.function.function),
                direct_call: Some(target),
                ..CallFeedbackSnapshot::default()
            }];
        })
    }

    #[cfg(target_arch = "x86_64")]
    fn run_direct(
        callee: &mut Callee,
        target: DirectCallTarget,
        arguments: &[u64],
        prepare: impl FnOnce(&mut Machine),
    ) -> (JitResult, Machine, CompiledCode) {
        let (snapshot, compiled) = compile_caller(target);
        let mut machine = Machine::new(caller_layout(), arguments);
        let stack = callee.interpreter_stack.as_mut_ptr() as u64;
        machine.vm[3] = stack;
        machine.vm[4] = stack + 8 * callee.interpreter_stack.len() as u64;
        callee.executable[EXECUTABLE_TEST_STATE_WORD] = std::ptr::from_ref::<RefCell<TestState>>(&machine.state) as u64;
        prepare(&mut machine);
        let result = machine.run(&snapshot, &compiled);
        (result, machine, compiled)
    }

    #[test]
    fn direct_calls_store_constant_frame_fields_16_bytes_at_a_time() {
        let callee = Callee::new();
        let (snapshot, _) = compile_caller(callee.target(true, true));
        let compiled = compile_for::<crate::asm::aarch64::MacroAssembler>(&snapshot, &|_| true).unwrap();
        let text = crate::asm::disassembler::aarch64(&compiled.code[..compiled.data_offset as usize]).join("\n");
        assert!(text.contains("str q0, [x"), "{text}");
        let compiled = compile_for::<crate::asm::x86_64::MacroAssembler>(&snapshot, &|_| true).unwrap();
        let text = crate::asm::disassembler::x86_64(&compiled.code[..compiled.data_offset as usize]).join("\n");
        assert!(text.contains("movups xmmword ptr ["), "{text}");
    }

    #[test]
    fn the_target_is_embedded() {
        let callee = Callee::new();
        let (_, compiled) = compile_caller(callee.target(false, true));
        for cell in [
            callee.function.as_ptr() as u64,
            callee.executable.as_ptr() as u64,
            REALM,
            GLOBAL_THIS,
        ] {
            assert!(compiled.embedded_cells.contains(&CellId(cell)), "{cell:#x}");
        }
    }

    #[cfg(target_arch = "x86_64")]
    mod execution {
        use super::*;

        #[test]
        fn direct_calls_build_the_callee_frame_and_pop_it() {
            let mut callee = Callee::new();
            let target = callee.target(true, false);
            let arguments = [callee.function_value(), value::int32(7), value::NULL];
            let (result, machine, _) = run_direct(&mut callee, target, &arguments, |machine| {
                machine.state.borrow_mut().direct.callee_result = Some((value::int32(99), RETURNED));
            });
            assert_eq!(result, returned(value::int32(99)));
            let program = caller();

            let state = machine.state.borrow();
            let frame = &state.direct.callee_frames[0];
            let word = |offset: usize| frame[offset / 8];
            let half = |offset: usize| (frame[offset / 8] >> (8 * (offset % 8))) as u32;
            let byte = |offset: usize| (frame[offset / 8] >> (8 * (offset % 8))) as u8;
            assert_eq!(word(64), callee.function.as_ptr() as u64, "function");
            assert_eq!(word(72), REALM);
            assert_eq!((word(80), word(88)), (FUNCTION_WORDS[2], FUNCTION_WORDS[3]));
            assert_eq!(
                (word(8), word(96), word(16)),
                (FUNCTION_WORDS[0], FUNCTION_WORDS[0], FUNCTION_WORDS[1])
            );
            assert_eq!(word(104), 0, "no frame id yet");
            assert_eq!(word(0), 0, "program counter and incumbent counter");
            assert_eq!(half(112), u32::MAX, "no yield continuation");
            assert_eq!((byte(116), byte(117), byte(118), byte(24)), (0, 0, 0, 0));
            assert_eq!(word(120), value::EMPTY, "no this value");
            assert_eq!(word(32), callee.executable.as_ptr() as u64);
            assert_eq!(word(8 * FRAME_CALLER_WORD), machine.frame.as_ptr() as u64);
            assert_eq!(half(136), 2, "passed argument count");
            assert_eq!(half(140), program.offsets[2], "return pc");
            assert_eq!(half(144), r(5).raw());
            assert_eq!(half(148), 9 + 3, "slot count");
            assert_eq!(half(152), 3, "argument count");
            assert_eq!(byte(156), 1, "returns to a native caller");
            let slots = &frame[FRAME_HEADER_WORDS..];
            assert_eq!(slots[..5], [value::EMPTY; 5]);
            assert_eq!(slots[9..12], [value::int32(7), value::int32(10), value::UNDEFINED]);

            // The callee's frame is popped like the interpreter's Return does.
            // The result is the call's value, which only the return register
            // holds, and the caller is still at its call.
            assert_ne!(machine.slot(r(5)), value::int32(99));
            assert_eq!(machine.vm[0], machine.frame.as_ptr() as u64);
            assert_eq!(machine.vm[3], callee.interpreter_stack.as_ptr() as u64);
            assert_eq!(machine.vm[VM_EXECUTION_GENERATION_WORD], 1);
            assert_eq!(machine.program_counter(), program.offsets[1]);
            assert!(state.direct.finishes.is_empty());
            assert_eq!(state.generic_calls, 0);
        }

        #[test]
        fn callees_that_do_not_return_are_finished_by_the_runtime() {
            for status in [RESUME, EXIT_INTERPRETER] {
                let mut callee = Callee::new();
                let target = callee.target(true, false);
                let arguments = [callee.function_value(), value::int32(7), value::NULL];
                let (result, machine, _) = run_direct(&mut callee, target, &arguments, |machine| {
                    let mut state = machine.state.borrow_mut();
                    state.direct.callee_result = Some((0, status));
                    state.direct.finish_result = value::int32(5);
                });
                assert_eq!(result, returned(value::int32(5)));
                let state = machine.state.borrow();
                assert_eq!(state.direct.finishes, [status]);
                assert_eq!(state.direct.callee_frames.len(), 1);
                assert_eq!(machine.vm[0], machine.frame.as_ptr() as u64);
            }
        }

        /// The call entry of executables without JIT code, like the
        /// runtime's: publishes the frame and has the interpreter run it.
        extern "C" fn not_compiled_call_entry(vm: *mut u64, frame: *mut u64) -> JitResult {
            // SAFETY: The fake VM is live.
            unsafe { *vm = frame as u64 };
            JitResult {
                value: 0,
                status: RESUME,
            }
        }

        #[test]
        fn callees_without_jit_code_run_in_the_interpreter() {
            let mut callee = Callee::new();
            callee.set_entry(not_compiled_call_entry as *const () as u64);
            let target = callee.target(true, false);
            let arguments = [callee.function_value(), value::int32(7), value::NULL];
            let (result, machine, _) = run_direct(&mut callee, target, &arguments, |machine| {
                machine.state.borrow_mut().direct.finish_result = value::int32(6);
            });
            assert_eq!(result, returned(value::int32(6)));
            let state = machine.state.borrow();
            assert_eq!(state.direct.finishes, [RESUME]);
            assert!(state.direct.callee_frames.is_empty());
        }

        #[test]
        fn callees_other_than_the_target_take_the_generic_path() {
            let mut callee = Callee::new();
            let target = callee.target(true, false);
            let arguments = [value::int32(1), value::int32(7), value::NULL];
            let (result, machine, _) = run_direct(&mut callee, target, &arguments, |_| {});
            assert_eq!(result, returned(value::int32(18)));
            assert_eq!(machine.state.borrow().generic_calls, 1);
        }

        #[test]
        fn code_with_direct_calls_checks_the_interpreter_stack_at_entry() {
            // NB: Without room for the callee's frame, the code resumes in
            //     the interpreter before it does anything.
            let mut callee = Callee::new();
            let target = callee.target(true, false);
            let arguments = [callee.function_value(), value::int32(7), value::NULL];
            let (result, machine, _) = run_direct(&mut callee, target, &arguments, |machine| {
                machine.vm[4] = machine.vm[3] + 64;
            });
            assert_eq!(result.status, RESUME);
            let state = machine.state.borrow();
            assert_eq!((state.generic_calls, state.direct.callee_frames.len()), (0, 0));
            assert_eq!(machine.vm[3], callee.interpreter_stack.as_ptr() as u64);
        }

        #[test]
        fn direct_calls_allocate_the_function_environments_of_their_callees() {
            // A fake heap (allocated, threshold and total bytes), and the
            // local free list of one cell, which has room for 12 words and
            // two binding values, and links to the next cell with `link`.
            let run = |threshold: u64, free_cell: bool, link: u64| {
                let mut callee = Callee::new();
                let mut heap = vec![100u64, threshold, 1000];
                let mut block = FakeBlock::new(14, 0x5a5a_5a5a_5a5a_5a5a);
                block.0[FakeBlock::CELL] = link;
                let mut free_list = vec![if free_cell { block.cell().as_ptr() as u64 } else { 0 }];
                let template = crate::snapshot::EnvironmentTemplateSnapshot {
                    allocator: free_list.as_mut_ptr() as u64,
                    cell_size: 14 * 8,
                    words: (0..12).map(|index| 0x1000 + index).collect(),
                    inline_binding_values: true,
                    binding_values_offset: 48,
                    binds_this: true,
                    this_value_offset: 72,
                    cells: vec![CellId(0xcafe)],
                };
                let target = DirectCallTarget {
                    environment: Some(0),
                    ..callee.target(true, true)
                };
                let (snapshot, compiled) = compile_program_with(&caller(), caller_layout(), &[], |snapshot| {
                    snapshot.executables[0].feedback.call = vec![CallFeedbackSnapshot {
                        target: Some(target.function.function),
                        direct_call: Some(target),
                        ..CallFeedbackSnapshot::default()
                    }];
                    snapshot.executables[0].environment_templates = vec![template.clone()];
                    let allocation = &mut snapshot.runtime.object_allocation;
                    allocation.heap = heap.as_mut_ptr() as u64;
                    allocation.heap_allocated_bytes_offset = 0;
                    allocation.heap_threshold_offset = 8;
                    allocation.heap_total_allocated_bytes_offset = 16;
                    allocation.local_free_list_offset = 0;
                    allocation.freelist_next_offset = 0;
                    allocation.freelist_link_mask = FakeBlock::LINK_MASK;
                });
                assert!(compiled.embedded_cells.contains(&CellId(0xcafe)));
                let mut machine = Machine::new(
                    caller_layout(),
                    &[callee.function_value(), value::int32(7), value::TRUE],
                );
                let stack = callee.interpreter_stack.as_mut_ptr() as u64;
                machine.vm[3] = stack;
                machine.vm[4] = stack + 8 * callee.interpreter_stack.len() as u64;
                callee.executable[EXECUTABLE_TEST_STATE_WORD] =
                    std::ptr::from_ref::<RefCell<TestState>>(&machine.state) as u64;
                machine.state.borrow_mut().direct.callee_result = Some((value::int32(99), RETURNED));
                let result = machine.run(&snapshot, &compiled);
                let frames = machine.state.borrow().direct.callee_frames.clone();
                let generic_calls = machine.state.borrow().generic_calls;
                (
                    result,
                    frames,
                    generic_calls,
                    heap,
                    block,
                    free_list,
                    machine.vm[3] == stack,
                )
            };

            let (result, frames, generic_calls, heap, block, free_list, popped) = run(10_000, true, 0);
            assert_eq!((result, generic_calls), (returned(value::int32(99)), 0));
            let cell = block.cell();
            let address = cell.as_ptr() as u64;
            let mut expected = (0..12).map(|index| 0x1000 + index).collect::<Vec<u64>>();
            expected[6] = address + 96;
            expected[9] = value::TRUE;
            assert_eq!(cell[..12], expected[..]);
            assert_eq!(heap, [100 + 112, 10_000, 1000 + 112]);
            assert_eq!(free_list[0], block.address(), "the end of the free list");
            // The callee's lexical and variable environments, and its `this`.
            let frame = &frames[0];
            assert_eq!((frame[1], frame[12], frame[15]), (address, address, value::TRUE));
            assert!(popped);

            // Whatever a free cell's link was overwritten with, the next cell
            // is in the cell's block.
            let (_, _, _, _, block, free_list, _) = run(10_000, true, 0x7f12_3456_0000_0040);
            assert_eq!(free_list[0], block.address() + 0x40, "the next cell");

            // Without room before the next collection, or without free
            // cells, the call takes the generic path and frees its frame.
            for (threshold, free_cell) in [(150, true), (10_000, false)] {
                let (result, frames, generic_calls, heap, _, _, popped) = run(threshold, free_cell, 0);
                assert_eq!(result.status, RETURNED);
                assert_eq!((frames.len(), generic_calls, heap[0]), (0, 1, 100));
                assert!(popped);
            }
        }

        #[test]
        fn sloppy_callees_get_the_global_this_or_an_object() {
            let global_this = (u64::from(value::OBJECT_TAG) << 48) | GLOBAL_THIS;
            let object = (u64::from(value::OBJECT_TAG) << 48) | 0x7000;
            for (this_value, expected) in [
                (value::UNDEFINED, Some(global_this)),
                (value::NULL, Some(global_this)),
                (object, Some(object)),
                (value::int32(3), None),
            ] {
                let mut callee = Callee::new();
                let target = callee.target(false, true);
                let arguments = [callee.function_value(), value::int32(7), this_value];
                let (_, machine, _) = run_direct(&mut callee, target, &arguments, |machine| {
                    machine.state.borrow_mut().direct.callee_result = Some((value::int32(1), RETURNED));
                });
                let state = machine.state.borrow();
                match expected {
                    Some(expected) => {
                        let frame = &state.direct.callee_frames[0];
                        assert_eq!(frame[120 / 8], expected);
                        assert_eq!(
                            frame[FRAME_HEADER_WORDS + crate::bytecode::THIS_VALUE_REGISTER as usize],
                            expected
                        );
                    }
                    None => assert_eq!(state.generic_calls, 1, "primitives are boxed by the generic call"),
                }
            }
        }

        /// A fake ECMAScript function for dynamic calls: shape, flags, shared
        /// data, then the words `FUNCTION_WORDS` has (see `dynamic_runtime()`).
        struct DynamicCallee {
            callee: Callee,
            function: Vec<u64>,
            shape: Vec<u64>,
            shared_data: Vec<u64>,
        }

        const ECMASCRIPT_FUNCTION_FLAG: u16 = 1 << 6;
        const CAN_INLINE_CALL: u64 = 1 << 32;
        const NEEDS_ENVIRONMENT: u64 = 1 << 33;
        const USES_THIS: u64 = 1 << 34;
        const STRICT: u64 = 1 << 35;
        /// The word of a fake `Executable` with the slot count before the
        /// arguments.
        const EXECUTABLE_ARGUMENTS_BASE_WORD: usize = 4;
        const CALL_FUNCTION: u64 = 0xca11_0000;

        impl DynamicCallee {
            fn new(formal_parameter_count: u64, flags: u64) -> Self {
                let mut callee = Callee::new();
                callee.executable[EXECUTABLE_ARGUMENTS_BASE_WORD] = 9;
                let shape = vec![REALM];
                let shared_data = vec![callee.executable.as_ptr() as u64, formal_parameter_count | flags];
                let mut function = vec![
                    shape.as_ptr() as u64,
                    u64::from(ECMASCRIPT_FUNCTION_FLAG),
                    shared_data.as_ptr() as u64,
                ];
                function.extend(FUNCTION_WORDS);
                Self {
                    callee,
                    function,
                    shape,
                    shared_data,
                }
            }

            fn value(&self) -> u64 {
                (u64::from(value::OBJECT_TAG) << 48) | self.function.as_ptr() as u64
            }
        }

        fn dynamic_runtime(runtime: &mut crate::snapshot::RuntimeInfo, callee: &DynamicCallee) {
            runtime.offsets.ecmascript_function_environment = 24;
            runtime.offsets.ecmascript_function_private_environment = 32;
            runtime.offsets.ecmascript_function_script_or_module = 40;
            runtime.dynamic_calls = crate::snapshot::DynamicCallLayout {
                object_flag_is_ecmascript_function: ECMASCRIPT_FUNCTION_FLAG,
                ecmascript_function_shared_data: 16,
                shared_data_executable: 0,
                shared_data_asm_call_metadata: 8,
                metadata_can_inline_call: CAN_INLINE_CALL,
                metadata_needs_environment_or_this_value_resolution: NEEDS_ENVIRONMENT,
                metadata_uses_this: USES_THIS,
                metadata_strict: STRICT,
                executable_registers_and_locals_and_constants_count: (8 * EXECUTABLE_ARGUMENTS_BASE_WORD) as u32,
                shape_realm: 0,
                ..crate::snapshot::DynamicCallLayout::default()
            };
            callee.callee.use_entry_table(runtime);
            install_call_stub(runtime);
        }

        /// Generates the call stub for the runtime and makes JIT code use it.
        /// The stub's code lives for the rest of the test process.
        fn install_call_stub(runtime: &mut crate::snapshot::RuntimeInfo) {
            let code = crate::codegen::generate_call_stub::<crate::asm::MacroAssembler>(runtime)
                .expect("the call stub can be generated")
                .expect("the runtime has a call stub");
            let code = Box::leak(Box::new(crate::asm::executable_code::ExecutableCode::new(&code)));
            runtime.dynamic_calls.call_stub = code.address();
        }

        /// Runs `caller()` with dynamic calls, for a site whose single callee
        /// was `Function.prototype.call` if `through_call`.
        fn run_dynamic(callee: &mut DynamicCallee, arguments: &[u64], through_call: bool) -> (JitResult, Machine) {
            let (snapshot, compiled) = compile_program_with(&caller(), caller_layout(), &[], |snapshot| {
                dynamic_runtime(&mut snapshot.runtime, &callee);
                snapshot.executables[0].feedback.call = vec![CallFeedbackSnapshot {
                    target: Some(CellId(CALL_FUNCTION)),
                    flags: if through_call { 0 } else { 1 },
                    target_intrinsic: through_call.then_some(crate::snapshot::Intrinsic::FunctionPrototypeCall),
                    ..CallFeedbackSnapshot::default()
                }];
            });
            let mut machine = Machine::new(caller_layout(), arguments);
            let stack = callee.callee.interpreter_stack.as_mut_ptr() as u64;
            machine.vm[3] = stack;
            machine.vm[4] = stack + 8 * callee.callee.interpreter_stack.len() as u64;
            callee.callee.executable[EXECUTABLE_TEST_STATE_WORD] =
                std::ptr::from_ref::<RefCell<TestState>>(&machine.state) as u64;
            machine.state.borrow_mut().direct.callee_result = Some((value::int32(99), RETURNED));
            machine.state.borrow_mut().direct.finish_result = value::int32(6);
            let result = machine.run(&snapshot, &compiled);
            (result, machine)
        }

        #[test]
        fn dynamic_calls_build_the_callee_frame_from_the_function() {
            let mut callee = DynamicCallee::new(4, CAN_INLINE_CALL | STRICT);
            let arguments = [callee.value(), value::int32(7), value::NULL];
            let (result, machine) = run_dynamic(&mut callee, &arguments, false);
            assert_eq!(result, returned(value::int32(99)));
            let program = caller();
            let state = machine.state.borrow();
            assert_eq!(state.generic_calls, 0);
            let frame = &state.direct.callee_frames[0];
            let word = |offset: usize| frame[offset / 8];
            let half = |offset: usize| (frame[offset / 8] >> (8 * (offset % 8))) as u32;
            let byte = |offset: usize| (frame[offset / 8] >> (8 * (offset % 8))) as u8;
            assert_eq!(word(64), callee.function.as_ptr() as u64, "function");
            assert_eq!(word(72), REALM);
            assert_eq!((word(80), word(88)), (FUNCTION_WORDS[2], FUNCTION_WORDS[3]));
            assert_eq!(
                (word(8), word(96), word(16)),
                (FUNCTION_WORDS[0], FUNCTION_WORDS[0], FUNCTION_WORDS[1])
            );
            assert_eq!(word(104), 0, "no frame id yet");
            assert_eq!(word(120), value::EMPTY, "no this value");
            assert_eq!(word(32), callee.callee.executable.as_ptr() as u64);
            assert_eq!(word(8 * FRAME_CALLER_WORD), machine.frame.as_ptr() as u64);
            assert_eq!(half(136), 2, "passed argument count");
            assert_eq!(half(140), program.offsets[2], "return pc");
            assert_eq!(half(144), r(5).raw());
            assert_eq!(half(148), 9 + 4, "slot count");
            assert_eq!(half(152), 4, "argument count");
            assert_eq!(
                (byte(24), byte(156)),
                (0, 1),
                "not initialized, returns to a native caller"
            );
            let slots = &frame[FRAME_HEADER_WORDS..];
            assert_eq!(slots[..5], [value::EMPTY; 5]);
            assert_eq!(
                slots[9..13],
                [value::int32(7), value::int32(10), value::UNDEFINED, value::UNDEFINED]
            );
            assert_eq!(machine.slot(r(5)), value::int32(99));
            assert_eq!(machine.vm[0], machine.frame.as_ptr() as u64);
            assert_eq!(machine.vm[3], callee.callee.interpreter_stack.as_ptr() as u64);
            let _ = (&callee.shape, &callee.shared_data);
        }

        #[test]
        fn dynamic_calls_bind_this_like_the_interpreter() {
            let object = (u64::from(value::OBJECT_TAG) << 48) | 0x7000;
            for (flags, this_value, expected) in [
                (STRICT | USES_THIS, value::int32(3), Some(value::int32(3))),
                (STRICT | USES_THIS, value::UNDEFINED, Some(value::UNDEFINED)),
                (USES_THIS, object, Some(object)),
                (USES_THIS, value::UNDEFINED, None),
                (USES_THIS, value::int32(3), None),
                (0, value::int32(3), Some(value::EMPTY)),
            ] {
                let mut callee = DynamicCallee::new(1, CAN_INLINE_CALL | flags);
                let arguments = [callee.value(), value::int32(7), this_value];
                let (_, machine) = run_dynamic(&mut callee, &arguments, false);
                let state = machine.state.borrow();
                match expected {
                    Some(expected) => {
                        let frame = &state.direct.callee_frames[0];
                        assert_eq!(frame[120 / 8], expected);
                        assert_eq!(
                            frame[FRAME_HEADER_WORDS + crate::bytecode::THIS_VALUE_REGISTER as usize],
                            expected
                        );
                    }
                    None => assert_eq!(state.generic_calls, 1),
                }
            }
        }

        #[test]
        fn other_callees_take_the_generic_path() {
            for (flags, function_flags) in [
                (CAN_INLINE_CALL | STRICT, 0),
                (STRICT, ECMASCRIPT_FUNCTION_FLAG),
                (CAN_INLINE_CALL | NEEDS_ENVIRONMENT, ECMASCRIPT_FUNCTION_FLAG),
            ] {
                let mut callee = DynamicCallee::new(1, flags);
                callee.function[1] = u64::from(function_flags);
                let arguments = [callee.value(), value::int32(7), value::NULL];
                let (result, machine) = run_dynamic(&mut callee, &arguments, false);
                let state = machine.state.borrow();
                assert_eq!((state.generic_calls, state.direct.callee_frames.len()), (1, 0));
                assert_ne!(result, returned(value::int32(99)));
            }
            let mut callee = DynamicCallee::new(1, CAN_INLINE_CALL);
            let (_, machine) = run_dynamic(&mut callee, &[value::int32(1), value::int32(7), value::NULL], false);
            assert_eq!(machine.state.borrow().generic_calls, 1);
        }

        #[test]
        fn dynamic_callees_without_jit_code_are_finished_by_the_runtime() {
            let mut callee = DynamicCallee::new(1, CAN_INLINE_CALL);
            callee.callee.set_entry(not_compiled_call_entry as *const () as u64);
            let arguments = [callee.value(), value::int32(7), value::NULL];
            let (result, machine) = run_dynamic(&mut callee, &arguments, false);
            assert_eq!(result, returned(value::int32(6)));
            assert_eq!(machine.state.borrow().direct.finishes, [RESUME]);
        }

        #[test]
        fn dynamic_callees_enter_slot_zero_through_slots_they_do_not_own() {
            // Slot 2 has compiled code, but it belongs to another executable:
            // the call enters slot 0, which has the interpreter run the frame.
            let mut callee = DynamicCallee::new(1, CAN_INLINE_CALL);
            let table = &mut callee.callee.entry_table;
            table[0] = not_compiled_call_entry as *const () as u64;
            table[2] = callee_entry as *const () as u64;
            table[ENTRY_TABLE_OWNERS_WORD + 2] = 0xbad0;
            callee.callee.executable[EXECUTABLE_JIT_ENTRY_SLOT_WORD] = 2;
            let arguments = [callee.value(), value::int32(7), value::NULL];
            let (result, machine) = run_dynamic(&mut callee, &arguments, false);
            assert_eq!(result, returned(value::int32(6)));
            let state = machine.state.borrow();
            assert_eq!(state.direct.finishes, [RESUME]);
            assert!(state.direct.callee_frames.is_empty());
        }

        /// A fake `libjs_jit_prepare_call_environment`.
        extern "C" fn prepare_call_environment(vm: *mut u64, function: u64, this_argument: u64) -> JitResult {
            let mut state = test_state(vm).borrow_mut();
            state.direct.prepared.push((function, this_argument));
            let (value, status) = state.direct.prepared_environment;
            JitResult { value, status }
        }

        #[test]
        fn callees_that_need_an_environment_get_it_from_the_runtime() {
            const ENVIRONMENT: u64 = 0xe9e9_0000;
            for (prepared, expected) in [((ENVIRONMENT, value::int32(5)), true), ((0, 0), false)] {
                let mut callee = DynamicCallee::new(1, CAN_INLINE_CALL | NEEDS_ENVIRONMENT | USES_THIS);
                let arguments = [callee.value(), value::int32(7), value::NULL];
                let (snapshot, compiled) = compile_program_with(&caller(), caller_layout(), &[], |snapshot| {
                    dynamic_runtime(&mut snapshot.runtime, &callee);
                    snapshot.runtime.dynamic_calls.prepare_call_environment =
                        prepare_call_environment as *const () as u64;
                    install_call_stub(&mut snapshot.runtime);
                    snapshot.executables[0].feedback.call = vec![CallFeedbackSnapshot {
                        target: Some(CellId(CALL_FUNCTION)),
                        flags: 1,
                        ..CallFeedbackSnapshot::default()
                    }];
                });
                let mut machine = Machine::new(caller_layout(), &arguments);
                let stack = callee.callee.interpreter_stack.as_mut_ptr() as u64;
                machine.vm[3] = stack;
                machine.vm[4] = stack + 8 * callee.callee.interpreter_stack.len() as u64;
                callee.callee.executable[EXECUTABLE_TEST_STATE_WORD] =
                    std::ptr::from_ref::<RefCell<TestState>>(&machine.state) as u64;
                {
                    let mut state = machine.state.borrow_mut();
                    state.direct.callee_result = Some((value::int32(99), RETURNED));
                    state.direct.prepared_environment = prepared;
                }
                let result = machine.run(&snapshot, &compiled);
                let state = machine.state.borrow();
                assert_eq!(state.direct.prepared, [(callee.function.as_ptr() as u64, value::NULL)]);
                if expected {
                    assert_eq!(result, returned(value::int32(99)));
                    let frame = &state.direct.callee_frames[0];
                    assert_eq!(
                        (frame[8 / 8], frame[96 / 8]),
                        (ENVIRONMENT, ENVIRONMENT),
                        "environments"
                    );
                    assert_eq!(frame[16 / 8], FUNCTION_WORDS[1], "private environment");
                    assert_eq!(frame[120 / 8], value::int32(5), "this");
                    assert_eq!(
                        frame[FRAME_HEADER_WORDS + crate::bytecode::THIS_VALUE_REGISTER as usize],
                        value::int32(5)
                    );
                    assert_eq!(state.generic_calls, 0);
                } else {
                    assert_eq!((state.generic_calls, state.direct.callee_frames.len()), (1, 0));
                }
                assert_eq!(machine.vm[3], stack);
            }
        }

        #[test]
        fn callees_with_a_call_environment_template_get_it_inline() {
            // A fake template (size class, cell size, binding values offset,
            // whether calls bind `this`, then six words) and heap, and the
            // environment's fields: binding values at 16, outer environment at
            // 24, function object at 32 and this value at 40.
            struct Case {
                strict: bool,
                binds_this: bool,
                this_value: u64,
                template: bool,
                free_cell: bool,
                inline: bool,
            }
            let run = |case: Case| {
                let flags = CAN_INLINE_CALL | NEEDS_ENVIRONMENT | USES_THIS | if case.strict { STRICT } else { 0 };
                let mut callee = DynamicCallee::new(1, flags);
                let mut heap = vec![100u64, 10_000, 1000];
                let block = FakeBlock::new(8, 0x5a5a);
                let mut free_list = vec![if case.free_cell {
                    block.cell().as_ptr() as u64
                } else {
                    0
                }];
                // The free lists of size classes 0 to 3: the template's size
                // class 6 masks to 2.
                let free_lists = vec![0, 0, free_list.as_mut_ptr() as u64, 0];
                let mut template = vec![6, 64, if case.inline { 48 } else { 0 }, u64::from(case.binds_this)];
                template.extend([0x1000, 0x1001, 0, 0, 0, 0]);
                callee
                    .shared_data
                    // NB: Bits above the heap region, which the stub drops.
                    .push(if case.template {
                        (0xbad << 48) | template.as_ptr() as u64
                    } else {
                        0
                    });
                callee.function[2] = callee.shared_data.as_ptr() as u64;
                let arguments = [callee.value(), value::int32(7), case.this_value];
                let (snapshot, compiled) = compile_program_with(&caller(), caller_layout(), &[], |snapshot| {
                    dynamic_runtime(&mut snapshot.runtime, &callee);
                    let layout = &mut snapshot.runtime.dynamic_calls;
                    layout.prepare_call_environment = prepare_call_environment as *const () as u64;
                    layout.shared_data_call_environment_template = 16;
                    layout.call_environment_template_size_class = 0;
                    layout.function_environment_free_lists = free_lists.as_ptr() as u64;
                    layout.function_environment_size_class_mask = 3;
                    layout.call_environment_template_cell_size = 8;
                    layout.call_environment_template_binding_values_offset = 16;
                    layout.call_environment_template_binds_this = 24;
                    layout.call_environment_template_words = 32;
                    layout.function_environment_words = 6;
                    layout.function_environment_binding_values = 16;
                    layout.function_environment_outer = 24;
                    layout.function_environment_function_object = 32;
                    layout.function_environment_this_value = 40;
                    let allocation = &mut snapshot.runtime.object_allocation;
                    allocation.heap = heap.as_mut_ptr() as u64;
                    allocation.heap_allocated_bytes_offset = 0;
                    allocation.heap_threshold_offset = 8;
                    allocation.heap_total_allocated_bytes_offset = 16;
                    allocation.freelist_link_mask = FakeBlock::LINK_MASK;
                    install_call_stub(&mut snapshot.runtime);
                    snapshot.executables[0].feedback.call = vec![CallFeedbackSnapshot {
                        target: Some(CellId(CALL_FUNCTION)),
                        flags: 1,
                        ..CallFeedbackSnapshot::default()
                    }];
                });
                let mut machine = Machine::new(caller_layout(), &arguments);
                let stack = callee.callee.interpreter_stack.as_mut_ptr() as u64;
                machine.vm[3] = stack;
                machine.vm[4] = stack + 8 * callee.callee.interpreter_stack.len() as u64;
                callee.callee.executable[EXECUTABLE_TEST_STATE_WORD] =
                    std::ptr::from_ref::<RefCell<TestState>>(&machine.state) as u64;
                {
                    let mut state = machine.state.borrow_mut();
                    state.direct.callee_result = Some((value::int32(99), RETURNED));
                    state.direct.prepared_environment = (0xe9e9_0000, value::int32(5));
                }
                let result = machine.run(&snapshot, &compiled);
                assert_eq!(result, returned(value::int32(99)));
                let state = machine.state.borrow();
                let frame = state.direct.callee_frames[0].clone();
                let from_runtime = !state.direct.prepared.is_empty();
                (callee, frame, from_runtime, heap, block)
            };

            let inline = |strict, binds_this, this_value, inline| Case {
                strict,
                binds_this,
                this_value,
                template: true,
                free_cell: true,
                inline,
            };
            for (case, this) in [
                (inline(true, true, value::int32(3), true), value::int32(3)),
                (inline(true, false, value::int32(3), false), value::EMPTY),
            ] {
                let binds_this = case.binds_this;
                let in_cell = case.inline;
                let (callee, frame, from_runtime, heap, block) = run(case);
                assert!(!from_runtime);
                let cell = block.cell();
                let address = cell.as_ptr() as u64;
                assert_eq!((frame[1], frame[12]), (address, address), "environments");
                assert_eq!(frame[120 / 8], this, "this");
                assert_eq!(&cell[..2], &[0x1000, 0x1001]);
                assert_eq!(cell[2], if in_cell { address + 48 } else { 0 });
                assert_eq!(cell[3], FUNCTION_WORDS[0], "outer environment");
                assert_eq!(cell[4], callee.function.as_ptr() as u64, "function object");
                assert_eq!(cell[5], if binds_this { value::int32(3) } else { 0 });
                assert_eq!(heap, [164, 10_000, 1064]);
            }

            // Sloppy callees binding primitives, callees without a template and
            // empty free lists get their environment from the runtime.
            for case in [
                inline(false, true, value::int32(3), true),
                Case {
                    template: false,
                    ..inline(true, true, value::int32(3), true)
                },
                Case {
                    free_cell: false,
                    ..inline(true, true, value::int32(3), true)
                },
            ] {
                let (_, frame, from_runtime, heap, _) = run(case);
                assert!(from_runtime);
                assert_eq!((frame[1], frame[120 / 8]), (0xe9e9_0000, value::int32(5)));
                assert_eq!(heap[0], 100);
            }
        }

        #[test]
        fn calls_through_function_prototype_call_call_the_this_value() {
            // `callee.call(a2, a1, 10)` with Function.prototype.call in a0 and
            // the called function in a2: it gets a1 as its `this` and 10.
            let mut callee = DynamicCallee::new(2, CAN_INLINE_CALL | STRICT | USES_THIS);
            let call_function = (u64::from(value::OBJECT_TAG) << 48) | CALL_FUNCTION;
            let arguments = [call_function, value::int32(7), callee.value()];
            let (result, machine) = run_dynamic(&mut callee, &arguments, true);
            assert_eq!(result, returned(value::int32(99)));
            let state = machine.state.borrow();
            let frame = &state.direct.callee_frames[0];
            assert_eq!(frame[120 / 8], value::int32(7), "this");
            assert_eq!((frame[136 / 8] as u32), 1, "passed argument count");
            assert_eq!(
                frame[FRAME_HEADER_WORDS + 9..FRAME_HEADER_WORDS + 11],
                [value::int32(10), value::UNDEFINED]
            );

            // Other callees at such a site take the generic path.
            let mut callee = DynamicCallee::new(2, CAN_INLINE_CALL | STRICT);
            let arguments = [callee.value(), value::int32(7), callee.value()];
            let (_, machine) = run_dynamic(&mut callee, &arguments, true);
            assert_eq!(machine.state.borrow().generic_calls, 1);
        }

        /// A fake raw native function: returns the sum of its `this` value
        /// and its arguments (as int32s), and records its frame.
        extern "C" fn fake_native(vm: *mut u64) -> JitResult {
            // SAFETY: The running frame is the native frame on the fake
            // interpreter stack.
            unsafe {
                let frame = *vm as *mut u64;
                let slot_count = *frame.cast::<u8>().add(148).cast::<u32>() as usize;
                let copy = std::slice::from_raw_parts(frame, FRAME_HEADER_WORDS + slot_count).to_vec();
                let mut sum = int(*frame.add(120 / 8));
                for index in 0..slot_count {
                    sum = sum.wrapping_add(int(*frame.add(FRAME_HEADER_WORDS + index)));
                }
                test_state(vm).borrow_mut().direct.callee_frames.push(copy);
                JitResult {
                    value: value::int32(sum),
                    status: 0,
                }
            }
        }

        #[test]
        fn dynamic_calls_call_raw_native_functions_in_their_frame() {
            const RAW_NATIVE_FLAG: u16 = 1 << 7;
            for through_call in [false, true] {
                let mut callee = DynamicCallee::new(0, 0);
                let shape = vec![REALM];
                // Native function index 1, with a bit above the table's index mask that the call stub drops.
                let native = vec![shape.as_ptr() as u64, u64::from(RAW_NATIVE_FLAG), 0x1_0001];
                let table = vec![0, 0, fake_native as *const () as u64, 0];
                let native_value = (u64::from(value::OBJECT_TAG) << 48) | native.as_ptr() as u64;
                let call_function = (u64::from(value::OBJECT_TAG) << 48) | CALL_FUNCTION;
                // `a0.call(a2, a1, 10)`: with Function.prototype.call in a0,
                // the native function gets this = a1 and 10.
                let arguments = if through_call {
                    [call_function, value::int32(7), native_value]
                } else {
                    [native_value, value::int32(7), value::int32(1)]
                };
                let (snapshot, compiled) = compile_program_with(&caller(), caller_layout(), &[], |snapshot| {
                    dynamic_runtime(&mut snapshot.runtime, &callee);
                    snapshot.runtime.raw_native_exception = 1;
                    let layout = &mut snapshot.runtime.dynamic_calls;
                    layout.object_flag_is_raw_native_function = RAW_NATIVE_FLAG;
                    layout.raw_native_function_index = 16;
                    layout.vm_native_function_table = (8 * VM_NATIVE_FUNCTION_TABLE_WORD) as u32;
                    layout.native_function_table_index_mask = 0xFFFF;
                    layout.native_function_table_entry_size = 16;
                    layout.native_function_table_entry_function = 0;
                    install_call_stub(&mut snapshot.runtime);
                    snapshot.executables[0].feedback.call = vec![CallFeedbackSnapshot {
                        target: Some(CellId(CALL_FUNCTION)),
                        flags: if through_call { 0 } else { 1 },
                        target_intrinsic: through_call.then_some(crate::snapshot::Intrinsic::FunctionPrototypeCall),
                        ..CallFeedbackSnapshot::default()
                    }];
                });
                let mut machine = Machine::new(caller_layout(), &arguments);
                let stack = callee.callee.interpreter_stack.as_mut_ptr() as u64;
                machine.vm[3] = stack;
                machine.vm[4] = stack + 8 * callee.callee.interpreter_stack.len() as u64;
                machine.vm[VM_NATIVE_FUNCTION_TABLE_WORD] = table.as_ptr() as u64;
                let result = machine.run(&snapshot, &compiled);
                // this + 10, plus a1 (7) when called directly with this = 1.
                assert_eq!(result, returned(value::int32(if through_call { 17 } else { 18 })));
                let state = machine.state.borrow();
                assert_eq!(state.generic_calls, 0);
                let frame = &state.direct.callee_frames[0];
                assert_eq!(frame[64 / 8], native.as_ptr() as u64, "function");
                assert_eq!(frame[72 / 8], REALM);
                assert_eq!(frame[32 / 8], 0, "no executable");
                assert_eq!(machine.vm[0], machine.frame.as_ptr() as u64);
                assert_eq!(machine.vm[3], stack);
                let _ = &callee.shape;
            }
        }

        #[test]
        fn direct_calls_enter_compiled_callees() {
            // `function (a) { return a; }`, compiled for real.
            let callee_program = assemble(|_| vec![Instruction::Enter, Instruction::Return { value: a(0) }]);
            let callee_layout = test_layout();
            let (_, callee_code) = compile_program(&callee_program, callee_layout, &[]);
            let code = crate::asm::executable_code::ExecutableCode::new(&callee_code.code);
            let mut callee = Callee::new();
            // SAFETY: Compiled code implements the JIT entry ABI.
            let entry: extern "C" fn(*mut u64, *mut u64) -> JitResult = unsafe { code.function() };
            callee.set_entry(entry as *const () as u64);
            let mut target = callee.target(true, false);
            target.function.formal_parameter_count = callee_layout.number_of_arguments;
            target.registers_and_locals_count = callee_layout.registers_and_locals_count;
            target.registers_and_locals_and_constants_count = callee_layout.arguments_base();
            let arguments = [callee.function_value(), value::int32(7), value::NULL];
            let (result, machine, _) = run_direct(&mut callee, target, &arguments, |_| {});
            assert_eq!(result, returned(value::int32(7)));
            assert_eq!(machine.vm[0], machine.frame.as_ptr() as u64);
        }
    }
}

#[test]
fn aarch64_direct_calls_disassemble() {
    use crate::builder::tests::inlining::call;
    use crate::snapshot::CallFeedbackSnapshot;
    use crate::snapshot::CellId;
    use crate::snapshot::DirectCallTarget;
    use crate::snapshot::InlinedFunctionSnapshot;

    let caller = assemble(|_| {
        vec![
            Instruction::Enter,
            call(r(5), r(6), r(7), vec![c(1)]),
            Instruction::Return { value: r(5) },
        ]
    });
    let mut snapshot = crate::builder::tests::snapshot_for(&caller, test_layout());
    snapshot.runtime = runtime();
    let target = DirectCallTarget {
        function: InlinedFunctionSnapshot {
            function: CellId(0x5000),
            formal_parameter_count: 2,
            strict: false,
            uses_this: true,
            global_this: CellId(0x6000),
            realm: CellId(0x8000),
            shared_data: crate::snapshot::CellId(0x5d00),
        },
        executable: CellId(0x7000),
        entry: 0x7100,
        registers_and_locals_count: 7,
        registers_and_locals_and_constants_count: 9,
        function_fields: FunctionFrameFields::default(),
        environment: None,
        closures: false,
    };
    snapshot.executables[0].feedback.call = vec![CallFeedbackSnapshot {
        target: Some(target.function.function),
        direct_call: Some(target),
        ..CallFeedbackSnapshot::default()
    }];
    let compiled = compile_for::<crate::asm::aarch64::MacroAssembler>(&snapshot, &|_| true).unwrap();
    let text = crate::asm::disassembler::aarch64(&compiled.code[..compiled.data_offset as usize]).join("\n");
    // The callee's entry, the runtime's finish helper, and the generic path.
    assert!(text.matches("blr").count() >= 3, "{text}");
}

#[test]
fn aarch64_inlined_calls_disassemble() {
    use crate::builder::tests::inlining::call;
    use crate::builder::tests::inlining::function;
    use crate::builder::tests::inlining::snapshot_with_callee;

    let caller = assemble(|_| {
        vec![
            Instruction::Enter,
            call(r(5), a(0), c(0), vec![c(1)]),
            Instruction::Return { value: r(5) },
        ]
    });
    let callee = assemble(|_| {
        vec![
            Instruction::Enter,
            Instruction::Exp {
                arith_feedback: 0,
                dst: r(5),
                lhs: a(0),
                rhs: a(0),
            },
            Instruction::Return { value: r(5) },
        ]
    });
    let mut snapshot = snapshot_with_callee(&caller, &callee, function(true, false));
    snapshot.runtime = runtime();
    let compiled = compile_for::<crate::asm::aarch64::MacroAssembler>(&snapshot, &|_| true).unwrap();
    let text = crate::asm::disassembler::aarch64(&compiled.code[..compiled.data_offset as usize]).join("\n");
    // The slow path, the exit stub and the leave stub are the only calls:
    // the callee's frame is published and popped inline.
    assert_eq!(text.matches("blr").count(), 3, "{text}");
}

#[cfg(target_arch = "x86_64")]
mod lowerings;
