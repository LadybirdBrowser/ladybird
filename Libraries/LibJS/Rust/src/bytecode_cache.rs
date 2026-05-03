/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Versioned serialization for fully compiled JavaScript bytecode cache blobs.
//!
//! The format is expressed as small record types with `Encode`
//! implementations. The matching decoder should mirror these records instead
//! of growing a separate procedural parser.

use crate::bytecode::ffi::ConstantTag;
use crate::bytecode::generator::{
    AssembledBytecode, ConstantValue, FunctionSfdMetadata, Generator, PendingClassBlueprint, PendingClassElement,
    PendingLiteralValueKind, PendingSharedFunctionData, PrecompiledFunction,
};
use crate::{CompiledProgram, CompiledProgramBytecode, ast, u32_from_usize};

const MAGIC: &[u8; 8] = b"LBJSBC\0\0";
const FORMAT_VERSION: u32 = 1;

pub fn serialize_compiled_program(compiled: &CompiledProgram) -> Vec<u8> {
    let mut encoder = Encoder::new();
    CacheBlob { compiled }.encode(&mut encoder);
    encoder.finish()
}

struct Encoder {
    bytes: Vec<u8>,
}

impl Encoder {
    fn new() -> Self {
        Self { bytes: Vec::new() }
    }

    fn finish(self) -> Vec<u8> {
        self.bytes
    }

    fn byte(&mut self, value: u8) {
        self.bytes.push(value);
    }

    fn bytes(&mut self, bytes: &[u8]) {
        self.bytes.extend_from_slice(bytes);
    }

    fn sequence<T>(&mut self, items: &[T], mut encode_item: impl FnMut(&T, &mut Self)) {
        u32_from_usize(items.len()).encode(self);
        for item in items {
            encode_item(item, self);
        }
    }
}

trait Encode {
    fn encode(&self, encoder: &mut Encoder);
}

impl Encode for bool {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.byte(*self as u8);
    }
}

impl Encode for u8 {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.byte(*self);
    }
}

impl Encode for u32 {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.bytes(&self.to_le_bytes());
    }
}

impl Encode for i32 {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.bytes(&self.to_le_bytes());
    }
}

impl Encode for u64 {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.bytes(&self.to_le_bytes());
    }
}

impl Encode for usize {
    fn encode(&self, encoder: &mut Encoder) {
        u64::try_from(*self).expect("usize does not fit in u64").encode(encoder);
    }
}

impl Encode for f64 {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.bytes(&self.to_le_bytes());
    }
}

impl<T: Encode> Encode for Option<T> {
    fn encode(&self, encoder: &mut Encoder) {
        self.is_some().encode(encoder);
        if let Some(value) = self {
            value.encode(encoder);
        }
    }
}

struct Bytes<'a>(&'a [u8]);

impl Encode for Bytes<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        u32_from_usize(self.0.len()).encode(encoder);
        encoder.bytes(self.0);
    }
}

struct Utf16<'a>(&'a [u16]);

impl Encode for Utf16<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        u32_from_usize(self.0.len()).encode(encoder);
        for code_unit in self.0 {
            encoder.bytes(&code_unit.to_le_bytes());
        }
    }
}

struct CacheBlob<'a> {
    compiled: &'a CompiledProgram,
}

impl Encode for CacheBlob<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.bytes(MAGIC);
        FORMAT_VERSION.encode(encoder);
        self.compiled.parsed.has_top_level_await.encode(encoder);
        self.compiled.parsed.is_strict_mode.encode(encoder);
        ProgramRecord::from(self.compiled).encode(encoder);
    }
}

struct ProgramRecord<'a> {
    kind: ProgramKind,
    executable: ExecutableRecord<'a>,
}

