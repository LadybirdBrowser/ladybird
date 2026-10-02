/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Compiled bytecode together with everything needed to run it, as owned Rust values.

use std::ffi::c_void;

use super::basic_block::SourceMapEntry;
use super::generator::AssembledBytecode;
use super::generator::ConstantValue;
use super::generator::ExceptionHandler;
use super::generator::Generator;
use super::generator::LocalVariable;
use super::generator::PendingClassBlueprint;
use super::generator::PendingSharedFunctionData;
use super::operand::PropertyKeyTableIndex;

/// How many inline cache slots of each kind the bytecode indexes into.
#[derive(Clone, Copy)]
pub struct ExecutableCacheCounts {
    pub property_lookup: u32,
    pub global_variable: u32,
    pub environment_coordinate: u32,
    pub template_object: u32,
    pub object_shape: u32,
    pub object_property_iterator: u32,
    pub environment_shape: u32,
}

/// One compiled script, module or function body.
///
/// Nested functions are compiled lazily: unless an entry of
/// `shared_function_data` was precompiled, it keeps the AST payload that its
/// bytecode is generated from when it is first called.
pub struct ExecutableData {
    pub bytecode: Vec<u8>,
    pub exception_handlers: Vec<ExceptionHandler>,
    pub source_map: Vec<SourceMapEntry>,
    pub basic_block_start_offsets: Vec<usize>,
    pub number_of_registers: u32,
    pub number_of_arguments: u32,
    pub is_strict: bool,
    pub this_value_needs_environment_resolution: bool,
    pub cache_counts: ExecutableCacheCounts,
    pub identifier_table: Vec<ak::Utf16FlyString>,
    pub property_key_table: Vec<ak::Utf16FlyString>,
    pub string_table: Vec<ak::Utf16FlyString>,
    pub constants: Vec<ConstantValue>,
    pub local_variables: Vec<LocalVariable>,
    pub argument_variable_names: Vec<ak::Utf16FlyString>,
    pub length_identifier: Option<PropertyKeyTableIndex>,
    pub shared_function_data: Vec<PendingSharedFunctionData>,
    pub class_blueprints: Vec<PendingClassBlueprint>,
    /// Handles from `host::compile_regex()`, owned by this executable until the
    /// runtime adopts them.
    pub compiled_regexes: Vec<*mut c_void>,
}

impl ExecutableData {
    pub fn new(generator: Generator, assembled: AssembledBytecode) -> Self {
        let arena = generator.arena;
        let shared_function_data = generator
            .shared_function_data
            .into_iter()
            .map(|mut shared_function_data| {
                shared_function_data.arena.get_or_insert_with(|| arena.clone());
                shared_function_data
            })
            .collect();
        Self {
            bytecode: assembled.bytecode,
            exception_handlers: assembled.exception_handlers,
            source_map: assembled.source_map,
            basic_block_start_offsets: assembled.basic_block_start_offsets,
            number_of_registers: assembled.number_of_registers,
            number_of_arguments: assembled.number_of_arguments,
            is_strict: generator.strict,
            this_value_needs_environment_resolution: generator.this_value_needs_environment_resolution,
            cache_counts: ExecutableCacheCounts {
                property_lookup: generator.next_property_lookup_cache,
                global_variable: generator.next_global_variable_cache,
                environment_coordinate: generator.next_environment_coordinate_cache,
                template_object: generator.next_template_object_cache,
                object_shape: generator.next_object_shape_cache,
                object_property_iterator: generator.next_object_property_iterator_cache,
                environment_shape: generator.next_environment_shape_cache,
            },
            identifier_table: generator.identifier_table,
            property_key_table: generator.property_key_table,
            string_table: generator.string_table,
            constants: generator.constants,
            local_variables: generator.local_variables,
            argument_variable_names: generator.argument_variable_names,
            length_identifier: generator.length_identifier,
            shared_function_data,
            class_blueprints: generator.class_blueprints,
            compiled_regexes: generator.compiled_regexes,
        }
    }
}
