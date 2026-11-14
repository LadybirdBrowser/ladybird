/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The translator from the state of compiled code at one of its sites to the interpreter's state.
//!
//! A site is a point of compiled code with a chain of frame states, innermost first (see
//! `libjs_jit::code::Site`): an exit, a leave (a slow path that did not continue in compiled code), a slow path or
//! call in an inlined callee that runs in the frames of the inlined calls it is in, or a call in an inlined callee that
//! runs without them. The outermost frame state is the compiled function's own frame; the others are frames of inlined
//! calls, which exist only as values of compiled code until the translator writes them.
//!
//! The translator has three modes, all driven by the site's frame states:
//! - Full writes interpreter frames: the compiled function's frame and the frames of the inlined calls, pushed where
//!   they are not on the interpreter stack yet (`exit()`, `leave()`, `materialize_call_site_frames()`).
//! - Header pushes the frames of the inlined calls with their headers only, for slow paths and calls that take their
//!   operands as values (`publish()`).
//! - View describes the frames of the inlined calls of a call site while the call runs, for stack walks
//!   (`call_site_frames()`, `call_site_frame_arguments()`).

use core::ptr::NonNull;
use std::collections::HashMap;

use libjs_abi::register;
use libjs_jit::code::{
    CALLEE_SLOT, FrameState, Repr, ResumeMode, Site, SiteKind, ValueLocation, VirtualObjectDescriptorKind,
};
use libjs_runtime_macros::Trace;

use super::allocation::clone_function;
use super::code::{
    CompileState, EXIT_COUNT_BEFORE_DISCARD, FRUITLESS_LOOP_CHECKS_AFTER_EXIT, InlinedFunction, JitCode,
};
use super::{executable_of, frame_of, frames_above};
use crate::bytecode::executable::Executable;
use crate::bytecode::instruction::{OpCode, instruction_length_from_bytes};
use crate::bytecode::instruction_at;
use crate::bytecode::op;
use crate::gc::defer_gc::{DeferGc, Deferred};
use crate::gc::root::MarkedVec;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::execution_context::ExecutionContext;
use crate::layout::function_object::FunctionObject;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{create_mapped_arguments_object, create_unmapped_arguments_object};
use crate::runtime::ecmascript_function_object::as_ecmascript_function_object;

/// What an exit stub saves on the stack before calling `libjs_jit_exit()`: every GPR by hardware encoding, then every
/// FPR (see `libjs_jit::codegen::register_dump_counts()`, which `RegisterDump::assert_layout()` checks against).
#[repr(C)]
pub struct RegisterDump {
    pub gprs: [u64; RegisterDump::GPR_COUNT],
    pub fprs: [u64; RegisterDump::FPR_COUNT],
}

impl RegisterDump {
    #[cfg(target_arch = "x86_64")]
    const GPR_COUNT: usize = 16;
    #[cfg(target_arch = "x86_64")]
    const FPR_COUNT: usize = 16;
    /// rbp.
    #[cfg(target_arch = "x86_64")]
    const FRAME_POINTER: usize = 5;

    #[cfg(target_arch = "aarch64")]
    const GPR_COUNT: usize = 31;
    #[cfg(target_arch = "aarch64")]
    const FPR_COUNT: usize = 32;
    /// x29.
    #[cfg(target_arch = "aarch64")]
    const FRAME_POINTER: usize = 29;

    /// Checks that the dump has room for every register exit stubs save, and the frame pointer where they save it.
    pub(super) fn assert_layout() {
        type MacroAssembler = libjs_jit::asm::MacroAssembler;
        assert_eq!(
            libjs_jit::codegen::register_dump_counts::<MacroAssembler>(),
            (RegisterDump::GPR_COUNT as u32, RegisterDump::FPR_COUNT as u32)
        );
        assert_eq!(
            usize::from(<MacroAssembler as libjs_jit::asm::PortableMacroAssembler>::FRAME_POINTER.0),
            RegisterDump::FRAME_POINTER
        );
    }

    /// The register dump of the JIT frame at `frame_pointer` of `code`, which holds the values of a publication while
    /// its slow path runs (see `libjs_jit::code::CompiledCode::register_dump_frame_offset`).
    fn in_jit_frame(code: &JitCode, frame_pointer: u64) -> RegisterDump {
        let address = frame_pointer.wrapping_add_signed(i64::from(code.register_dump_frame_offset())) as *const Self;
        // SAFETY: The JIT frame is on the stack, suspended in the slow path the publication was for.
        unsafe { address.read() }
    }

    /// A dump for reading the values of a call site from the stack slots of the JIT frame at `frame_pointer`, which is
    /// all a call site's values are in while the call runs.
    fn of_stack_slots(frame_pointer: u64) -> RegisterDump {
        let mut gprs = [0; RegisterDump::GPR_COUNT];
        gprs[RegisterDump::FRAME_POINTER] = frame_pointer;
        RegisterDump {
            gprs,
            fprs: [0; RegisterDump::FPR_COUNT],
        }
    }
}

/// Reads a value that is not an object the code never created, with the compiled function's frame at `root`.
fn read_location(location: ValueLocation, dump: &RegisterDump, root: NonNull<ExecutionContext>) -> Value {
    let (bits, repr) = match location {
        ValueLocation::Constant(bits) => return Value::from_bits(bits),
        // NB: Nothing writes such a slot, so it still holds the value while frames are written.
        ValueLocation::Frame(slot) => return frame_of(root).slots()[slot as usize].get(),
        _ => read_bits(location, dump),
    };
    match repr {
        Repr::Tagged => Value::from_bits(bits),
        Repr::Int32 => Value::from_i32(bits as i32),
        Repr::Float64 => Value::from_f64(f64::from_bits(bits)),
        Repr::Bool => Value::from_bool(bits as u32 != 0),
        Repr::Pointer => unreachable!("only virtual closures have cell addresses in frame states"),
    }
}

/// Reads the cell address at `location`, null or the address of a live cell, which only the environments of a closure
/// the code never created are (see `libjs_jit::code::VirtualObjectDescriptorKind::Closure`).
fn read_cell_address<T>(location: ValueLocation, dump: &RegisterDump) -> Option<Gc<T>> {
    // NB: Closures that inlined callees create have the environments of the callee, which the code embeds.
    let bits = match location {
        ValueLocation::Constant(bits) => bits,
        _ => {
            let (bits, repr) = read_bits(location, dump);
            assert_eq!(repr, Repr::Pointer);
            bits
        }
    };
    // SAFETY: The code keeps the cell alive until the site, which is where it is read.
    NonNull::new(bits as *mut T).map(|cell| unsafe { Gc::from_non_null(cell) })
}