impl<'a> From<&'a CompiledProgram> for ProgramRecord<'a> {
    fn from(compiled: &'a CompiledProgram) -> Self {
        match &compiled.bytecode {
            CompiledProgramBytecode::Program(bytecode) => Self {
                kind: ProgramKind::ScriptOrModule,
                executable: ExecutableRecord {
                    generator: &bytecode.generator,
                    assembled: &bytecode.assembled,
                },
            },
            CompiledProgramBytecode::AsyncModule(bytecode) => Self {
                kind: ProgramKind::AsyncModule,
                executable: ExecutableRecord {
                    generator: &bytecode.generator,
                    assembled: &bytecode.assembled,
                },
            },
        }
    }
}

impl Encode for ProgramRecord<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        self.kind.encode(encoder);
        self.executable.encode(encoder);
    }
}

#[repr(u8)]
#[derive(Clone, Copy)]
enum ProgramKind {
    ScriptOrModule = 0,
    AsyncModule = 1,
}

impl Encode for ProgramKind {
    fn encode(&self, encoder: &mut Encoder) {
        (*self as u8).encode(encoder);
    }
}

struct ExecutableRecord<'a> {
    generator: &'a Generator,
    assembled: &'a AssembledBytecode,
}

impl Encode for ExecutableRecord<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        self.generator.strict.encode(encoder);
        self.assembled.number_of_registers.encode(encoder);
        self.assembled.number_of_arguments.encode(encoder);
        CacheCounters(self.generator).encode(encoder);
        self.generator.this_value_needs_environment_resolution.encode(encoder);
        self.generator.length_identifier.map(|index| index.0).encode(encoder);

        Bytes(&self.assembled.bytecode).encode(encoder);
        Utf16Table(&self.generator.identifier_table).encode(encoder);
        Utf16Table(&self.generator.property_key_table).encode(encoder);
        Utf16Table(&self.generator.string_table).encode(encoder);
        ConstantTable(&self.generator.constants).encode(encoder);
        ExceptionHandlerTable(self.assembled).encode(encoder);
        SourceMapTable(self.assembled).encode(encoder);
        BasicBlockOffsetTable(self.assembled).encode(encoder);
        LocalVariableTable(self.generator).encode(encoder);
        SharedFunctionTable(self.generator).encode(encoder);
        ClassBlueprintTable(self.generator).encode(encoder);
    }
}

struct CacheCounters<'a>(&'a Generator);

impl Encode for CacheCounters<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        self.0.next_property_lookup_cache.encode(encoder);
        self.0.next_global_variable_cache.encode(encoder);
        self.0.next_template_object_cache.encode(encoder);
        self.0.next_object_shape_cache.encode(encoder);
        self.0.next_object_property_iterator_cache.encode(encoder);
    }
}

struct Utf16Table<'a>(&'a [ast::Utf16String]);

impl Encode for Utf16Table<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.sequence(self.0, |value, encoder| Utf16(value).encode(encoder));
    }
}

struct ConstantTable<'a>(&'a [ConstantValue]);

impl Encode for ConstantTable<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.sequence(self.0, |constant, encoder| constant.encode(encoder));
    }
}

impl Encode for ConstantValue {
    fn encode(&self, encoder: &mut Encoder) {
        match self {
            ConstantValue::Number(value) => {
                (ConstantTag::Number as u8).encode(encoder);
                value.encode(encoder);
            }
            ConstantValue::Boolean(true) => (ConstantTag::BooleanTrue as u8).encode(encoder),
            ConstantValue::Boolean(false) => (ConstantTag::BooleanFalse as u8).encode(encoder),
            ConstantValue::Null => (ConstantTag::Null as u8).encode(encoder),
            ConstantValue::Undefined => (ConstantTag::Undefined as u8).encode(encoder),
            ConstantValue::Empty => (ConstantTag::Empty as u8).encode(encoder),
            ConstantValue::String(value) => {
                (ConstantTag::String as u8).encode(encoder);
                Utf16(value).encode(encoder);
            }
            ConstantValue::BigInt(value) => {
                (ConstantTag::BigInt as u8).encode(encoder);
                Bytes(value.as_bytes()).encode(encoder);
            }
            ConstantValue::WellKnownSymbol(symbol) => {
                (ConstantTag::WellKnownSymbol as u8).encode(encoder);
                (*symbol as u8).encode(encoder);
            }
            ConstantValue::AbstractOperation(operation) => {
                (ConstantTag::AbstractOperation as u8).encode(encoder);
                (*operation as u8).encode(encoder);
            }
        }
    }
}

