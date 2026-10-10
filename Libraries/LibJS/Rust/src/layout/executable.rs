/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use super::buffer::InterpreterBuffer;
use super::cell::CellHeader;
use super::property_lookup_cache::{EnvironmentCoordinate, GlobalVariableCache, PropertyLookupCache};
use super::value::Value;

/// The part of a bytecode executable that the interpreter reads. The rest of the executable follows it.
#[repr(C)]
pub struct ExecutableHead {
    pub header: CellHeader,
    pub registers_and_locals_count: Cell<u32>,
    pub registers_and_locals_and_constants_count: Cell<u32>,
    pub asm_constants_size: Cell<u64>,
    pub asm_constants_data: Cell<*const Value>,
    pub bytecode_data: Cell<*const u8>,
    pub bytecode_size: Cell<usize>,
    /// Which of the VM's dispatch tables (see `VmHead::dispatch_tables`) the interpreter runs this executable's frames
    /// with, which it switches to whenever it enters one of them.
    pub dispatch_table_index: Cell<u8>,
    pub constants: InterpreterBuffer<Value>,
    pub property_lookup_caches: InterpreterBuffer<PropertyLookupCache>,
    pub global_variable_caches: InterpreterBuffer<GlobalVariableCache>,
    pub environment_coordinate_caches: InterpreterBuffer<EnvironmentCoordinate>,
}