/// The bits and representation of a value in a register or stack slot.
fn read_bits(location: ValueLocation, dump: &RegisterDump) -> (u64, Repr) {
    match location {
        ValueLocation::Register(encoding, repr) => {
            let bits = if repr == Repr::Float64 {
                dump.fprs[usize::from(encoding)]
            } else {
                dump.gprs[usize::from(encoding)]
            };
            (bits, repr)
        }
        ValueLocation::Stack(offset, repr) => {
            let frame_pointer = dump.gprs[RegisterDump::FRAME_POINTER];
            let address = frame_pointer.wrapping_add_signed(i64::from(offset)) as *const u64;
            // SAFETY: The JIT frame of the site is still on the stack, and the slot holds the value.
            (unsafe { address.read() }, repr)
        }
        ValueLocation::Constant(_) | ValueLocation::Frame(_) => {
            unreachable!("only registers and stack slots hold bits")
        }
        ValueLocation::ArgumentsObject { .. } | ValueLocation::VirtualObject(_) => {
            unreachable!("objects are created by SiteValues")
        }
    }
}

/// The values of a site of `code` for the compiled function's frame `root`, creating the objects the code never
/// allocated once each, so that every slot referring to one holds the same object: the one created for the frame
/// before, if any (see `JitCode::materialized_object()`). Its users defer collections until the frames hold the values.
struct SiteValues<'a> {
    vm: &'a Vm,
    code: &'a JitCode,
    root: NonNull<ExecutionContext>,
    site: &'a Site,
    dump: &'a RegisterDump,
    created: Deferred<'a, HashMap<u32, Gc<Object>>>,
    /// Whether the objects created are remembered for the frame, for a frame that continues in compiled code until it
    /// leaves it, where the objects must be the same.
    remember: bool,
    /// For frames of inlined calls written while compiled code keeps running: the frame pointer of the JIT frame, for
    /// the markers that argument slots of objects the code never allocated get (see `argument_value()`).
    deferred_arguments: Option<u64>,
}

impl<'a> SiteValues<'a> {
    fn new(
        vm: &'a Vm,
        code: &'a JitCode,
        root: NonNull<ExecutionContext>,
        site: &'a Site,
        dump: &'a RegisterDump,
        defer_gc: &'a DeferGc<'_>,
    ) -> Self {
        Self {
            vm,
            code,
            root,
            site,
            dump,
            created: defer_gc.cover(HashMap::new()),
            remember: false,
            deferred_arguments: None,
        }
    }

    fn value(&mut self, location: ValueLocation) -> Value {
        let ValueLocation::VirtualObject(index) = location else {
            return read_location(location, self.dump, self.root);
        };
        if let Some(object) = self.created.get(&index) {
            return Value::from_object(*object);
        }
        let description = &self.site.objects[index as usize];
        // NB: An object created for the frame before is the one the frame has, with whatever happened to it since.
        if let Some(object) = self
            .code
            .materialized_object(self.root.as_ptr(), description.allocation)
        {
            self.created.insert(index, object);
            return Value::from_object(object);
        }
        let shape = match description.kind {
            VirtualObjectDescriptorKind::Object { shape } => shape,
            VirtualObjectDescriptorKind::Closure { sample } => {
                // SAFETY: The code embeds the sample, which keeps it alive.
                let sample = unsafe { Gc::from_non_null(NonNull::new(sample.0 as *mut _).expect("a sample")) };
                let closure = clone_function(
                    self.vm,
                    sample,
                    read_cell_address(description.properties[0], self.dump),
                    read_cell_address(description.properties[1], self.dump),
                )
                .upcast::<Object>();
                self.created.insert(index, closure);
                if self.remember {
                    self.code
                        .remember_materialized_object(self.root.as_ptr(), description.allocation, closure);
                }
                return Value::from_object(closure);
            }
        };
        // SAFETY: The code embeds the shape, which keeps it alive.
        let shape = unsafe { Gc::from_non_null(NonNull::new(shape.0 as *mut _).expect("a shape")) };
        let object = Object::create_with_premade_shape(self.vm, shape);
        // NB: The object exists before its properties, which may refer to it.
        self.created.insert(index, object);
        if self.remember {
            self.code
                .remember_materialized_object(self.root.as_ptr(), description.allocation, object);
        }
        let property_count = shape.property_count();
        for index in 0..property_count {
            let value = match description.properties.get(index as usize) {
                Some(location) => self.value(*location),
                None => Value::UNDEFINED,
            };
            object.initialize_direct(index, value);
        }
        Value::from_object(object)
    }

    /// The value of the argument slot `slot` at `location` of the frame of `frame_state`, a frame of an inlined call
    /// written while compiled code keeps running: a marker for an object the code never allocated that the legacy
    /// `arguments` of the frame's function shows, which code reading them creates (see `resolve_deferred_arguments()`),
    /// so that nothing creates it while nothing reads it. Compiled code leaves after the calls where they may be read
    /// (see `libjs_jit::ir::Op::AssumeLegacyArgumentsUnread`).
    fn argument_value(&mut self, slot: u32, location: ValueLocation, frame_state: &FrameState) -> Value {
        match (self.deferred_arguments, location) {
            (Some(frame_pointer), ValueLocation::VirtualObject(_)) if frame_state.legacy_arguments.contains(&slot) => {
                deferred_argument_marker(frame_pointer)
            }
            _ => self.value(location),
        }
    }

    /// The function the frame of `frame_state`, a frame of an inlined call, runs: the closure that was called for
    /// frames of inlined closures, and otherwise the function the code was compiled for.
    fn function(&mut self, code: &JitCode, frame_state: &FrameState) -> InlinedFunction {
        let closure = frame_state
            .values
            .iter()
            .find(|(slot, _)| *slot == CALLEE_SLOT)
            .map(|(_, location)| self.value(*location));
        match closure {
            Some(closure) => InlinedFunction::Ecmascript(
                as_ecmascript_function_object(closure.as_object()).expect("an inlined closure is a function"),
            ),
            None => code
                .snapshot_executable(frame_state.executable)
                .function
                .expect("an inlined executable has a function"),
        }
    }
}