struct ExceptionHandlerTable<'a>(&'a AssembledBytecode);

impl Encode for ExceptionHandlerTable<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.sequence(&self.0.exception_handlers, |handler, encoder| {
            handler.start_offset.encode(encoder);
            handler.end_offset.encode(encoder);
            handler.handler_offset.encode(encoder);
        });
    }
}

struct SourceMapTable<'a>(&'a AssembledBytecode);

impl Encode for SourceMapTable<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.sequence(&self.0.source_map, |entry, encoder| {
            entry.bytecode_offset.encode(encoder);
            entry.source_start.line.encode(encoder);
            entry.source_start.column.encode(encoder);
            entry.source_start.offset.encode(encoder);
            entry.source_end.line.encode(encoder);
            entry.source_end.column.encode(encoder);
            entry.source_end.offset.encode(encoder);
        });
    }
}

struct BasicBlockOffsetTable<'a>(&'a AssembledBytecode);

impl Encode for BasicBlockOffsetTable<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.sequence(&self.0.basic_block_start_offsets, |offset, encoder| {
            offset.encode(encoder);
        });
    }
}

struct LocalVariableTable<'a>(&'a Generator);

impl Encode for LocalVariableTable<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.sequence(&self.0.local_variables, |local_variable, encoder| {
            Utf16(&local_variable.name).encode(encoder);
            local_variable.is_lexically_declared.encode(encoder);
            local_variable
                .is_initialized_during_declaration_instantiation
                .encode(encoder);
        });
    }
}

struct SharedFunctionTable<'a>(&'a Generator);

impl Encode for SharedFunctionTable<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.sequence(&self.0.shared_function_data, |shared_data, encoder| {
            FunctionRecord {
                shared_data,
                arena: &self.0.arena,
            }
            .encode(encoder);
        });
    }
}

struct FunctionRecord<'a> {
    shared_data: &'a PendingSharedFunctionData,
    arena: &'a ast::AstArena,
}

impl Encode for FunctionRecord<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        let function_data = self
            .shared_data
            .function_data
            .as_ref()
            .expect("bytecode cache requires function data to be retained until serialization");
        let precompiled = self
            .shared_data
            .precompiled_function
            .as_ref()
            .expect("fully compiled bytecode cache entry is missing nested function bytecode");

        self.function_name(function_data).encode(encoder);
        function_data.source_text_start.encode(encoder);
        function_data.source_text_end.encode(encoder);
        function_data.function_length.encode(encoder);
        u32_from_usize(function_data.parameters.len()).encode(encoder);
        (function_data.kind as u8).encode(encoder);
        function_data.is_strict_mode.encode(encoder);
        function_data.is_arrow_function.encode(encoder);
        SimpleParameterList {
            function_data,
            arena: self.function_arena(),
        }
        .encode(encoder);
        function_data.parsing_insights.uses_this.encode(encoder);
        function_data
            .parsing_insights
            .uses_this_from_environment
            .encode(encoder);
        ClassFieldInitializerName(self.shared_data).encode(encoder);
        precompiled.metadata.encode(encoder);
        PrecompiledFunctionRecord(precompiled).encode(encoder);
    }
}

impl<'a> FunctionRecord<'a> {
    fn function_name(&self, function_data: &'a ast::FunctionData) -> Option<Utf16<'a>> {
        self.shared_data
            .name_override
            .as_deref()
            .or_else(|| function_data.name.map(|name| self.function_arena().name_slice(name)))
            .map(Utf16)
    }

    fn function_arena(&self) -> &'a ast::AstArena {
        self.shared_data.arena.as_deref().unwrap_or(self.arena)
    }
}

struct SimpleParameterList<'a> {
    function_data: &'a ast::FunctionData,
    arena: &'a ast::AstArena,
}

