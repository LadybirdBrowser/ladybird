/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use super::buffer::InterpreterBuffer;
use super::cell::{CellHeader, Gc};
use super::object::Object;
use super::value::Value;
use crate::layout_forward::{
    DeclarativeEnvironmentRareDataStorage, EnvironmentShapeStorage, ObjectEnvironment, PrivateEnvironmentStorage,
};

/// Set in a binding's flags when the binding can be assigned to.
pub const BINDING_FLAG_MUTABLE: u8 = 1 << 1;

#[repr(C)]
pub struct Environment {
    pub header: CellHeader,
    pub this_binding_status: Cell<u8>,
    pub permanently_screwed_by_eval: Cell<bool>,
    pub declarative: Cell<bool>,
    pub outer: Cell<Option<Gc<Environment>>>,
}

#[repr(C)]
pub struct DeclarativeEnvironment {
    pub base: Environment,
    pub shape: Cell<Option<Gc<EnvironmentShape>>>,
    pub binding_values: InterpreterBuffer<Value>,
    pub rare_data: Cell<*mut DeclarativeEnvironmentRareData>,
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
    pub outer: Cell<Option<Gc<PrivateEnvironment>>>,
    pub storage: PrivateEnvironmentStorage,
}

#[repr(C)]
pub struct EnvironmentShape {
    pub header: CellHeader,
    pub binding_flags: InterpreterBuffer<u8>,
    pub storage: EnvironmentShapeStorage,
}