/// The bits of a marker in the argument slot of a frame of an inlined call for an object the code never allocated
/// (see `SiteValues::argument_value()`), with the low half of the JIT frame's frame pointer in the low bits: the empty
/// value with a payload no other empty value has. The garbage collector skips it, and nothing else reads the slot
/// before a leave writes the object there, but code reading the legacy `arguments` of the frame's function.
const DEFERRED_ARGUMENT_MARKER: u64 = Value::EMPTY.0 | (1 << 47);

fn deferred_argument_marker(frame_pointer: u64) -> Value {
    Value::from_bits(DEFERRED_ARGUMENT_MARKER | (frame_pointer & u64::from(u32::MAX)))
}

/// The low half of the frame pointer of the JIT frame that `value` is a marker of an object of (see
/// `DEFERRED_ARGUMENT_MARKER`), if it is one.
fn deferred_argument_frame_pointer(value: Value) -> Option<u32> {
    (value.0 & !u64::from(u32::MAX) == DEFERRED_ARGUMENT_MARKER).then_some(value.0 as u32)
}

/// The slot of the first argument of frames of `executable`.
fn arguments_base_of(executable: &Executable) -> u32 {
    executable.registers_and_locals_count()
        + u32::try_from(executable.constants().len()).expect("the constant count fits in u32")
}

/// Where `frame_state`, of a frame of `executable`, has the argument `argument`, if it lists it.
fn argument_location(frame_state: &FrameState, executable: &Executable, argument: u32) -> Option<ValueLocation> {
    let slot = arguments_base_of(executable) + argument;
    frame_state
        .values
        .iter()
        .find(|(listed, _)| *listed == slot)
        .map(|(_, location)| *location)
}

/// Whether `code` is the code `compiled_executable` has installed. What discarded code learns counts for no later
/// code.
fn is_installed_code(vm: &Vm, compiled_executable: &Executable, code: &JitCode) -> bool {
    compiled_executable.jit_compile_state() == CompileState::Installed
        && compiled_executable
            .jit_code(vm)
            .is_some_and(|installed| core::ptr::eq(installed, code))
}

/// How the frame of a frame state at a call is linked to the frame of its callee.
#[derive(Clone, Copy)]
struct CallLinkage {
    passed_argument_count: u32,
    return_pc: u32,
    dst: u32,
    is_construct: bool,
    /// Whether the call is the super() of a derived constructor, whose new target is the caller's.
    is_super_call: bool,
}

/// The kind of call an instruction that an inlined call is made for makes.
#[derive(Clone, Copy, PartialEq, Eq)]
enum CallKind {
    Call,
    Construct,
    SuperCall,
}

/// The argument count, length and kind of the Call, CallConstruct or SuperCall* instruction at `pc`, or of the call of
/// a getter a GetById instruction or of a setter a PutById instruction makes.
fn call_instruction_shape(executable: &Executable, pc: u32) -> (u32, u32, CallKind) {
    let opcode = executable.bytecode()[pc as usize];
    if opcode == OpCode::GetById as u8 {
        let get = instruction_at::<op::GetById>(executable, pc);
        return (0, get.length(), CallKind::Call);
    }
    if opcode == OpCode::PutById as u8 {
        let put = instruction_at::<op::PutById>(executable, pc);
        return (1, put.length(), CallKind::Call);
    }
    if opcode == OpCode::CallConstruct as u8 {
        let call = instruction_at::<op::CallConstruct>(executable, pc);
        return (u32::from(call.argument_count), call.length(), CallKind::Construct);
    }
    if opcode == OpCode::SuperCall as u8 {
        let call = instruction_at::<op::SuperCall>(executable, pc);
        return (u32::from(call.argument_count), call.length(), CallKind::SuperCall);
    }
    // NB: Frame states at it list how many arguments it passes on.
    if opcode == OpCode::SuperCallWithForwardedArguments as u8 {
        return (0, op::SuperCallWithForwardedArguments::LENGTH, CallKind::SuperCall);
    }
    let call = instruction_at::<op::Call>(executable, pc);
    (u32::from(call.argument_count), call.length(), CallKind::Call)
}

/// The linkage of the frame of `executable` at the call of `frame_state`: its frame continues after the call (or
/// CallConstruct, or the GetById of a getter or the PutById of a setter) instruction once the callee returns into its
/// destination.
fn call_linkage(executable: &Executable, frame_state: &FrameState) -> CallLinkage {
    let ResumeMode::ResumeAfter { dst } = frame_state.mode else {
        unreachable!("frames at calls resume after them");
    };
    let pc = frame_state.pc;
    let (argument_count, length, kind) = call_instruction_shape(executable, pc);
    CallLinkage {
        passed_argument_count: frame_state.passed_argument_count.unwrap_or(argument_count),
        return_pc: pc + length,
        dst,
        is_construct: kind != CallKind::Call,
        is_super_call: kind == CallKind::SuperCall,
    }
}

/// Initializes the registers, locals and constants of a frame like the interpreter's Enter does, except for the
/// `preserved` registers and locals, which hold values already.
fn initialize_frame_preserving(frame: &ExecutionContext, preserved: &[u32]) {
    let executable = executable_of(frame);
    let slots = frame.slots();
    let registers_and_locals_count = executable.registers_and_locals_count() as usize;
    for (index, slot) in slots
        .iter()
        .enumerate()
        .take(registers_and_locals_count)
        .skip(register::RESERVED_REGISTER_COUNT as usize)
    {
        if !preserved.contains(&(index as u32)) {
            slot.set(Value::EMPTY);
        }
    }
    for (slot, constant) in slots[registers_and_locals_count..].iter().zip(executable.constants()) {
        slot.set(*constant);
    }
    frame.frame_initialized.set(true);
}

/// Whether a frame pushed for an inlined call gets its registers, locals and constants.
#[derive(Clone, Copy, PartialEq, Eq)]
enum FrameSlots {
    /// Initialize them like the interpreter's Enter does (Full translation writes them afterwards).
    Initialize,
    /// Leave the frame uninitialized: only its reserved registers and arguments hold values, which is all the garbage
    /// collector visits, and a leave initializes it if the slow path does not continue (Header translation).
    Uninitialized,
}

