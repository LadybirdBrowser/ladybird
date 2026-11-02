/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ptr::NonNull;

use super::cell::Gc;
use super::environment::{Environment, PrivateEnvironment};
use super::executable::ExecutableHead;
use super::function_object::FunctionObject;
use super::realm::Realm;
use super::value::Value;
use crate::layout_forward::{Module, Script};

/// [[ScriptOrModule]]: null, a Script Record or a Module Record.
#[derive(Clone, Copy)]
pub enum ScriptOrModule {
    Empty,
    Script(Gc<Script>),
    Module(Gc<Module>),
}

/// A ScriptOrModule in one word, which is what frames and shared function data keep: zero for Empty, otherwise the
/// pointer to the cell with its lowest bit set for a module. The interpreter copies it around and clears it by zeroing.
#[repr(transparent)]
pub struct ScriptOrModuleSlot(Cell<u64>);

pub const SCRIPT_OR_MODULE_SLOT_MODULE_BIT: u64 = 1;

impl ScriptOrModuleSlot {
    pub fn new(value: ScriptOrModule) -> Self {
        Self(Cell::new(Self::encode(value)))
    }

    fn encode(value: ScriptOrModule) -> u64 {
        match value {
            ScriptOrModule::Empty => 0,
            ScriptOrModule::Script(script) => script.as_ptr() as u64,
            ScriptOrModule::Module(module) => module.as_ptr() as u64 | SCRIPT_OR_MODULE_SLOT_MODULE_BIT,
        }
    }

    pub fn get(&self) -> ScriptOrModule {
        let word = self.0.get();
        let Some(pointer) = NonNull::new((word & !SCRIPT_OR_MODULE_SLOT_MODULE_BIT) as *mut u8) else {
            return ScriptOrModule::Empty;
        };
        // SAFETY: A slot holds what encode() made of a live script or module, which whoever holds the slot traces.
        unsafe {
            if word & SCRIPT_OR_MODULE_SLOT_MODULE_BIT != 0 {
                ScriptOrModule::Module(Gc::from_non_null(pointer.cast()))
            } else {
                ScriptOrModule::Script(Gc::from_non_null(pointer.cast()))
            }
        }
    }

    pub fn set(&self, value: ScriptOrModule) {
        self.0.set(Self::encode(value));
    }

    pub fn word(&self) -> u64 {
        self.0.get()
    }

    pub fn from_word(word: u64) -> Self {
        Self(Cell::new(word))
    }
}

/// Mirrors JS::ExecutionContext field for field: the interpreter builds whole frames inline, so a field it does not
/// know about would be left uninitialized in those frames. The frame's value slots follow it directly.
#[repr(C)]
pub struct ExecutionContext {
    pub function: Cell<Option<Gc<FunctionObject>>>,
    pub realm: Cell<Option<Gc<Realm>>>,
    pub script_or_module: ScriptOrModuleSlot,
    pub lexical_environment: Cell<Option<Gc<Environment>>>,
    pub variable_environment: Cell<Option<Gc<Environment>>>,
    pub private_environment: Cell<Option<Gc<PrivateEnvironment>>>,
    /// Tells frames apart for the debugger. Frames that JIT code creates have none (0) until the debugger asks (see
    /// Vm::frame_id_of()).
    pub frame_id: Cell<u64>,
    pub program_counter: Cell<u32>,
    pub skip_when_determining_incumbent_counter: Cell<u32>,
    pub yield_continuation: Cell<u32>,
    pub yield_is_await: Cell<bool>,
    pub yield_value_is_iterator_result: Cell<bool>,
    pub caller_is_construct: Cell<bool>,
    pub frame_initialized: Cell<bool>,
    /// The empty value means the frame has no this value.
    pub this_value: Cell<Value>,
    pub executable: Cell<Option<Gc<ExecutableHead>>>,
    pub caller_frame: Cell<*mut ExecutionContext>,
    pub passed_argument_count: Cell<u32>,
    pub caller_return_pc: Cell<u32>,
    pub caller_dst_raw: Cell<u32>,
    pub registers_and_constants_and_locals_and_arguments_count: Cell<u32>,
    pub argument_count: Cell<u32>,
    /// Non-standard: Set on an inline frame whose caller is waiting for it in native code (a call made by JIT code)
    /// rather than in the interpreter. Returning from such a frame, or unwinding it for an exception, leaves the
    /// interpreter.
    pub returns_to_native_caller: Cell<bool>,
    /// Non-standard: Set while JIT code runs the frame. An executable keeps its discarded JIT code until no live
    /// frame of it is run by JIT code (see jit::code::detach_discarded_jit_code_if_unused()).
    pub runs_jit_code: Cell<bool>,
}

impl ExecutionContext {
    pub const NO_YIELD_CONTINUATION: u32 = u32::MAX;
}

const _: () = assert!(align_of::<ExecutionContext>() == size_of::<Value>());
