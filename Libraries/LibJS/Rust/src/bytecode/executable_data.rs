/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Compiled bytecode together with everything needed to run it, as owned Rust values.

use std::sync::Arc;

use ak::Utf16String;

use super::constant::AbstractOperationKind;
use super::constant::WellKnownSymbolKind;
use super::operand::PropertyKeyTableIndex;
use super::source_map::SourceOffsetEntry;
use crate::ast::FunctionId;
use crate::compile::SharedFunctionDescription;
use crate::compile::utf16_string_memory_size;
use crate::layout::buffer::BlockSlice;
use crate::slice_block::SliceBlock;

/// Exception handler range (with byte offsets, post-linking).
#[derive(Debug, Clone)]
pub struct ExceptionHandler {
    pub start_offset: u32,
    pub end_offset: u32,
    pub handler_offset: u32,
    pub catches_exception: bool,
}

/// The longest string constant that gets interned. Long ones are rarely compared, and not worth hashing.
pub(crate) const MAXIMUM_INTERNED_CONSTANT_LENGTH: usize = 256;

/// A typed constant value stored in the constant pool.
///
/// The actual NaN-boxed encoding happens at the FFI boundary when
/// creating the `Bytecode::Executable`.
#[derive(Debug, Clone)]
pub enum ConstantValue {
    Number(f64),
    Boolean(bool),
    Null,
    Undefined,
    Empty,
    String(Utf16String),
    BigInt(String),
    /// A VM-specific well-known symbol resolved when the Executable is materialized.
    WellKnownSymbol(WellKnownSymbolKind),
    /// A NativeJavaScriptBackedFunction intrinsic resolved when the Executable is materialized.
    AbstractOperation(AbstractOperationKind),
}

pub struct PendingSharedFunctionData {
    /// The function in the tree of the compile job, for as long as that job runs.
    pub function: FunctionId,
    pub description: SharedFunctionDescription,
    pub class_field_initializer_name: Option<(Utf16String, bool)>,
    pub should_eager_compile: bool,
    pub precompiled_function: Option<Arc<PrecompiledFunction>>,
}

/// Metadata computed from scope analysis for a SharedFunctionInstanceData.
#[derive(Clone)]
pub struct FunctionSfdMetadata {
    /// The parameter names if a mapped arguments object goes by them, and empty otherwise.
    pub parameter_names: Box<[ak::Utf16FlyString]>,
    pub uses_this: bool,
    pub this_value_needs_environment_resolution: bool,
    pub function_environment_needed: bool,
    /// Whether the code of the function can change the running lexical environment.
    pub creates_environment: bool,
    pub function_environment_bindings_count: usize,
    pub var_environment_bindings_count: usize,
    pub might_need_arguments: bool,
    pub contains_eval: bool,
}

/// GC-free compiled bytecode for a function that top-level code will immediately invoke.
pub struct PrecompiledFunction {
    pub executable: Arc<ExecutableData>,
    pub metadata: FunctionSfdMetadata,
}

impl PrecompiledFunction {
    /// The bytes of this function's code, without the functions kept for its payloads.
    pub fn memory_size(&self) -> usize {
        size_of::<Self>() + size_of_val(&*self.metadata.parameter_names) + self.executable.memory_size()
    }

    /// This function again, sharing its code.
    pub fn shared_copy(&self) -> Self {
        Self {
            executable: self.executable.clone(),
            metadata: self.metadata.clone(),
        }
    }
}

#[derive(Clone, Copy)]
pub enum PendingLiteralValueKind {
    None,
    Number,
    BooleanTrue,
    BooleanFalse,
    Null,
    String,
}

pub struct PendingClassElement {
    pub kind: u8,
    pub is_static: bool,
    pub is_private: bool,
    pub private_identifier: Option<Utf16String>,
    pub shared_function_data_index: Option<u32>,
    pub has_initializer: bool,
    pub literal_value_kind: PendingLiteralValueKind,
    pub literal_value_number: f64,
    pub literal_value_string: Option<Utf16String>,
}

pub struct PendingClassBlueprint {
    pub name: Option<Utf16String>,
    pub source_text_offset: usize,
    pub source_text_length: usize,
    pub constructor_sfd_index: u32,
    pub has_super_class: bool,
    pub has_name: bool,
    pub elements: Vec<PendingClassElement>,
}

impl PendingClassBlueprint {
    pub fn memory_size(&self) -> usize {
        let string_size = |string: &Option<Utf16String>| string.as_ref().map_or(0, utf16_string_memory_size);
        size_of::<Self>()
            + string_size(&self.name)
            + self.elements.capacity() * size_of::<PendingClassElement>()
            + self
                .elements
                .iter()
                .map(|element| string_size(&element.private_identifier) + string_size(&element.literal_value_string))
                .sum::<usize>()
    }
}

/// How many inline cache and feedback slots of each kind the bytecode indexes into.
#[derive(Clone, Copy)]
pub struct ExecutableCacheCounts {
    pub property_lookup: u32,
    pub global_variable: u32,
    pub environment_coordinate: u32,
    pub template_object: u32,
    pub object_shape: u32,
    pub object_property_iterator: u32,
    pub environment_shape: u32,
    pub arith_feedback: u32,
    pub value_feedback: u32,
    pub call_feedback: u32,
    pub keyed_feedback: u32,
}