/// Pushes the frame of an inlined call, linked to the running frame, `caller`, like an interpreter inline call.
fn push_frame(
    vm: &Vm,
    caller: &ExecutionContext,
    callee: InlinedFunction,
    linkage: CallLinkage,
    slots: FrameSlots,
) -> NonNull<ExecutionContext> {
    // NB: JIT code checks at entry that the interpreter stack has room for every frame it may materialize.
    let frame = match callee {
        InlinedFunction::Ecmascript(callee) => {
            // NB: Inlined functions never need a function environment, so pushing their frame does not allocate (and
            //     cannot collect garbage while values of JIT code are in flight).
            assert!(!callee.function_environment_needed());
            // NB: Constructs of class constructors run in inline frames too, but only calls have an inline call
            //     executable.
            let executable = if linkage.is_construct {
                callee.bytecode_executable().expect("a construct runs an executable")
            } else {
                callee.inline_call_executable()
            };
            // NB: The super() of a derived constructor constructs with the derived constructor's new target, like
            //     the interpreter's SuperCall does.
            let new_target = if linkage.is_super_call {
                Some(caller.new_target.get().as_object())
            } else {
                linkage.is_construct.then(|| callee.upcast())
            };
            vm.push_inline_frame_without_this(
                callee,
                executable,
                &[],
                linkage.passed_argument_count,
                linkage.return_pc,
                linkage.dst,
                new_target,
                linkage.is_construct,
            )
        }
        InlinedFunction::Builtin(callee) => {
            assert!(!linkage.is_construct);
            let executable = callee
                .shared_data()
                .executable()
                .expect("an inlined builtin has an executable");
            vm.push_builtin_inline_frame(
                callee,
                executable,
                &[],
                linkage.passed_argument_count,
                linkage.return_pc,
                linkage.dst,
                Value::EMPTY,
            )
        }
    }
    .expect("JIT code made room for its materialized frames");
    let frame_ref = frame_of(frame);
    if slots == FrameSlots::Initialize {
        initialize_frame_preserving(frame_ref, &[]);
    }
    frame_ref.register(register::THIS_VALUE).set(Value::EMPTY);
    frame
}

/// Creates the arguments object of a frame like CreateArguments does.
pub(super) fn create_arguments_object(vm: &Vm, frame: &ExecutionContext, mapped: bool) -> Value {
    let passed_arguments = &frame.arguments()[..frame.passed_argument_count.get() as usize];
    if !mapped {
        return Value::from_object(create_unmapped_arguments_object(vm, passed_arguments));
    }
    // NB: Mapped arguments objects belong to functions with simple parameter lists, whose parameters live in the
    //     function environment, which is their variable environment (the lexical one may be a block's by now).
    let function = frame
        .function
        .get()
        .expect("an arguments object is created for a function");
    let ecmascript_function =
        as_ecmascript_function_object(function).expect("only ECMAScript functions have mapped arguments objects");
    Value::from_object(create_mapped_arguments_object(
        vm,
        function,
        ecmascript_function.mapped_argument_names(),
        passed_arguments,
        frame
            .variable_environment
            .get()
            .expect("a function runs in an environment"),
    ))
}

/// Whether translation writes the destination of a `ResumeAfter` frame state.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Destination {
    /// The instruction did not continue (it threw, or its callee runs in the interpreter and writes the destination
    /// when it returns), so the destination gets the value it had before the instruction, if the frame state lists it.
    WriteOldValue,
    /// The instruction wrote it.
    Keep,
}

/// Writes the values of `frame_state` into `frame`, creating its arguments object once if a value is it, and clears the
/// registers and locals the frame state does not list, which are dead.
fn write_frame_state(
    vm: &Vm,
    frame: &ExecutionContext,
    frame_state: &FrameState,
    values: &mut SiteValues<'_>,
    destination: Destination,
) {
    // NB: The frame holds the slots listed as in it, and the destination of the instruction it resumes after is never
    //     cleared.
    let resume_destination = match frame_state.mode {
        ResumeMode::ResumeAfter { dst } => Some(dst),
        ResumeMode::ResumeAt => None,
    };
    let kept = resume_destination.filter(|_| destination == Destination::Keep);
    // NB: Compiled code initializes its frame lazily, before anything observes the frame (see
    //     passes/frame_initialization.rs), and pushes the frames of inlined calls for slow paths uninitialized. Until
    //     then nothing wrote the frame's registers and locals, so the ones the frame state lists as held in the frame
    //     hold what the interpreter's Enter put there, not what the frame's memory had before.
    if !frame.frame_initialized.get() {
        initialize_frame_preserving(frame, kept.as_slice());
    }
    let slots = frame.slots();
    // NB: Frame states list every live slot, so the others are dead. Clearing them keeps the interpreter from seeing
    //     values the compiled code no longer maintains, and drops references to them. Both lists are in slot order.
    let registers_and_locals_count = executable_of(frame).registers_and_locals_count();
    let mut held = frame_state.in_frame.iter().copied().peekable();
    let mut written = frame_state.values.iter().map(|(slot, _)| *slot).peekable();
    for index in register::RESERVED_REGISTER_COUNT..registers_and_locals_count {
        let is_held = sorted_slots_contain(&mut held, index);
        let is_written = sorted_slots_contain(&mut written, index);
        if !is_held && !is_written && resume_destination != Some(index) {
            slots[index as usize].set(Value::EMPTY);
        }
    }
    // NB: Every slot holding the arguments object the code never created holds the same object, which is created from
    //     the frame's arguments once they are written: frames of inlined calls get theirs from the frame state too.
    let mut arguments_object_slots = Vec::new();
    let arguments_base = arguments_base_of(&executable_of(frame));
    let mut has_deferred_arguments = false;
    for (slot, location) in &frame_state.values {
        // NB: The function object of a frame of an inlined closure is in no slot.
        if *slot == CALLEE_SLOT || Some(*slot) == kept {
            continue;
        }
        if let ValueLocation::ArgumentsObject { mapped } = location {
            arguments_object_slots.push((*slot, *mapped));
            continue;
        }
        let value = if *slot >= arguments_base {
            values.argument_value(*slot, *location, frame_state)
        } else {
            values.value(*location)
        };
        has_deferred_arguments |= deferred_argument_frame_pointer(value).is_some();
        slots[*slot as usize].set(value);
    }
    // NB: A frame with markers among its arguments does not run in the interpreter before a leave writes it again, and
    //     nothing else reads its arguments object.
    if let Some((_, mapped)) = arguments_object_slots.first().filter(|_| !has_deferred_arguments) {
        let arguments_object = create_arguments_object(vm, frame, *mapped);
        for (slot, _) in &arguments_object_slots {
            slots[*slot as usize].set(arguments_object);
        }
    }
}

