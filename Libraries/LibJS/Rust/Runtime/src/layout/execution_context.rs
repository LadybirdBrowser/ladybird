/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use super::cell::Gc;
use super::environment::{Environment, PrivateEnvironment};
use super::executable::ExecutableHead;
use super::function_object::FunctionObject;
use super::realm::Realm;
use super::value::Value;
use crate::layout_forward::{Module, Script};

pub const SCRIPT_OR_MODULE_TAG_EMPTY: u8 = 0;
pub const SCRIPT_OR_MODULE_TAG_SCRIPT: u8 = 1;
pub const SCRIPT_OR_MODULE_TAG_MODULE: u8 = 2;

/// Mirrors JS::ScriptOrModule. The interpreter clears the field by zeroing it, so all-zero bytes must mean Empty.
#[derive(Clone, Copy)]
#[repr(C, u8)]
pub enum ScriptOrModule {
    Empty = SCRIPT_OR_MODULE_TAG_EMPTY,
    Script(Gc<Script>) = SCRIPT_OR_MODULE_TAG_SCRIPT,
    Module(Gc<Module>) = SCRIPT_OR_MODULE_TAG_MODULE,
}

const _: () = assert!(size_of::<ScriptOrModule>() == 16);

/// Mirrors JS::ExecutionContext field for field: the interpreter builds whole frames inline, so a field it does not
/// know about would be left uninitialized in those frames. The frame's value slots follow it directly.
#[repr(C)]
pub struct ExecutionContext {
    pub function: Cell<Option<Gc<FunctionObject>>>,
    pub realm: Cell<Option<Gc<Realm>>>,
    pub script_or_module: Cell<ScriptOrModule>,
    pub lexical_environment: Cell<Option<Gc<Environment>>>,
    pub variable_environment: Cell<Option<Gc<Environment>>>,
    pub private_environment: Cell<Option<Gc<PrivateEnvironment>>>,
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
}

impl ExecutionContext {
    pub const NO_YIELD_CONTINUATION: u32 = u32::MAX;
}

const _: () = assert!(align_of::<ExecutionContext>() == size_of::<Value>());