impl Encode for SimpleParameterList<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        let names = simple_parameter_names(self.function_data, self.arena);
        names.is_some().encode(encoder);
        if let Some(names) = names {
            encoder.sequence(&names, |name, encoder| Utf16(name).encode(encoder));
        }
    }
}

fn simple_parameter_names<'a>(
    function_data: &'a ast::FunctionData,
    arena: &'a ast::AstArena,
) -> Option<Vec<&'a [u16]>> {
    let mut names = Vec::with_capacity(function_data.parameters.len());
    for parameter in &function_data.parameters {
        if parameter.is_rest || parameter.default_value.is_some() {
            return None;
        }
        let ast::FunctionParameterBinding::Identifier(identifier) = &parameter.binding else {
            return None;
        };
        names.push(arena.name_slice(*identifier));
    }
    Some(names)
}

struct ClassFieldInitializerName<'a>(&'a PendingSharedFunctionData);

impl Encode for ClassFieldInitializerName<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        self.0
            .class_field_initializer_name
            .as_ref()
            .map(|(name, is_private)| (Utf16(name.as_slice()), *is_private))
            .encode(encoder);
    }
}

impl Encode for (Utf16<'_>, bool) {
    fn encode(&self, encoder: &mut Encoder) {
        self.0.encode(encoder);
        self.1.encode(encoder);
    }
}

impl Encode for FunctionSfdMetadata {
    fn encode(&self, encoder: &mut Encoder) {
        self.uses_this.encode(encoder);
        self.this_value_needs_environment_resolution.encode(encoder);
        self.function_environment_needed.encode(encoder);
        self.function_environment_bindings_count.encode(encoder);
        self.var_environment_bindings_count.encode(encoder);
        self.might_need_arguments.encode(encoder);
        self.contains_eval.encode(encoder);
    }
}

struct PrecompiledFunctionRecord<'a>(&'a PrecompiledFunction);

impl Encode for PrecompiledFunctionRecord<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        ExecutableRecord {
            generator: &self.0.generator,
            assembled: &self.0.assembled,
        }
        .encode(encoder);
    }
}

struct ClassBlueprintTable<'a>(&'a Generator);

impl Encode for ClassBlueprintTable<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.sequence(&self.0.class_blueprints, |blueprint, encoder| {
            ClassBlueprintRecord(blueprint).encode(encoder);
        });
    }
}

struct ClassBlueprintRecord<'a>(&'a PendingClassBlueprint);

impl Encode for ClassBlueprintRecord<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        self.0.name.as_deref().map(Utf16).encode(encoder);
        self.0.source_text_offset.encode(encoder);
        self.0.source_text_length.encode(encoder);
        self.0.constructor_sfd_index.encode(encoder);
        self.0.has_super_class.encode(encoder);
        self.0.has_name.encode(encoder);
        encoder.sequence(&self.0.elements, |element, encoder| {
            ClassElementRecord(element).encode(encoder);
        });
    }
}

struct ClassElementRecord<'a>(&'a PendingClassElement);

impl Encode for ClassElementRecord<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        self.0.kind.encode(encoder);
        self.0.is_static.encode(encoder);
        self.0.is_private.encode(encoder);
        self.0.private_identifier.as_deref().map(Utf16).encode(encoder);
        self.0.shared_function_data_index.encode(encoder);
        self.0.has_initializer.encode(encoder);
        literal_value_kind_tag(self.0.literal_value_kind).encode(encoder);
        self.0.literal_value_number.encode(encoder);
        self.0.literal_value_string.as_deref().map(Utf16).encode(encoder);
    }
}

fn literal_value_kind_tag(kind: PendingLiteralValueKind) -> u8 {
    match kind {
        PendingLiteralValueKind::None => 0,
        PendingLiteralValueKind::Number => 1,
        PendingLiteralValueKind::BooleanTrue => 2,
        PendingLiteralValueKind::BooleanFalse => 3,
        PendingLiteralValueKind::Null => 4,
        PendingLiteralValueKind::String => 5,
    }
}