/// Whether the slots, in slot order, contain `slot`, skipping those below it. Called with ascending slots.
fn sorted_slots_contain(slots: &mut core::iter::Peekable<impl Iterator<Item = u32>>, slot: u32) -> bool {
    while slots.next_if(|listed| *listed < slot).is_some() {}
    slots.next_if_eq(&slot).is_some()
}

/// Whether Full translation writes the compiled function's frame too.
#[derive(Clone, Copy, PartialEq, Eq)]
enum CompiledFunctionFrame {
    /// Write its frame state, and its pc if it is at a call.
    Write,
    /// Leave it alone: compiled code keeps running in it.
    Keep,
}

/// Full translation: pushes the frames of the inlined calls of `site` on top of the interpreter stack, above the
/// compiled function's frame `root`, from the outside in, each linked to its caller like an interpreter inline call, and
/// writes their frame states and program counters. Returns the innermost frame.
fn push_site_frames(
    vm: &Vm,
    code: &JitCode,
    root: NonNull<ExecutionContext>,
    values: &mut SiteValues<'_>,
    compiled_function_frame: CompiledFunctionFrame,
    innermost_destination: Destination,
) -> NonNull<ExecutionContext> {
    let site = values.site;
    let frame_states = &site.frames;
    assert!(!frame_states.is_empty());
    let mut frame = root;
    let mut linkage = None;
    for (index, frame_state) in frame_states.iter().enumerate().rev() {
        let is_root = index + 1 == frame_states.len();
        if is_root {
            assert_eq!(frame_state.executable, 0);
        } else {
            let function = values.function(code, frame_state);
            frame = push_frame(
                vm,
                frame_of(frame),
                function,
                linkage.expect("callers of inlined calls link them"),
                FrameSlots::Initialize,
            );
        }
        let frame_ref = frame_of(frame);
        if !is_root || compiled_function_frame == CompiledFunctionFrame::Write {
            let destination = if index == 0 {
                innermost_destination
            } else {
                Destination::WriteOldValue
            };
            write_frame_state(vm, frame_ref, frame_state, values, destination);
            frame_ref.program_counter.set(frame_state.pc);
        }
        if !is_root {
            let this_value = frame_ref.register(register::THIS_VALUE).get();
            if this_value != Value::EMPTY {
                frame_ref.this_value.set(this_value);
            }
        }
        if index != 0 {
            linkage = Some(call_linkage(&executable_of(frame_ref), frame_state));
        }
    }
    frame
}

/// Full translation of an exit: writes the frame state of the exit into the compiled function's frame `root` (boxing
/// unboxed values), pushes the frames of the inlined calls it is in, sets the program counters and records the exit.
/// The interpreter then continues in the innermost frame.
pub(super) fn exit(vm: &Vm, root: NonNull<ExecutionContext>, code: &JitCode, index: u32, dump: &RegisterDump) {
    let site = code.site(index);
    let SiteKind::Exit(kind) = site.kind else {
        unreachable!("exits are sites of exits");
    };
    // NB: Exits the "stress-exits" option causes teach the executable nothing.
    let is_stress_exit = vm.jit.take_stress_exit();
    // NB: Compiled code publishes its frame lazily, before anything else may see it (see
    //     passes/frame_initialization.rs), so the frame of a direct call may not be the running one yet.
    if vm.running_execution_context() != Some(root) {
        assert_eq!(
            vm.head.running_execution_context.get(),
            frame_of(root).caller_frame.get()
        );
        vm.head.running_execution_context.set(root.as_ptr());
    }
    let compiled_executable = executable_of(frame_of(root));
    assert!(core::ptr::eq(
        compiled_executable.jit_code(vm).expect("the code is attached"),
        code
    ));

    // NB: Nothing may collect the objects created here before the frames hold them.
    let defer_gc = DeferGc::new(vm);
    let mut values = SiteValues::new(vm, code, root, site, dump, &defer_gc);
    let innermost = push_site_frames(
        vm,
        code,
        root,
        &mut values,
        CompiledFunctionFrame::Write,
        Destination::Keep,
    );
    code.forget_materialized_objects(root.as_ptr());
    let frame_state = &site.frames[0];
    let frame_ref = frame_of(innermost);
    let executable = executable_of(frame_ref);
    if let ResumeMode::ResumeAfter { .. } = frame_state.mode {
        // NB: The call already stored its result in its destination slot.
        let bytecode = executable.bytecode();
        let pc = frame_state.pc;
        let length = instruction_length_from_bytes(bytecode[pc as usize], bytecode, pc as usize)
            .expect("the bytecode is valid") as u32;
        frame_ref.program_counter.set(pc + length);
    }

    if vm.jit.options.log_exits {
        // NB: The code is identified by its executable and how often that executable's code was discarded before.
        let stress = if is_stress_exit { " (stress)" } else { "" };
        if executable == compiled_executable {
            eprintln!(
                "JIT exit: {} pc {} {:?}{stress}, code {:p}/{}",
                super::describe_executable(&executable),
                frame_state.pc,
                kind,
                compiled_executable.as_ptr(),
                compiled_executable.jit_discard_count()
            );
        } else {
            eprintln!(
                "JIT exit: {} pc {} {:?}{stress}, inlined into {}, code {:p}/{}",
                super::describe_executable(&executable),
                frame_state.pc,
                kind,
                super::describe_executable(&compiled_executable),
                compiled_executable.as_ptr(),
                compiled_executable.jit_discard_count()
            );
        }
    }
    if is_stress_exit {
        vm.jit.count_coverage(format_args!("stress-exit.{kind:?}"));
        return;
    }
    // NB: Every frame materialized by an exit runs in the interpreter, including intermediate inlined callers whose
    //     own code may continue them after their callees return.
    let mut frame = innermost;
    loop {
        if let Some(frame_code) = executable_of(frame_of(frame)).jit_code(vm) {
            frame_code.forget_inactive_interpreted_frames(vm);
            frame_code.note_interpreted_frame(vm, frame_of(frame), FRUITLESS_LOOP_CHECKS_AFTER_EXIT);
        }
        if frame == root {
            break;
        }
        frame = NonNull::new(frame_of(frame).caller_frame.get()).expect("materialized frames are linked to the root");
    }
    vm.jit.count_coverage(format_args!("exit.{kind:?}"));
    if site.frames.len() > 1 {
        vm.jit.count_coverage("exit.from-inlined-code");
    }
    // NB: Exits in inlined builtins count for the code they were inlined into only.
    // NB: Invalidated code was discarded, and its recompile sees that what it depended on no longer holds. An interrupt
    //     speculated nothing, and nothing runs after it.
    let snapshot_executable = code.snapshot_executable(frame_state.executable);
    let learned = if matches!(
        kind,
        libjs_jit::code::ExitKind::Invalidated | libjs_jit::code::ExitKind::Interrupted
    ) {
        false
    } else if matches!(snapshot_executable.function, Some(InlinedFunction::Builtin(_))) {
        let index = compiled_executable.add_jit_builtin_exit_site((executable.as_ptr() as u64, frame_state.pc, kind));
        index >= code.snapshot_executable(0).known_builtin_exit_sites
    } else {
        // NB: Sites in inlined callees are their executables', but the compiled code learns from them too.
        let new_site = executable.add_jit_exit_site((frame_state.pc, kind)) >= snapshot_executable.known_exit_sites;
        let learned_shapes = kind == libjs_jit::code::ExitKind::BadShape
            && executable.note_jit_shape_exit(frame_state.pc, &snapshot_executable.property_cache_sizes);
        new_site || learned_shapes
    };
    if learned && is_installed_code(vm, &compiled_executable, code) {
        compiled_executable.note_learned_jit_exit_site();
    }

    if kind != libjs_jit::code::ExitKind::Interrupted
        && code.count_exit() >= EXIT_COUNT_BEFORE_DISCARD
        && compiled_executable.jit_compile_state() == CompileState::Installed
    {
        compiled_executable.discard_jit_code(vm);
    }
}

