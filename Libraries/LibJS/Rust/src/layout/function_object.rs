/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use super::cell::{CellHeader, Gc};
use super::environment::{DeclarativeEnvironment, Environment, PrivateEnvironment};
use super::executable::ExecutableHead;
use super::execution_context::ScriptOrModule;
use super::object::Object;
use super::primitive_string::PrimitiveString;
use super::realm::Realm;
use crate::layout_forward::{
    EcmascriptFunctionObjectStorage, FlyStringSlot, RawNativeFunctionPointer, SharedFunctionInstanceDataStorage,
};

#[repr(C)]
pub struct FunctionObject {
    pub base: Object,
    /// The interpreter reads the builtin and whether there is one as two separate bytes, like Optional<Builtin>.
    pub builtin: Cell<u8>,
    pub has_builtin: Cell<bool>,
}

#[repr(C)]
pub struct NativeFunction {
    pub base: FunctionObject,
    /// The name call stacks show, which only some native functions have.
    pub name: FlyStringSlot,
    pub initial_name: FlyStringSlot,
    pub realm: Cell<Gc<Realm>>,
}

#[repr(C)]
pub struct RawNativeFunction {
    pub base: NativeFunction,
    pub native_function_index: Cell<u32>,
}

/// A raw native getter that reads a wrapped value straight out of a platform object, at word offsets the embedder
/// provides.
#[repr(C)]
pub struct DirectGetterFunction {
    pub base: RawNativeFunction,
    pub wrapper_implementation_word_offset: Cell<u32>,
    pub implementation_value_word_offset: Cell<u32>,
    pub main_world_wrapper_word_offset: Cell<u32>,
    pub weak_impl_value_word_offset: Cell<u32>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum NativeFunctionType {
    RawNativeFunction,
}

/// The native function table never grows past this power of two, so the interpreter masks an index it reads from a
/// function object into the table instead of trusting it.
pub const NATIVE_FUNCTION_TABLE_CAPACITY: usize = 1 << 16;
pub const NATIVE_FUNCTION_TABLE_INDEX_MASK: u32 = (NATIVE_FUNCTION_TABLE_CAPACITY - 1) as u32;

#[repr(C)]
pub struct NativeFunctionTableEntry {
    pub function: RawNativeFunctionPointer,
    pub function_type: NativeFunctionType,
}

#[repr(C)]
pub struct EcmascriptFunctionObject {
    pub base: FunctionObject,
    pub shared_data: Cell<Gc<SharedFunctionInstanceData>>,
    pub name: FlyStringSlot,
    pub name_string: Cell<Option<Gc<PrimitiveString>>>,
    pub environment: Cell<Option<Gc<Environment>>>,
    pub private_environment: Cell<Option<Gc<PrivateEnvironment>>>,
    pub script_or_module: Cell<ScriptOrModule>,
    pub home_object: Cell<Option<Gc<Object>>>,
    pub storage: EcmascriptFunctionObjectStorage,
}

/// Bits of SharedFunctionInstanceData::asm_call_metadata above the formal parameter count in its low 32 bits.
pub mod asm_call_metadata {
    pub const CAN_INLINE_CALL: u64 = 1 << 32;
    pub const NEEDS_ENVIRONMENT_OR_THIS_VALUE_RESOLUTION: u64 = 1 << 33;
    pub const USES_THIS: u64 = 1 << 34;
    pub const STRICT: u64 = 1 << 35;
}

#[repr(C)]
pub struct SharedFunctionInstanceData {
    pub header: CellHeader,
    pub executable: Cell<Option<Gc<ExecutableHead>>>,
    pub asm_call_metadata: Cell<u64>,
    pub formal_parameter_count: Cell<u32>,
    pub strict: Cell<bool>,
    pub function_environment_needed: Cell<bool>,
    pub uses_this: Cell<bool>,
    pub can_inline_call: Cell<bool>,
    /// How JIT code allocates the function environments of calls of the functions, once the runtime made one.
    pub call_environment_template: Cell<Option<Gc<CallEnvironmentTemplate>>>,
    pub storage: SharedFunctionInstanceDataStorage,
}

/// The number of words of a FunctionEnvironment: a DeclarativeEnvironment and three more fields.
pub const FUNCTION_ENVIRONMENT_WORDS: usize = size_of::<DeclarativeEnvironment>() / 8 + 3;

/// A function environment like the ones the calls of a function get, with its final shape and room for its binding
/// values in its cell, for JIT code to allocate from the local free list of its `size_class` (cells of `cell_size`
/// bytes, see `jit::allocation::FunctionEnvironmentFreeLists`) and fill with `words`. The words have no binding values pointer, outer environment, function object or this
/// value, which differ between calls: the binding values start `binding_values_offset` bytes into the cell (0 if they
/// are not in the cell), and the call binds `this` in the environment if `binds_this` is not 0. Templates are cells, so
/// that JIT code decodes the pointer to one into the GC heap like any other cell pointer.
#[repr(C)]
pub struct CallEnvironmentTemplate {
    pub header: CellHeader,
    pub size_class: u64,
    pub cell_size: u64,
    pub binding_values_offset: u64,
    pub binds_this: u64,
    pub words: [u64; FUNCTION_ENVIRONMENT_WORDS],
}
