/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The translator from the state of compiled code at one of its sites to the interpreter's state.
//!
//! A site is a point of compiled code with a frame state (see `libjs_jit::code::Site`): an exit, or a leave (a slow
//! path that did not continue in compiled code). The frame state lists the values of the compiled function's frame,
//! which exist only as values of compiled code until the translator writes them into the interpreter's frame.

use core::ptr::NonNull;
use std::collections::HashMap;

use libjs_abi::register;
use libjs_jit::code::{FrameState, Repr, ResumeMode, Site, SiteKind, ValueLocation};

use super::code::{CompileState, EXIT_COUNT_BEFORE_DISCARD, JitCode};
use super::{DeferGc, executable_of, frame_of, frames_above};
use crate::bytecode::instruction::instruction_length_from_bytes;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::execution_context::ExecutionContext;
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
}

/// Reads a value that is not an object the code never created.
fn read_location(location: ValueLocation, dump: &RegisterDump) -> Value {
    let (bits, repr) = match location {
        ValueLocation::Constant(bits) => return Value(bits),
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
        ValueLocation::ArgumentsObject { .. } | ValueLocation::VirtualObject(_) => {
            unreachable!("objects are created by SiteValues")
        }
    };
    match repr {
        Repr::Tagged => Value(bits),
        Repr::Int32 => Value::from_i32(bits as i32),
        Repr::Float64 => Value::from_f64(f64::from_bits(bits)),
        Repr::Bool => Value::from_bool(bits as u32 != 0),
        Repr::Pointer => unreachable!("frame states hold no pointers"),
    }
}

/// The values of a site, creating the objects the code never allocated once each, so that every slot referring to one
/// holds the same object. Its users defer collections until the frames hold the values.
struct SiteValues<'a> {
    vm: &'a Vm,
    site: &'a Site,
    dump: &'a RegisterDump,
    created: HashMap<u32, Gc<Object>>,
}

impl<'a> SiteValues<'a> {
    fn new(vm: &'a Vm, site: &'a Site, dump: &'a RegisterDump) -> Self {
        Self {
            vm,
            site,
            dump,
            created: HashMap::new(),
        }
    }