/// Full translation of a leave, which is no exit: compiled code wrote only the slots its slow path reads before calling
/// it, and the slow path did not continue in compiled code. Writes the frame state of the leave into the compiled
/// function's frame `root`, and into the frames compiled code pushed for a slow path in an inlined callee, as far as
/// they are still on the stack: an exception may have unwound inner ones (and maybe the compiled function's, which then
/// needs nothing), and the slow path may have pushed a callee for the interpreter above them. Counts an exception that
/// a handler of an inlined callee caught like an exit.
pub(super) fn leave(vm: &Vm, root: NonNull<ExecutionContext>, code: &JitCode, index: u32, dump: &RegisterDump) {
    let site = code.site(index);
    assert_eq!(site.kind, SiteKind::Leave);
    // NB: The pushed frames are right above the compiled function's, outermost first.
    let Some(frames) = frames_above(vm, root) else {
        code.forget_materialized_objects(root.as_ptr());
        return;
    };
    let frame_states = &site.frames;
    let pushed = frames.len().min(frame_states.len() - 1);

    // NB: Nothing may collect the objects created here before the frames hold them.
    let defer_gc = DeferGc::new(vm);
    let mut values = SiteValues::new(vm, code, root, site, dump, &defer_gc);
    let mut write = |frame: &ExecutionContext, frame_state: &FrameState| {
        let expected = code.snapshot_executable(frame_state.executable).executable;
        assert!(executable_of(frame) == expected);
        write_frame_state(vm, frame, frame_state, &mut values, Destination::WriteOldValue);
    };
    write(frame_of(root), frame_states.last().expect("there is a frame state"));
    for index in 0..pushed {
        // NB: The slow path ran in the innermost frame, whose outputs its frame state lists as in the frame.
        let frame = frames[frames.len() - 1 - index];
        write(frame_of(frame), &frame_states[frame_states.len() - 2 - index]);
    }
    code.forget_materialized_objects(root.as_ptr());

    // NB: A handler of an inlined callee that caught an exception left the callee's frame running at its Catch. The
    //     code was compiled before that handler caught anything. Such catches count like exits, so that the executable
    //     is compiled again once they keep happening, and then calls the callee.
    let handler_of_inlined_callee_caught = pushed == frames.len()
        && frames.first().is_some_and(|running| {
            let running = frame_of(*running);
            executable_of(running).bytecode()[running.program_counter.get() as usize] == OpCode::Catch as u8
        });
    if handler_of_inlined_callee_caught
        && code.count_exit() >= EXIT_COUNT_BEFORE_DISCARD
        && executable_of(frame_of(root)).jit_compile_state() == CompileState::Installed
    {
        vm.jit.count_coverage("discard-from-inlined-catch");
        executable_of(frame_of(root)).discard_jit_code(vm);
    }
}

/// Header translation for a slow path or call in an inlined callee, which takes its operands as values (see
/// `SiteKind::Publish`): pushes the frames of the inlined calls it is in on top of the compiled function's frame `root`,
/// from the outside in, uninitialized, with their function, `this`, arguments and program counter. The slow path then
/// runs in the innermost one, which is the running frame.
pub(super) fn publish(vm: &Vm, root: NonNull<ExecutionContext>, code: &JitCode, index: u32, dump: &RegisterDump) {
    let site = code.site(index);
    assert_eq!(site.kind, SiteKind::Publish);
    assert_eq!(vm.running_execution_context(), Some(root));
    let frame_states = &site.frames;
    assert!(frame_states.len() >= 2);
    // NB: Nothing may collect the objects created here before the frames hold them.
    let defer_gc = DeferGc::new(vm);
    let mut values = SiteValues::new(vm, code, root, site, dump, &defer_gc);
    // NB: The values of the site stay in the register dump of the JIT frame while the slow path runs.
    values.deferred_arguments = Some(dump.gprs[RegisterDump::FRAME_POINTER]);
    // NB: The compiled function's frame is at the call of the outermost inlined call, which stack walks show.
    let root_frame_state = frame_states.last().expect("a frame state");
    frame_of(root).program_counter.set(root_frame_state.pc);
    let mut linkage = call_linkage(&executable_of(frame_of(root)), root_frame_state);
    let mut caller = root;
    for (index, frame_state) in frame_states.iter().enumerate().rev().skip(1) {
        let function = values.function(code, frame_state);
        let frame = push_frame(vm, frame_of(caller), function, linkage, FrameSlots::Uninitialized);
        caller = frame;
        let frame_ref = frame_of(frame);
        let executable = executable_of(frame_ref);
        let arguments_base = arguments_base_of(&executable);
        let slots = frame_ref.slots();
        for (slot, location) in &frame_state.values {
            if *slot == register::THIS_VALUE {
                let this_value = values.value(*location);
                slots[*slot as usize].set(this_value);
                if this_value != Value::EMPTY {
                    frame_ref.this_value.set(this_value);
                }
            } else if *slot != CALLEE_SLOT && *slot >= arguments_base {
                slots[*slot as usize].set(values.argument_value(*slot, *location, frame_state));
            }
        }
        frame_ref.program_counter.set(frame_state.pc);
        if index != 0 {
            linkage = call_linkage(&executable, frame_state);
        }
    }
}

