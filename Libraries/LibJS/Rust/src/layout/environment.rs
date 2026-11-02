/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use super::buffer::InterpreterBuffer;
use super::cell::{CellHeader, CellHeaderTail, Gc};
use super::member::NullableMember;
use super::object::Object;
use super::value::Value;
use crate::layout_forward::{
    DeclarativeEnvironmentRareDataStorage, EnvironmentShapeStorage, ObjectEnvironment, PrivateEnvironmentStorage,
};

/// Set in a binding's flags when the binding can be assigned to.
pub const BINDING_FLAG_MUTABLE: u8 = 1 << 1;

#[repr(C)]
pub struct Environment {
    pub outer: NullableMember<Environment>,
    /// The shape of a declarative environment (null for other environments).
    pub shape: NullableMember<EnvironmentShape>,
    pub header: CellHeaderTail,
    pub this_binding_status: Cell<u8>,
    pub permanently_screwed_by_eval: Cell<bool>,
    pub declarative: Cell<bool>,
    /// Whether the binding values of a declarative environment are stored in its cell, after the fields of its class.
    pub binding_values_are_inline: Cell<bool>,
}

const _: () = assert!(size_of::<Environment>() == 16);

#[repr(C)]
pub struct DeclarativeEnvironment {
    pub base: Environment,
    pub binding_values: InterpreterBuffer<Value>,
    pub rare_data: Cell<*mut DeclarativeEnvironmentRareData>,
}

const _: () = assert!(size_of::<DeclarativeEnvironment>() == 40);

/// The declarative environment of a global environment. Caches of global variables hold on to its serial number, which
/// changes when the bindings do.
#[repr(C)]
pub struct GlobalDeclarativeEnvironment {
    pub base: DeclarativeEnvironment,
    pub serial_number: Cell<u64>,
}

#[repr(C)]
pub struct DeclarativeEnvironmentRareData {
    pub binding_flags: InterpreterBuffer<u8>,
    pub storage: DeclarativeEnvironmentRareDataStorage,
}

#[repr(C)]
pub struct GlobalEnvironment {
    pub base: Environment,
    pub object_record: Cell<Option<Gc<ObjectEnvironment>>>,
    pub global_this_value: Cell<Option<Gc<Object>>>,
    pub declarative_record: Cell<Option<Gc<DeclarativeEnvironment>>>,
}

#[repr(C)]
pub struct PrivateEnvironment {
    pub header: CellHeader,
    pub outer: NullableMember<PrivateEnvironment>,
    pub storage: PrivateEnvironmentStorage,
}

#[repr(C)]
pub struct EnvironmentShape {
    pub header: CellHeader,
    pub binding_flags: InterpreterBuffer<u8>,
    /// The first of the names of the bindings (the one-word Utf16FlyStrings of the storage), one per flag, which never
    /// change.
    pub binding_names: *const usize,
    /// Whether no two bindings have the same name, and none has the empty name.
    pub has_unique_binding_names: bool,
    pub storage: EnvironmentShapeStorage,
}