    fn value(&mut self, location: ValueLocation) -> Value {
        let ValueLocation::VirtualObject(index) = location else {
            return read_location(location, self.dump);
        };
        if let Some(object) = self.created.get(&index) {
            return Value::from_object(*object);
        }
        let description = &self.site.objects[index as usize];
        // SAFETY: The code embeds the shape, which keeps it alive.
        let shape = unsafe { Gc::from_non_null(NonNull::new(description.shape.0 as *mut _).expect("a shape")) };
        let object = Object::create_with_premade_shape(self.vm, shape);
        // NB: The object exists before its properties, which may refer to it.
        self.created.insert(index, object);
        let property_count = shape.property_count();
        for index in 0..property_count {
            let value = match description.properties.get(index as usize) {
                Some(location) => self.value(*location),
                None => Value::UNDEFINED,
            };
            object.put_direct(index, value);
        }
        Value::from_object(object)
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
    let mut held = frame_state.in_frame.clone();
    let mut kept = None;
    if let ResumeMode::ResumeAfter { dst } = frame_state.mode {
        held.push(dst);
        if destination == Destination::Keep {
            kept = Some(dst);
        }
    }
    // NB: Compiled code initializes its frame lazily, before anything observes the frame (see
    //     passes/frame_initialization.rs), and pushes the frames of inlined calls for slow paths uninitialized. Until
    //     then nothing wrote the frame's registers and locals, so the ones the frame state lists as held in the frame
    //     hold what the interpreter's Enter put there, not what the frame's memory had before.
    if !frame.frame_initialized.get() {
        initialize_frame_preserving(frame, kept.as_slice());
    }
    let slots = frame.slots();
    // NB: Frame states list every live slot, so the others are dead. Clearing them keeps the interpreter from seeing
    //     values the compiled code no longer maintains, and drops references to them.
    let registers_and_locals_count = executable_of(frame).registers_and_locals_count() as usize;
    let mut listed = vec![false; registers_and_locals_count];
    for slot in held.iter().chain(frame_state.values.iter().map(|(slot, _)| slot)) {
        if let Some(listed) = listed.get_mut(*slot as usize) {
            *listed = true;
        }
    }
    for (index, listed) in listed
        .iter()
        .enumerate()
        .skip(register::RESERVED_REGISTER_COUNT as usize)
    {
        if !listed {
            slots[index].set(Value::EMPTY);
        }
    }
    // NB: Every slot holding the arguments object the code never created holds the same object.
    let mut arguments_object = None;
    for (slot, location) in &frame_state.values {
        if Some(*slot) == kept {
            continue;
        }
        let value = if let ValueLocation::ArgumentsObject { mapped } = location {
            *arguments_object.get_or_insert_with(|| create_arguments_object(vm, frame, *mapped))
        } else {
            values.value(*location)
        };
        slots[*slot as usize].set(value);
    }
}

/// Full translation of an exit: writes the frame state of the exit into the compiled function's frame `root` (boxing
/// unboxed values), sets the program counter and records the exit. The interpreter then continues in the frame.
pub(super) fn exit(vm: &Vm, root: NonNull<ExecutionContext>, code: &JitCode, index: u32, dump: &RegisterDump) {
    let site = code.site(index);
    let SiteKind::Exit(kind) = site.kind else {
        unreachable!("exits are sites of exits");
    };
    // NB: Exits the "stress-exits" option causes teach the executable nothing.
    let is_stress_exit = vm.jit.take_stress_exit();
    assert_eq!(vm.running_execution_context(), Some(root));
    let compiled_executable = executable_of(frame_of(root));
    assert!(core::ptr::eq(
        compiled_executable.jit_code().expect("the code is attached"),
        code
    ));

    // NB: Nothing may collect the objects created here before the frame holds them.
    let _defer_gc = DeferGc::new(vm);
    let mut values = SiteValues::new(vm, site, dump);
    let frame_state = &site.frames[0];
    let frame_ref = frame_of(root);
    write_frame_state(vm, frame_ref, frame_state, &mut values, Destination::Keep);
    frame_ref.program_counter.set(frame_state.pc);
    if let ResumeMode::ResumeAfter { .. } = frame_state.mode {
        // NB: The call already stored its result in its destination slot.
        let bytecode = compiled_executable.bytecode();
        let pc = frame_state.pc;
        let length = instruction_length_from_bytes(bytecode[pc as usize], bytecode, pc as usize)
            .expect("the bytecode is valid") as u32;
        frame_ref.program_counter.set(pc + length);
    }

    if vm.jit.options.log_exits {
        // NB: The code is identified by its executable and how often that executable's code was discarded before.
        let stress = if is_stress_exit { " (stress)" } else { "" };
        eprintln!(
            "JIT exit: {} pc {} {:?}{stress}, code {:p}/{}",
            super::describe_executable(&compiled_executable),
            frame_state.pc,
            kind,
            compiled_executable.as_ptr(),
            compiled_executable.jit_discard_count()
        );
    }
    if is_stress_exit {
        vm.jit.count_coverage(&format!("stress-exit.{kind:?}"));
        return;
    }
    vm.jit.count_coverage(&format!("exit.{kind:?}"));
    // NB: Invalidated code was discarded, and its recompile sees that what it depended on no longer holds.
    if kind != libjs_jit::code::ExitKind::Invalidated {
        compiled_executable.add_jit_exit_site((frame_state.pc, kind));
    }

    if code.count_exit() >= EXIT_COUNT_BEFORE_DISCARD
        && compiled_executable.jit_compile_state() == CompileState::Installed
    {
        compiled_executable.discard_jit_code(vm);
    }
}

/// Full translation of a leave, which is no exit: compiled code wrote only the slots its slow path reads before calling
/// it, and the slow path did not continue in compiled code. Writes the frame state of the leave into the compiled
/// function's frame `root`, if it is still on the stack: an exception may have unwound it, which then needs nothing.
pub(super) fn leave(vm: &Vm, root: NonNull<ExecutionContext>, code: &JitCode, index: u32, dump: &RegisterDump) {
    let site = code.site(index);
    assert_eq!(site.kind, SiteKind::Leave);
    if frames_above(vm, root).is_none() {
        return;
    }

    // NB: Nothing may collect the objects created here before the frame holds them.
    let _defer_gc = DeferGc::new(vm);
    let mut values = SiteValues::new(vm, site, dump);
    write_frame_state(
        vm,
        frame_of(root),
        &site.frames[0],
        &mut values,
        Destination::WriteOldValue,
    );
}
