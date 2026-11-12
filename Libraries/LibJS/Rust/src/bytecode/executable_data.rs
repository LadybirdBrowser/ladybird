/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Compiled bytecode together with everything needed to run it, as owned Rust values.

use core::num::NonZeroU32;
use std::sync::Arc;

use ak::Utf16String;

use super::constant::AbstractOperationKind;
use super::constant::WellKnownSymbolKind;
use super::operand::PropertyKeyTableIndex;
use super::source_map::{SourceMapEntries, SourceOffset};
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
    /// The text of the literal, boxed once more so that a constant takes 16 bytes: a BigInt literal is rare.
    BigInt(Box<Box<str>>),
    /// A VM-specific well-known symbol resolved when the Executable is materialized.
    WellKnownSymbol(WellKnownSymbolKind),
    /// A NativeJavaScriptBackedFunction intrinsic resolved when the Executable is materialized.
    AbstractOperation(AbstractOperationKind),
}

// NB: Every constant of code has these bytes, until the code is linked for the last time.
const _: () = assert!(size_of::<ConstantValue>() == 16);

pub struct PendingSharedFunctionData {
    pub description: SharedFunctionDescription,
    /// The name of the class field whose initializer the function is, and whether the name is private.
    pub class_field_initializer_name: Option<Box<(Utf16String, bool)>>,
    pub should_eager_compile: bool,
    pub precompiled_function: Option<Arc<PrecompiledFunction>>,
}

// NB: Code has these bytes for every function it creates, until the code is linked for the last time.
const _: () = assert!(size_of::<PendingSharedFunctionData>() == 64);

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
    pub scope_start: SourceOffset,
    pub scope_end: SourceOffset,
}

/// A range of bytecode offsets, from `start_offset` up to but not including `end_offset`.
#[derive(Clone, Copy)]
pub struct StrictRange {
    pub start_offset: u32,
    pub end_offset: u32,
}

impl StrictRange {
    /// The same range, `distance` bytes later.
    pub fn shifted(self, distance: u32) -> Self {
        Self {
            start_offset: self.start_offset + distance,
            end_offset: self.end_offset + distance,
        }
    }
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
    /// Sorted by bytecode offset: where in the source code the instructions from each offset on came from.
    pub source_map: BlockSlice<u8>,
    pub number_of_registers: u32,
    pub number_of_arguments: u32,
    pub is_strict: bool,
    cache_slot_counts: CacheSlotCounts,
    /// The index of the length in the property key table, plus one.
    length_identifier: Option<NonZeroU32>,
    pub identifier_table: BlockSlice<ak::Utf16FlyString>,
    /// The names of the local variables, then the names of the arguments.
    pub variable_names: BlockSlice<ak::Utf16FlyString>,
    /// One for each of the local variables.
    pub local_variable_metadata: BlockSlice<LocalVariableMetadata>,
    rare_data: Option<Box<RareExecutableData>>,
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

// NB: Every compiled function has these bytes.
const _: () = assert!(size_of::<ExecutableData>() == 136);

/// The parts of code that few codes have.
#[derive(Default)]
struct RareExecutableData {
    /// Sorted by start offset, and not overlapping.
    exception_handlers: Box<[ExceptionHandler]>,
    /// Sorted by start offset. Where a class body makes the code strict, if the function is not.
    strict_ranges: Box<[StrictRange]>,
    string_table: Box<[ak::Utf16FlyString]>,
    template_object_caches: u32,
    object_property_iterator_caches: u32,
    environment_shape_caches: u32,
}

/// The counts of the kinds of cache and feedback slots whose sites past the last slot index share it, so that each is
/// at most 65536, kept in two bytes as `u16::MAX`. Code with exactly 65535 slots of a kind gets one more, which no
/// site uses.
#[derive(Clone, Copy)]
struct CacheSlotCounts {
    property_lookup: u16,
    global_variable: u16,
    environment_coordinate: u16,
    object_shape: u16,
    arith_feedback: u16,
    value_feedback: u16,
    call_feedback: u16,
    keyed_feedback: u16,
}

impl CacheSlotCounts {
    fn compact(count: u32) -> u16 {
        assert!(
            count <= u32::from(u16::MAX) + 1,
            "sites past the last slot index share it"
        );
        u16::try_from(count).unwrap_or(u16::MAX)
    }