/// Executable::LocalVariableMetadata
#[derive(Clone, Copy)]
pub struct LocalVariableMetadata {
    pub is_mutable: bool,
    /// The part of the source code in which the local variable is in scope.
    pub scope_range: Option<crate::ast::SourceRange>,
}

/// One compiled script, module or function body: everything about it that does not depend on where it runs. Nothing
/// here changes once it is compiled, so any number of [`Executable`]s can link it, each with caches, constants and
/// functions of its own, and they all run the same bytecode.
///
/// Nested functions are compiled lazily: unless an entry of
/// `shared_function_data` was precompiled, it describes the function that gets
/// parsed and compiled when it is first called.
///
/// [`Executable`]: super::executable::Executable
pub struct ExecutableData {
    pub bytecode: BlockSlice<u8>,
    /// Sorted by start offset, and not overlapping.
    pub exception_handlers: BlockSlice<ExceptionHandler>,
    /// Sorted by bytecode offset: where in the source code the instructions from each offset on came from.
    pub source_map: BlockSlice<SourceOffsetEntry>,
    pub number_of_registers: u32,
    pub number_of_arguments: u32,
    pub is_strict: bool,
    pub cache_counts: ExecutableCacheCounts,
    pub identifier_table: BlockSlice<ak::Utf16FlyString>,
    pub string_table: BlockSlice<ak::Utf16FlyString>,
    /// The names of the local variables, then the names of the arguments.
    pub variable_names: BlockSlice<ak::Utf16FlyString>,
    /// One for each of the local variables.
    pub local_variable_metadata: BlockSlice<LocalVariableMetadata>,
    pub length_identifier: Option<PropertyKeyTableIndex>,
    /// What linking reads, and nothing after it (see [`ExecutableData::release_link_inputs()`]).
    pub property_key_table: BlockSlice<ak::Utf16FlyString>,
    pub constants: BlockSlice<ConstantValue>,
    pub shared_function_data: BlockSlice<PendingSharedFunctionData>,
    pub class_blueprints: BlockSlice<PendingClassBlueprint>,
    /// What turns the source offsets of a source map that has them into lines and columns.
    pub position_source: Arc<crate::parser::SourcePositions>,
    /// The memory of everything above that running the code reads, which gets dropped before it.
    pub tables: SliceBlock,
    /// The memory of what only linking reads.
    pub link_tables: SliceBlock,
}

fn constant_memory_size(constant: &ConstantValue) -> usize {
    match constant {
        ConstantValue::String(string) => utf16_string_memory_size(string),
        ConstantValue::BigInt(literal) => literal.capacity(),
        _ => 0,
    }
}

// SAFETY: Code compiles away from the heap, so it cannot hold a cell. What it describes turns into cells only when an
//         Executable links it, and those stay with that Executable.
#[cfg(not(test))]
unsafe impl crate::gc::lint_markers::HoldsNoCells for ExecutableData {}

impl ExecutableData {
    /// The entries of the source map, with the lines and columns that they stand for, which get worked out when something
    /// asks.
    pub fn source_positions(&self) -> impl Iterator<Item = crate::bytecode::source_map::SourceMapEntry> + '_ {
        use crate::bytecode::source_map::SourceMapEntry;
        use crate::bytecode::source_map::SourceOffset;
        self.source_map.iter().map(|entry| {
            if entry.source_offset == SourceOffset::NONE {
                return SourceMapEntry {
                    bytecode_offset: entry.bytecode_offset,
                    line: 0,
                    column: 0,
                };
            }
            let position = self.position_source.lines.position(entry.source_offset.0);
            SourceMapEntry {
                bytecode_offset: entry.bytecode_offset,
                line: position.line,
                column: position.column,
            }
        })
    }

    /// The bytes of this code and of what it holds for the functions and classes it declares. A name in one of the tables
    /// counts as its slot in the table, since the text of a name is shared by everything that uses it.
    pub fn memory_size(&self) -> usize {
        size_of::<Self>()
            + size_of_val(&*self.bytecode)
            + size_of_val(&*self.exception_handlers)
            + size_of_val(&*self.source_map)
            + size_of_val(&*self.identifier_table)
            + size_of_val(&*self.string_table)
            + size_of_val(&*self.variable_names)
            + size_of_val(&*self.local_variable_metadata)
            + size_of_val(&*self.property_key_table)
            + size_of_val(&*self.constants)
            + self.constants.iter().map(constant_memory_size).sum::<usize>()
            + self
                .shared_function_data
                .iter()
                .map(PendingSharedFunctionData::memory_size)
                .sum::<usize>()
            + self
                .class_blueprints
                .iter()
                .map(PendingClassBlueprint::memory_size)
                .sum::<usize>()
    }

    /// Frees what only linking reads, for an executable that is the last to hold this code, so that code nothing can
    /// link again costs no more than what running it reads.
    pub fn release_link_inputs(&mut self) {
        self.property_key_table = BlockSlice::empty();
        self.constants = BlockSlice::empty();
        self.shared_function_data = BlockSlice::empty();
        self.class_blueprints = BlockSlice::empty();
        self.link_tables = SliceBlock::empty();
    }
}