/// Full translation of a call site in an inlined callee: materializes the frames of the inlined calls that the call at
/// `site` of the code running `root` is in, outermost first, with the values the site's frame states give them, which
/// are in the JIT frame at `frame_pointer`. With `callee`, the frame of the call's callee, which compiled code put right
/// above the room it left for them, they go into that room and the callee is linked to the innermost one, which then
/// runs the call; otherwise they go on top of the interpreter stack, and the innermost one is the running frame.
/// Returns the innermost one.
pub(super) fn materialize_call_site_frames(
    vm: &Vm,
    root: NonNull<ExecutionContext>,
    index: u32,
    frame_pointer: u64,
    callee: Option<NonNull<ExecutionContext>>,
) -> NonNull<ExecutionContext> {
    let compiled_executable = executable_of(frame_of(root));
    let code = compiled_executable.jit_code(vm).expect("the code is attached");
    let site = code.site(index);
    assert_eq!(site.kind, SiteKind::Call);
    assert!(site.frames.len() >= 2);

    let stack = vm.interpreter_stack();
    let running = vm.head.running_execution_context.get();
    let top = stack.top.get();
    if let Some(callee) = callee {
        // NB: Compiled code left this room below the callee's frame.
        stack.top.set(
            callee
                .as_ptr()
                .cast::<u8>()
                .wrapping_sub(site.inlined_frame_bytes as usize),
        );
    }
    vm.head.running_execution_context.set(root.as_ptr());

    // NB: Nothing may collect the objects created here before the frames hold them.
    let defer_gc = DeferGc::new(vm);
    let dump = RegisterDump::of_stack_slots(frame_pointer);
    let mut values = SiteValues::new(vm, code, root, site, &dump, &defer_gc);
    // NB: Compiled code keeps running once the call returns, and the call may read the frames' legacy arguments, which
    //     find their values through the site (see `resolve_deferred_arguments()`).
    values.deferred_arguments = Some(frame_pointer);
    code.set_site_index_of_frame(frame_pointer, index);
    let innermost = push_site_frames(
        vm,
        code,
        root,
        &mut values,
        CompiledFunctionFrame::Keep,
        Destination::WriteOldValue,
    );

    if let Some(callee) = callee {
        assert!(stack.top.get() <= callee.as_ptr().cast());
        let linkage = call_linkage(&executable_of(frame_of(innermost)), &site.frames[0]);
        let callee_ref = frame_of(callee);
        callee_ref.caller_frame.set(innermost.as_ptr());
        callee_ref.caller_return_pc.set(linkage.return_pc);
        callee_ref.caller_dst_raw.set(linkage.dst);
        vm.head.running_execution_context.set(running);
        stack.top.set(top);
    }
    innermost
}

/// A frame of an inlined call that a call at a call site of the code running a frame is in, as stack walks see it while
/// the call runs without it (View translation).
#[derive(Clone, Trace)]
pub struct CallSiteFrame {
    pub function: Gc<FunctionObject>,
    pub executable: Gc<Executable>,
    /// The pc of the call the frame makes.
    #[gc(untraced)]
    pub program_counter: u32,
    /// The index of its frame state in the site.
    #[gc(untraced)]
    pub frame_state: usize,
}

/// The frame pointer of a JIT frame from its low half, which a callee frame of a call at a call site in an inlined
/// callee has as its destination: the JIT frame is on this thread's stack, which is smaller than 4 GiB, above the
/// frames of the runtime asking.
pub fn frame_pointer_from_low_half(low_half: u32) -> u64 {
    let marker = 0u8;
    let stack_pointer = core::ptr::from_ref(&marker) as u64;
    let frame_pointer = (stack_pointer & !u64::from(u32::MAX)) | u64::from(low_half);
    if frame_pointer < stack_pointer {
        frame_pointer + (1 << 32)
    } else {
        frame_pointer
    }
}

/// View translation: the frames of the inlined calls that a call at the call site `index` of the code running `frame`
/// is in, innermost first, while it runs without them, with the JIT frame at `frame_pointer`.
pub fn call_site_frames<'vm>(
    vm: &'vm Vm,
    frame: &ExecutionContext,
    index: u32,
    frame_pointer: u64,
) -> MarkedVec<'vm, CallSiteFrame> {
    let compiled_executable = executable_of(frame);
    let code = compiled_executable
        .jit_code(vm)
        .expect("a frame at a call site in an inlined callee runs its JIT code");
    let site = code.site(index);
    assert_eq!(site.kind, SiteKind::Call);
    let dump = RegisterDump::of_stack_slots(frame_pointer);
    let (_, inlined) = site.frames.split_last().expect("a call site has frames");
    let frames = MarkedVec::with_capacity(vm, inlined.len());
    for (index, frame_state) in inlined.iter().enumerate() {
        let snapshot_executable = code.snapshot_executable(frame_state.executable);
        // NB: Frames of inlined closures run the closure that was called. Walks create no objects, and the closure
        //     never is one the code did not create.
        let closure = frame_state
            .values
            .iter()
            .find(|(slot, _)| *slot == CALLEE_SLOT)
            .and_then(|(_, location)| match location {
                ValueLocation::Stack(..) | ValueLocation::Constant(_) | ValueLocation::Frame(_) => {
                    Some(read_location(*location, &dump, NonNull::from(frame)))
                }
                _ => None,
            });
        let function = match closure {
            Some(closure) => as_ecmascript_function_object(closure.as_object())
                .expect("an inlined closure is a function")
                .as_function_object_gc(),
            None => match snapshot_executable
                .function
                .expect("an inlined executable has a function")
            {
                InlinedFunction::Ecmascript(function) => function.as_function_object_gc(),
                InlinedFunction::Builtin(function) => function.as_function_object_gc(),
            },
        };
        frames.push(CallSiteFrame {
            function,
            executable: snapshot_executable.executable,
            program_counter: frame_state.pc,
            frame_state: index,
        });
    }
    frames
}

