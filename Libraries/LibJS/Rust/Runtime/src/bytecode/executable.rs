/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use crate::gc::class::{GcCell, define_cell};
use crate::gc::visitor::{Trace, Visitor};
use crate::layout::buffer::InterpreterBuffer;
use crate::layout::cell::CellHeader;
use crate::layout::executable::ExecutableHead;
use crate::layout::property_lookup_cache::{
    EnvironmentCoordinate, GlobalVariableCache, PropertyLookupCache, PropertyLookupCacheEntry,
    PropertyLookupCacheEntryType,
};
use crate::layout::value::Value;

/// A unit of bytecode: a script, a module, a function body or an eval, with what the interpreter needs to run it.
#[repr(C)]
pub struct Executable {
    pub head: ExecutableHead,
    bytecode: Box<[u8]>,
    constants: Box<[Value]>,
    property_lookup_caches: Box<[PropertyLookupCache]>,
    global_variable_caches: Box<[GlobalVariableCache]>,
    environment_coordinate_caches: Box<[EnvironmentCoordinate]>,
    pub number_of_registers: u32,
    pub number_of_arguments: u32,
    pub is_strict_mode: bool,
}

define_cell!(Executable, Other);

const _: () = assert!(core::mem::offset_of!(Executable, head) == 0);

/// The cache counts an executable's bytecode refers to.
pub struct ExecutableCacheCounts {
    pub property_lookup_caches: u32,
    pub global_variable_caches: u32,
    pub environment_coordinate_caches: u32,
}

fn interpreter_buffer<T>(elements: &[T]) -> InterpreterBuffer<T> {
    InterpreterBuffer {
        data: Cell::new(elements.as_ptr().cast_mut()),
        size: Cell::new(elements.len()),
        capacity: Cell::new(elements.len()),
    }
}

fn empty_cache_entry() -> PropertyLookupCacheEntry {
    PropertyLookupCacheEntry {
        entry_type: Cell::new(PropertyLookupCacheEntryType::Empty),
        property_offset: Cell::new(0),
        shape_dictionary_generation: Cell::new(0),
        direct_getter_validated: Cell::new(false),
        writes_data_property: Cell::new(false),
        from_shape: Cell::new(None),
        shape: Cell::new(None),
        prototype: Cell::new(None),
        prototype_chain_validity: Cell::new(None),
    }
}

impl Executable {
    pub fn new(
        bytecode: Box<[u8]>,
        number_of_registers: u32,
        number_of_locals: u32,
        number_of_arguments: u32,
        constants: Box<[Value]>,
        cache_counts: &ExecutableCacheCounts,
        is_strict_mode: bool,
    ) -> Self {
        let property_lookup_caches: Box<[PropertyLookupCache]> = (0..cache_counts.property_lookup_caches)
            .map(|_| PropertyLookupCache { data: Cell::new(0) })
            .collect();
        let global_variable_caches: Box<[GlobalVariableCache]> = (0..cache_counts.global_variable_caches)
            .map(|_| GlobalVariableCache {
                entry: empty_cache_entry(),
                environment_serial_number: Cell::new(0),
                environment_binding_index: Cell::new(0),
                has_environment_binding_index: Cell::new(false),
            })
            .collect();
        let environment_coordinate_caches: Box<[EnvironmentCoordinate]> = (0..cache_counts
            .environment_coordinate_caches)
            .map(|_| EnvironmentCoordinate {
                hops: EnvironmentCoordinate::INVALID_MARKER,
                index: EnvironmentCoordinate::INVALID_MARKER,
            })
            .collect();
        let registers_and_locals_count = number_of_registers + number_of_locals;
        let constant_count = u32::try_from(constants.len()).expect("constant count fits in u32");
        let head = ExecutableHead {
            header: CellHeader::for_class(Self::CLASS),
            registers_and_locals_count: Cell::new(registers_and_locals_count),
            registers_and_locals_and_constants_count: Cell::new(registers_and_locals_count + constant_count),
            asm_constants_size: Cell::new(constants.len() as u64),
            asm_constants_data: Cell::new(constants.as_ptr()),
            bytecode_data: Cell::new(bytecode.as_ptr()),
            bytecode_size: Cell::new(bytecode.len()),
            constants: interpreter_buffer(&constants),
            property_lookup_caches: interpreter_buffer(&property_lookup_caches),
            global_variable_caches: interpreter_buffer(&global_variable_caches),
            environment_coordinate_caches: interpreter_buffer(&environment_coordinate_caches),
        };
        Self {
            head,
            bytecode,
            constants,
            property_lookup_caches,
            global_variable_caches,
            environment_coordinate_caches,
            number_of_registers,
            number_of_arguments,
            is_strict_mode,
        }
    }

    pub fn bytecode(&self) -> &[u8] {
        &self.bytecode
    }

    pub fn constants(&self) -> &[Value] {
        &self.constants
    }

    pub fn registers_and_locals_count(&self) -> u32 {
        self.head.registers_and_locals_count.get()
    }
}

// SAFETY: The constants are the only cells an executable holds so far. The inline caches do not keep the shapes
// and objects they remember alive.
unsafe impl Trace for Executable {
    fn trace(&self, visitor: &mut Visitor) {
        visitor.visit_values(&self.constants);
    }
}
