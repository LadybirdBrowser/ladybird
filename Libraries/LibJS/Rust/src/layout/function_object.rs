/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use super::cell::CellHeader;
use super::environment::{DeclarativeEnvironment, Environment, PrivateEnvironment};
use super::executable::ExecutableHead;
use super::execution_context::ScriptOrModule;
use super::member::{Member, NullableMember};
use super::object::Object;
use super::realm::Realm;
use super::value::Value;
use crate::layout_forward::{
    EcmascriptFunctionObjectStorage, FlyStringSlot, RawNativeFunctionPointer, SharedFunctionInstanceDataStorage,
};

#[repr(C)]
pub struct FunctionObject {
    pub base: Object,
}

const _: () = assert!(size_of::<FunctionObject>() == size_of::<Object>());

/// The property values of a function that are its "length" and "name".
pub const FUNCTION_LENGTH_AND_NAME_SLOTS: usize = 2;

#[repr(C)]
pub struct NativeFunction {
    pub base: FunctionObject,
    /// OPTIMIZATION: The named properties of most native functions are "length" and "name".
    pub inline_named_storage: [Cell<Value>; FUNCTION_LENGTH_AND_NAME_SLOTS],
    /// The name call stacks show, which only some native functions have.
    pub name: FlyStringSlot,
    pub initial_name: FlyStringSlot,
    pub realm: Member<Realm>,
    /// The index of a raw native function's function in the native function table, in what would otherwise be padding
    /// (0 for other native functions).
    pub raw_native_function_index: Cell<u32>,
}

const _: () = assert!(size_of::<NativeFunction>() == 72);
const _: () = assert!(core::mem::offset_of!(NativeFunction, inline_named_storage) == size_of::<Object>());

#[repr(C)]
pub struct RawNativeFunction {
    pub base: NativeFunction,
}

const _: () = assert!(size_of::<RawNativeFunction>() == 72);

/// A raw native getter that reads a wrapped value straight out of a platform object, at offsets the embedder provides:
/// word offsets of pointer-sized fields, and the byte offset of the implementation value, which is a compressed cell
/// pointer if `implementation_value_is_compressed` is 1, and pointer-sized if it is 0.
#[repr(C)]
pub struct DirectGetterFunction {
    pub base: RawNativeFunction,
    pub wrapper_implementation_word_offset: Cell<u32>,
    pub implementation_value_offset: Cell<u32>,
    pub main_world_wrapper_word_offset: Cell<u32>,
    pub weak_impl_value_word_offset: Cell<u32>,
    pub implementation_value_is_compressed: Cell<u32>,
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
    pub shared_data: NullableMember<SharedFunctionInstanceData>,
    pub environment: NullableMember<Environment>,
}

/// An ECMAScript function object that needs more than most: a method or a class constructor, or a function of code
/// with private names (see ecmascript_function_flags::HAS_RARE_DATA).
#[repr(C)]
pub struct EcmascriptFunctionObjectWithRareData {
    pub base: EcmascriptFunctionObject,
    pub private_environment: NullableMember<PrivateEnvironment>,
    pub home_object: NullableMember<Object>,
    pub storage: EcmascriptFunctionObjectStorage,
}

/// The bits of FunctionObject::flags of an ECMAScript function object.
pub mod ecmascript_function_flags {
    /// Set on all ECMAScript function objects, which keeps their flags from reading as a builtin.
    pub const IS_ECMASCRIPT_FUNCTION: u8 = 1 << 7;
    pub const MAY_NEED_LAZY_PROTOTYPE_INSTANTIATION: u8 = 1 << 0;
    pub const IS_METHOD: u8 = 1 << 1;
    /// The function does not have its "length" and "name" properties yet, and acts as if it had them, in front of the
    /// properties it has. Functions with this flag have shapes of their own: descendants of the premade shape of their
    /// kind, whose first property is not "length".
    pub const HAS_LAZY_LENGTH_AND_NAME: u8 = 1 << 2;
    /// The function is an EcmascriptFunctionObjectWithRareData.
    pub const HAS_RARE_DATA: u8 = 1 << 3;
    /// The function was given a name other than the one of its shared data, which is the value of its "name" property.
    pub const HAS_CUSTOM_NAME: u8 = 1 << 4;
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
    pub executable: NullableMember<ExecutableHead>,
    pub asm_call_metadata: Cell<u64>,
    pub formal_parameter_count: Cell<u32>,
    pub strict: Cell<bool>,
    pub function_environment_needed: Cell<bool>,
    pub uses_this: Cell<bool>,
    pub can_inline_call: Cell<bool>,
    /// How JIT code allocates the function environments of calls of the functions, once the runtime made one.
    pub call_environment_template: NullableMember<CallEnvironmentTemplate>,
    /// The [[ScriptOrModule]] of the functions, which is the same for all of them.
    pub script_or_module: Cell<ScriptOrModule>,
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