/// View translation: the passed arguments of the frame of an inlined call that a call at the call site `index` of the
/// code running `frame` is in, the one with the frame state at `frame_state` of the site, while it runs without it,
/// with the JIT frame at `frame_pointer`, for code reading the legacy `arguments` of the frame's function. Objects
/// among them that the code never allocated are created for the frame (see `note_legacy_arguments_read()`).
pub fn call_site_frame_arguments<'d>(
    vm: &Vm,
    frame: &ExecutionContext,
    index: u32,
    frame_pointer: u64,
    frame_state: usize,
    defer_gc: &'d DeferGc<'_>,
) -> Deferred<'d, Vec<Value>> {
    let compiled_executable = executable_of(frame);
    let code = compiled_executable
        .jit_code(vm)
        .expect("a frame at a call site in an inlined callee runs its JIT code");
    let site = code.site(index);
    let caller_frame_state = &site.frames[frame_state + 1];
    let caller_executable = code.snapshot_executable(caller_frame_state.executable).executable;
    let passed_argument_count = call_linkage(&caller_executable, caller_frame_state).passed_argument_count;
    let frame_state = &site.frames[frame_state];
    let executable = code.snapshot_executable(frame_state.executable).executable;
    // NB: The caller keeps collections deferred while it holds the objects created here.
    let dump = RegisterDump::of_stack_slots(frame_pointer);
    let mut values = SiteValues::new(vm, code, NonNull::from(frame), site, &dump, defer_gc);
    values.remember = true;
    let mut reads_objects = false;
    let arguments = defer_gc.cover(
        (0..passed_argument_count)
            .map(|argument| {
                let location = argument_location(frame_state, &executable, argument);
                reads_objects |= matches!(location, Some(ValueLocation::VirtualObject(_)));
                location.map_or(Value::UNDEFINED, |location| values.value(location))
            })
            .collect(),
    );
    if reads_objects {
        note_legacy_arguments_read(vm, frame, code, frame_state);
    }
    arguments
}

/// Replaces the markers among the arguments of `frame` for objects the code never allocated (see
/// `SiteValues::argument_value()`) with the objects, for code reading the legacy `arguments` of the frame's function or
/// the debugger: a frame of an inlined call written while compiled code keeps running has them. The objects are
/// created for the compiled function's frame from the values of the site its JIT frame is suspended at (see
/// `note_legacy_arguments_read()`). Copies of the frame then hold the objects too.
pub fn resolve_deferred_arguments(vm: &Vm, frame: &ExecutionContext) {
    for (argument, slot) in (0..).zip(frame.arguments()) {
        if let Some(low_half) = deferred_argument_frame_pointer(slot.get()) {
            slot.set(deferred_argument(
                vm,
                frame,
                argument,
                frame_pointer_from_low_half(low_half),
            ));
        }
    }
}

/// The object a marker in the argument slot `argument` of `frame` stands for, created for the frame of the compiled
/// function whose JIT frame is at `frame_pointer`.
fn deferred_argument(vm: &Vm, frame: &ExecutionContext, argument: u32, frame_pointer: u64) -> Value {
    // NB: The frames of inlined calls are right above the compiled function's frame, the first below them that JIT
    //     code runs.
    let mut depth = 0;
    let mut root = frame.caller_frame.get();
    loop {
        // SAFETY: A frame's callers outlive it.
        let caller = unsafe { root.as_ref() }.expect("the frames of inlined calls are above the compiled function's");
        if caller.runs_jit_code.get() {
            break;
        }
        depth += 1;
        root = caller.caller_frame.get();
    }
    let root = NonNull::new(root).expect("the compiled function's frame is not null");
    let code = executable_of(frame_of(root))
        .jit_code(vm)
        .expect("a frame suspended in JIT code has it attached");
    let site = code.site(code.site_index_of_frame(frame_pointer));
    let frame_state = &site.frames[site.frames.len() - 2 - depth];
    let executable = executable_of(frame);
    assert!(code.snapshot_executable(frame_state.executable).executable == executable);
    let location =
        argument_location(frame_state, &executable, argument).expect("the frame state lists arguments with markers");
    assert!(matches!(location, ValueLocation::VirtualObject(_)));
    // NB: The objects created here are remembered for the frame, which keeps them alive.
    let defer_gc = DeferGc::new(vm);
    // NB: The values of a publication are in the JIT frame's register dump while its slow path runs; those of a call
    //     site, in stack slots.
    let dump = match site.kind {
        SiteKind::Publish => RegisterDump::in_jit_frame(code, frame_pointer),
        _ => RegisterDump::of_stack_slots(frame_pointer),
    };
    let mut values = SiteValues::new(vm, code, root, site, &dump, &defer_gc);
    values.remember = true;
    let object = values.value(location);
    note_legacy_arguments_read(vm, frame_of(root), code, frame_state);
    object
}

/// Makes `code`, which runs `root`, leave compiled code after the call `root` is suspended in, whose code read objects
/// the code never allocated through the legacy `arguments` of the function of the frame of `frame_state`: the code is
/// invalidated, so that the frame exits after the call (see `libjs_jit::ir::Op::AssumeLegacyArgumentsUnread`) with the
/// objects created for it, and discarded. Its recompiles allocate the objects at that call (see
/// `ExitKind::LegacyArgumentsRead`).
fn note_legacy_arguments_read(vm: &Vm, root: &ExecutionContext, code: &JitCode, frame_state: &FrameState) {
    let snapshot_executable = code.snapshot_executable(frame_state.executable);
    let executable = snapshot_executable.executable;
    let site = (frame_state.pc, libjs_jit::code::ExitKind::LegacyArgumentsRead);
    let learned = executable.add_jit_exit_site(site) >= snapshot_executable.known_exit_sites;
    let compiled_executable = executable_of(root);
    if learned && is_installed_code(vm, &compiled_executable, code) {
        compiled_executable.note_learned_jit_exit_site();
    }
    if vm.jit.options.log_exits && !code.is_invalidated() {
        eprintln!(
            "JIT invalidate: {}, legacy arguments of {} read at pc {}",
            super::describe_executable(&compiled_executable),
            super::describe_executable(&executable),
            frame_state.pc
        );
    }
    code.invalidate();
    if is_installed_code(vm, &compiled_executable, code) {
        compiled_executable.discard_jit_code(vm);
    }
}
