/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use crate::frontend_host::rust_free_compiled_regex;
use crate::gc::class::{GcCell, define_cell};
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::runtime_functions::unimplemented_runtime_function;
use crate::layout::buffer::InterpreterBuffer;
use crate::layout::cell::CellHeader;
use crate::layout::executable::ExecutableHead;
use crate::layout::property_lookup_cache::{
    EnvironmentCoordinate, GlobalVariableCache, PropertyLookupCache, PropertyLookupCacheEntry,
    PropertyLookupCacheEntryType,
};
use crate::layout::value::Value;
use libjs_rust::bytecode::executable::ExecutableData;
use libjs_rust::bytecode::generator::ConstantValue;

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
    pub identifier_table: Vec<ak::Utf16FlyString>,
    pub property_key_table: Vec<ak::Utf16FlyString>,
    pub string_table: Vec<ak::Utf16FlyString>,
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
            identifier_table: Vec::new(),
            property_key_table: Vec::new(),
            string_table: Vec::new(),
        }
    }

    /// Builds the executable for what the frontend compiled.
    pub fn from_executable_data(data: ExecutableData) -> Self {
        if !data.shared_function_data.is_empty() {
            unimplemented_runtime_function("creating the functions an executable declares", 0);
        }
        if !data.class_blueprints.is_empty() {
            unimplemented_runtime_function("creating the classes an executable declares", 0);
        }
        // The regexes were only compiled to report early errors; the runtime compiles them again when it runs.
        for regex in data.compiled_regexes {
            // SAFETY: Each handle came from rust_compile_regex and is freed once.
            unsafe { rust_free_compiled_regex(regex) };
        }
        let constants: Box<[Value]> = data.constants.iter().map(constant_value).collect();
        let counts = ExecutableCacheCounts {
            property_lookup_caches: data.cache_counts.property_lookup,
            global_variable_caches: data.cache_counts.global_variable,
            environment_coordinate_caches: data.cache_counts.environment_coordinate,
        };
        let number_of_locals = u32::try_from(data.local_variables.len()).expect("local count fits in u32");
        let mut executable = Self::new(
            data.bytecode.into_boxed_slice(),
            data.number_of_registers,
            number_of_locals,
            data.number_of_arguments,
            constants,
            &counts,
            data.is_strict,
        );
        executable.identifier_table = data.identifier_table;
        executable.property_key_table = data.property_key_table;
        executable.string_table = data.string_table;
        executable
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

fn constant_value(constant: &ConstantValue) -> Value {
    match constant {
        ConstantValue::Number(number) => Value::from_f64(*number),
        ConstantValue::Boolean(boolean) => Value::from_bool(*boolean),
        ConstantValue::Null => Value::NULL,
        ConstantValue::Undefined => Value::UNDEFINED,
        ConstantValue::Empty => Value::EMPTY,
        ConstantValue::String(_) => unimplemented_runtime_function("string constants", 0),
        ConstantValue::BigInt(_) => unimplemented_runtime_function("BigInt constants", 0),
        ConstantValue::WellKnownSymbol(_) => unimplemented_runtime_function("well-known symbol constants", 0),
        ConstantValue::AbstractOperation(_) => unimplemented_runtime_function("abstract operation constants", 0),
    }
}

// SAFETY: The constants are the only cells an executable holds so far. The inline caches do not keep the shapes
// and objects they remember alive.
unsafe impl Trace for Executable {
    fn trace(&self, visitor: &mut Visitor) {
        visitor.visit_values(&self.constants);
    }
}