    fn expand(count: u16) -> u32 {
        if count == u16::MAX {
            u32::from(u16::MAX) + 1
        } else {
            u32::from(count)
        }
    }
}

fn constant_memory_size(constant: &ConstantValue) -> usize {
    match constant {
        ConstantValue::String(string) => utf16_string_memory_size(string),
        ConstantValue::BigInt(literal) => size_of::<Box<str>>() + literal.len(),
        _ => 0,
    }
}

// SAFETY: Code compiles away from the heap, so it cannot hold a cell. What it describes turns into cells only when an
//         Executable links it, and those stay with that Executable.
#[cfg(not(test))]
unsafe impl crate::gc::lint_markers::HoldsNoCells for ExecutableData {}

impl ExecutableData {
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        bytecode: BlockSlice<u8>,
        source_map: BlockSlice<u8>,
        exception_handlers: Box<[ExceptionHandler]>,
        strict_ranges: Box<[StrictRange]>,
        number_of_registers: u32,
        number_of_arguments: u32,
        is_strict: bool,
        cache_counts: ExecutableCacheCounts,
        identifier_table: BlockSlice<ak::Utf16FlyString>,
        string_table: Box<[ak::Utf16FlyString]>,
        variable_names: BlockSlice<ak::Utf16FlyString>,
        local_variable_metadata: BlockSlice<LocalVariableMetadata>,
        length_identifier: Option<PropertyKeyTableIndex>,
        property_key_table: BlockSlice<ak::Utf16FlyString>,
        constants: BlockSlice<ConstantValue>,
        shared_function_data: BlockSlice<PendingSharedFunctionData>,
        class_blueprints: BlockSlice<PendingClassBlueprint>,
        position_source: Arc<crate::parser::SourcePositions>,
        tables: SliceBlock,
        link_tables: SliceBlock,
    ) -> Self {
        let rare_data = RareExecutableData {
            exception_handlers,
            strict_ranges,
            string_table,
            template_object_caches: cache_counts.template_object,
            object_property_iterator_caches: cache_counts.object_property_iterator,
            environment_shape_caches: cache_counts.environment_shape,
        };
        let has_rare_data = !rare_data.exception_handlers.is_empty()
            || !rare_data.strict_ranges.is_empty()
            || !rare_data.string_table.is_empty()
            || rare_data.template_object_caches != 0
            || rare_data.object_property_iterator_caches != 0
            || rare_data.environment_shape_caches != 0;
        Self {
            bytecode,
            source_map,
            number_of_registers,
            number_of_arguments,
            is_strict,
            cache_slot_counts: CacheSlotCounts {
                property_lookup: CacheSlotCounts::compact(cache_counts.property_lookup),
                global_variable: CacheSlotCounts::compact(cache_counts.global_variable),
                environment_coordinate: CacheSlotCounts::compact(cache_counts.environment_coordinate),
                object_shape: CacheSlotCounts::compact(cache_counts.object_shape),
                arith_feedback: CacheSlotCounts::compact(cache_counts.arith_feedback),
                value_feedback: CacheSlotCounts::compact(cache_counts.value_feedback),
                call_feedback: CacheSlotCounts::compact(cache_counts.call_feedback),
                keyed_feedback: CacheSlotCounts::compact(cache_counts.keyed_feedback),
            },
            length_identifier: length_identifier
                .map(|index| NonZeroU32::new(index.0 + 1).expect("the index plus one is not zero")),
            identifier_table,
            variable_names,
            local_variable_metadata,
            rare_data: has_rare_data.then(|| Box::new(rare_data)),
            property_key_table,
            constants,
            shared_function_data,
            class_blueprints,
            position_source,
            tables,
            link_tables,
        }
    }

    /// How many inline cache and feedback slots of each kind the bytecode indexes into.
    pub fn cache_counts(&self) -> ExecutableCacheCounts {
        let counts = &self.cache_slot_counts;
        let rare_data = self.rare_data.as_deref();
        ExecutableCacheCounts {
            property_lookup: CacheSlotCounts::expand(counts.property_lookup),
            global_variable: CacheSlotCounts::expand(counts.global_variable),
            environment_coordinate: CacheSlotCounts::expand(counts.environment_coordinate),
            template_object: rare_data.map_or(0, |rare_data| rare_data.template_object_caches),
            object_shape: CacheSlotCounts::expand(counts.object_shape),
            object_property_iterator: rare_data.map_or(0, |rare_data| rare_data.object_property_iterator_caches),
            environment_shape: rare_data.map_or(0, |rare_data| rare_data.environment_shape_caches),
            arith_feedback: CacheSlotCounts::expand(counts.arith_feedback),
            value_feedback: CacheSlotCounts::expand(counts.value_feedback),
            call_feedback: CacheSlotCounts::expand(counts.call_feedback),
            keyed_feedback: CacheSlotCounts::expand(counts.keyed_feedback),
        }
    }

    pub fn length_identifier(&self) -> Option<PropertyKeyTableIndex> {
        self.length_identifier
            .map(|index| PropertyKeyTableIndex(index.get() - 1))
    }

    /// Sorted by start offset, and not overlapping.
    pub fn exception_handlers(&self) -> &[ExceptionHandler] {
        self.rare_data
            .as_deref()
            .map_or(&[], |rare_data| &rare_data.exception_handlers)
    }

    /// Sorted by start offset. Where a class body makes the code strict, if the function is not.
    pub fn strict_ranges(&self) -> &[StrictRange] {
        self.rare_data
            .as_deref()
            .map_or(&[], |rare_data| &rare_data.strict_ranges)
    }

    pub fn string_table(&self) -> &[ak::Utf16FlyString] {
        self.rare_data
            .as_deref()
            .map_or(&[], |rare_data| &rare_data.string_table)
    }

    /// The entries of the source map, with the lines and columns that they stand for, which get worked out when something
    /// asks.
    pub fn source_positions(&self) -> impl Iterator<Item = crate::bytecode::source_map::SourceMapEntry> + '_ {
        use crate::bytecode::source_map::SourceMapEntry;
        SourceMapEntries::new(&self.source_map).map(|entry| {
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
            + self.rare_data.as_ref().map_or(0, |rare_data| {
                size_of::<RareExecutableData>()
                    + size_of_val(&*rare_data.exception_handlers)
                    + size_of_val(&*rare_data.strict_ranges)
                    + size_of_val(&*rare_data.string_table)
            })
            + size_of_val(&*self.source_map)
            + size_of_val(&*self.identifier_table)
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
