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

use std::collections::HashMap;

use crate::bytecode::basic_block::SourceMapEntry;
use crate::bytecode::ffi::{AbstractOperationKind, ConstantTag, WellKnownSymbolKind};
use crate::bytecode::generator::{
    AssembledBytecode, ConstantValue, ExceptionHandler, FunctionSfdMetadata, Generator, LocalVariable,
    PendingClassBlueprint, PendingClassElement, PendingLiteralValueKind, PendingSharedFunctionData,
    PrecompiledFunction,
};
use crate::{CompiledProgram, CompiledProgramBytecode, ast, u32_from_usize};

const MAGIC: &[u8; 8] = b"LBJSBC\0\0";
const FORMAT_VERSION: u32 = 1;

pub fn serialize_compiled_program(compiled: &CompiledProgram, program_type: ast::ProgramType) -> Vec<u8> {
    let mut encoder = Encoder::new();
    CacheBlob { compiled, program_type }.encode(&mut encoder);
    encoder.finish()
}

pub(crate) fn decode_blob(bytes: &[u8], expected_program_type: ast::ProgramType) -> Option<DecodedCacheBlob> {
    let mut decoder = Decoder::new(bytes);
    let blob = CacheBlob::decode(&mut decoder, expected_program_type)?;
    if !decoder.is_empty() {
        return None;
    }
    blob.validate();
    Some(blob)
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

struct Decoder<'a> {
    bytes: &'a [u8],
}

impl<'a> Decoder<'a> {
    fn new(bytes: &'a [u8]) -> Self {
        Self { bytes }
    }

    fn is_empty(&self) -> bool {
        self.bytes.is_empty()
    }

    fn bytes(&mut self, length: usize) -> Option<&'a [u8]> {
        if self.bytes.len() < length {
            return None;
        }

        let (bytes, rest) = self.bytes.split_at(length);
        self.bytes = rest;
        Some(bytes)
    }

    fn expect_bytes(&mut self, expected: &[u8]) -> Option<()> {
        (self.bytes(expected.len())? == expected).then_some(())
    }

    fn sequence_values<T>(&mut self, mut decode_item: impl FnMut(&mut Self) -> Option<T>) -> Option<Vec<T>> {
        let length: usize = u32::decode(self)?.try_into().ok()?;
        // Reject lengths that cannot fit in the remaining blob even for one-byte items, so a
        // malformed sidecar with a four-billion element header cannot drag the allocator down.
        if length > self.bytes.len() {
            return None;
        }

        let mut values = Vec::with_capacity(length);
        for _ in 0..length {
            values.push(decode_item(self)?);
        }
        Some(values)
    }
}

trait Encode {
    fn encode(&self, encoder: &mut Encoder);
}

trait Decode: Sized {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Self>;
}

impl Encode for bool {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.byte(*self as u8);
    }
}

impl Decode for bool {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Self> {
        match u8::decode(decoder)? {
            0 => Some(false),
            1 => Some(true),
            _ => None,
        }
    }
}

impl Encode for u8 {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.byte(*self);
    }
}

impl Decode for u8 {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Self> {
        decoder.bytes(1)?.first().copied()
    }
}

impl Encode for u32 {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.bytes(&self.to_le_bytes());
    }
}

impl Decode for u32 {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Self> {
        Some(u32::from_le_bytes(decoder.bytes(4)?.try_into().ok()?))
    }
}

impl Encode for i32 {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.bytes(&self.to_le_bytes());
    }
}

impl Decode for i32 {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Self> {
        Some(i32::from_le_bytes(decoder.bytes(4)?.try_into().ok()?))
    }
}

impl Encode for u64 {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.bytes(&self.to_le_bytes());
    }
}

impl Decode for u64 {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Self> {
        Some(u64::from_le_bytes(decoder.bytes(8)?.try_into().ok()?))
    }
}

impl Encode for usize {
    fn encode(&self, encoder: &mut Encoder) {
        u64::try_from(*self).expect("usize does not fit in u64").encode(encoder);
    }
}

impl Decode for usize {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Self> {
        u64::decode(decoder)?.try_into().ok()
    }
}

impl Encode for f64 {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.bytes(&self.to_le_bytes());
    }
}

impl Decode for f64 {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Self> {
        Some(f64::from_le_bytes(decoder.bytes(8)?.try_into().ok()?))
    }
}

impl Decode for ast::Position {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Self> {
        Some(Self {
            line: u32::decode(decoder)?,
            column: u32::decode(decoder)?,
            offset: u32::decode(decoder)?,
        })
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

impl<T: Decode> Decode for Option<T> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Self> {
        if bool::decode(decoder)? {
            Some(Some(T::decode(decoder)?))
        } else {
            Some(None)
        }
    }
}

impl Decode for ast::Utf16String {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Self> {
        let length: usize = u32::decode(decoder)?.try_into().ok()?;
        let bytes = decoder.bytes(length.checked_mul(size_of::<u16>())?)?;
        let mut code_units = Vec::with_capacity(length);
        for chunk in bytes.chunks_exact(size_of::<u16>()) {
            code_units.push(u16::from_le_bytes(chunk.try_into().ok()?));
        }
        Some(code_units.into())
    }
}

struct Bytes<'a>(&'a [u8]);

impl Encode for Bytes<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        u32_from_usize(self.0.len()).encode(encoder);
        encoder.bytes(self.0);
    }
}

struct ByteVector;

impl ByteVector {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Vec<u8>> {
        let length: usize = u32::decode(decoder)?.try_into().ok()?;
        Some(decoder.bytes(length)?.to_vec())
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
    program_type: ast::ProgramType,
}

impl Encode for CacheBlob<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.bytes(MAGIC);
        FORMAT_VERSION.encode(encoder);
        self.program_type.encode(encoder);
        self.compiled.parsed.has_top_level_await.encode(encoder);
        self.compiled.parsed.is_strict_mode.encode(encoder);
        DeclarationMetadataRecord {
            compiled: self.compiled,
            program_type: self.program_type,
        }
        .encode(encoder);
        ProgramRecord::from(self.compiled).encode(encoder);
    }
}

impl CacheBlob<'_> {
    fn decode(decoder: &mut Decoder<'_>, expected_program_type: ast::ProgramType) -> Option<DecodedCacheBlob> {
        decoder.expect_bytes(MAGIC)?;
        (u32::decode(decoder)? == FORMAT_VERSION).then_some(())?;
        let program_type = ast::ProgramType::decode(decoder)?;
        (program_type == expected_program_type).then_some(())?;
        Some(DecodedCacheBlob {
            program_type,
            has_top_level_await: bool::decode(decoder)?,
            is_strict_mode: bool::decode(decoder)?,
            metadata: DeclarationMetadataRecord::decode(decoder)?,
            program: ProgramRecord::decode(decoder)?,
        })
    }
}

pub(crate) struct DecodedCacheBlob {
    program_type: ast::ProgramType,
    has_top_level_await: bool,
    is_strict_mode: bool,
    metadata: DecodedDeclarationMetadata,
    program: DecodedProgramRecord,
}

impl DecodedCacheBlob {
    fn validate(&self) {
        let _ = self.program_type as u8;
        let _ = self.has_top_level_await || self.is_strict_mode;
        self.metadata.validate();
        self.program.validate();
    }
}

impl Encode for ast::ProgramType {
    fn encode(&self, encoder: &mut Encoder) {
        (*self as u8).encode(encoder);
    }
}

impl Decode for ast::ProgramType {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Self> {
        match u8::decode(decoder)? {
            0 => Some(Self::Script),
            1 => Some(Self::Module),
            _ => None,
        }
    }
}

impl Decode for ast::ExportEntryKind {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Self> {
        match u8::decode(decoder)? {
            0 => Some(Self::NamedExport),
            1 => Some(Self::ModuleRequestAll),
            2 => Some(Self::ModuleRequestAllButDefault),
            _ => None,
        }
    }
}

impl Decode for ast::FunctionKind {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Self> {
        match u8::decode(decoder)? {
            0 => Some(Self::Normal),
            1 => Some(Self::Generator),
            2 => Some(Self::Async),
            3 => Some(Self::AsyncGenerator),
            _ => None,
        }
    }
}

struct DeclarationMetadataRecord<'a> {
    compiled: &'a CompiledProgram,
    program_type: ast::ProgramType,
}

impl Encode for DeclarationMetadataRecord<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        let ast::StatementKind::Program(program) = &self.compiled.parsed.program.inner else {
            unreachable!("bytecode cache expects a parsed program root");
        };
        let arena = &self.compiled.parsed.arena;
        let scope = &arena.scopes[program.scope];
        match self.program_type {
            ast::ProgramType::Script => ScriptDeclarationMetadata::from_scope(scope, arena).encode(encoder),
            ast::ProgramType::Module => ModuleDeclarationMetadata::from_scope(scope, arena).encode(encoder),
        }
        DeclarationFunctionTable(&self.compiled.declaration_functions).encode(encoder);
    }
}

impl DeclarationMetadataRecord<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<DecodedDeclarationMetadata> {
        match MetadataKind::decode(decoder)? {
            MetadataKind::Script => Some(DecodedDeclarationMetadata::Script {
                metadata: ScriptDeclarationMetadata::decode_payload(decoder)?,
                declaration_functions: DeclarationFunctionTable::decode(decoder)?,
            }),
            MetadataKind::Module => Some(DecodedDeclarationMetadata::Module {
                metadata: ModuleDeclarationMetadata::decode_payload(decoder)?,
                declaration_functions: DeclarationFunctionTable::decode(decoder)?,
            }),
        }
    }
}

enum DecodedDeclarationMetadata {
    Script {
        metadata: ScriptDeclarationMetadata,
        declaration_functions: Vec<DecodedFunctionRecord>,
    },
    Module {
        metadata: ModuleDeclarationMetadata,
        declaration_functions: Vec<DecodedFunctionRecord>,
    },
}

impl DecodedDeclarationMetadata {
    fn validate(&self) {
        match self {
            Self::Script {
                metadata,
                declaration_functions,
            } => {
                metadata.validate();
                for function in declaration_functions {
                    function.validate();
                }
            }
            Self::Module {
                metadata,
                declaration_functions,
            } => {
                metadata.validate();
                for function in declaration_functions {
                    function.validate();
                }
            }
        }
    }
}

#[repr(u8)]
#[derive(Clone, Copy)]
enum MetadataKind {
    Script = 0,
    Module = 1,
}

impl Encode for MetadataKind {
    fn encode(&self, encoder: &mut Encoder) {
        (*self as u8).encode(encoder);
    }
}

impl Decode for MetadataKind {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Self> {
        match u8::decode(decoder)? {
            0 => Some(Self::Script),
            1 => Some(Self::Module),
            _ => None,
        }
    }
}

struct ScriptDeclarationMetadata {
    lexical_names: Vec<ast::Utf16String>,
    var_names: Vec<ast::Utf16String>,
    function_names: Vec<ast::Utf16String>,
    var_scoped_names: Vec<ast::Utf16String>,
    annex_b_candidate_names: Vec<ast::Utf16String>,
    lexical_bindings: Vec<LexicalBindingRecord>,
}

impl ScriptDeclarationMetadata {
    fn from_scope(scope: &ast::ScopeData, arena: &ast::AstArena) -> Self {
        let mut metadata = Self {
            lexical_names: Vec::new(),
            var_names: Vec::new(),
            function_names: script_function_names(scope, arena),
            var_scoped_names: Vec::new(),
            annex_b_candidate_names: scope.annexb_function_names.to_vec(),
            lexical_bindings: Vec::new(),
        };

        for child in &scope.children {
            collect_var_names_recursive(&child.inner, arena, &mut metadata.var_names);
            if let ast::StatementKind::FunctionDeclaration(ref function) = child.inner
                && let Some(name) = function.name
            {
                metadata.var_names.push(arena.name_of(name).clone());
            }
            collect_script_lexical_names(&child.inner, arena, &mut metadata.lexical_names);
            collect_script_lexical_bindings(&child.inner, arena, &mut metadata.lexical_bindings);
            collect_var_names_recursive(&child.inner, arena, &mut metadata.var_scoped_names);
        }

        metadata
    }

    fn decode_payload(decoder: &mut Decoder<'_>) -> Option<Self> {
        Some(Self {
            lexical_names: Utf16Vector::decode(decoder)?,
            var_names: Utf16Vector::decode(decoder)?,
            function_names: Utf16Vector::decode(decoder)?,
            var_scoped_names: Utf16Vector::decode(decoder)?,
            annex_b_candidate_names: Utf16Vector::decode(decoder)?,
            lexical_bindings: LexicalBindingTable::decode(decoder)?,
        })
    }

    fn validate(&self) {
        let _ = self.lexical_names.len()
            + self.var_names.len()
            + self.function_names.len()
            + self.var_scoped_names.len()
            + self.annex_b_candidate_names.len()
            + self.lexical_bindings.len();
    }
}

impl Encode for ScriptDeclarationMetadata {
    fn encode(&self, encoder: &mut Encoder) {
        MetadataKind::Script.encode(encoder);
        Utf16Vector(&self.lexical_names).encode(encoder);
        Utf16Vector(&self.var_names).encode(encoder);
        Utf16Vector(&self.function_names).encode(encoder);
        Utf16Vector(&self.var_scoped_names).encode(encoder);
        Utf16Vector(&self.annex_b_candidate_names).encode(encoder);
        LexicalBindingTable(&self.lexical_bindings).encode(encoder);
    }
}

struct ModuleDeclarationMetadata {
    import_entries: Vec<ModuleImportEntryRecord>,
    local_exports: Vec<ModuleExportEntryRecord>,
    indirect_exports: Vec<ModuleExportEntryRecord>,
    star_exports: Vec<ModuleExportEntryRecord>,
    requested_modules: Vec<ModuleRequestRecord>,
    default_export_binding_name: Option<ast::Utf16String>,
    var_declared_names: Vec<ast::Utf16String>,
    function_names: Vec<ast::Utf16String>,
    lexical_bindings: Vec<ModuleLexicalBindingRecord>,
}

impl ModuleDeclarationMetadata {
    fn from_scope(scope: &ast::ScopeData, arena: &ast::AstArena) -> Self {
        let mut metadata = Self {
            import_entries: Vec::new(),
            local_exports: Vec::new(),
            indirect_exports: Vec::new(),
            star_exports: Vec::new(),
            requested_modules: requested_modules(scope),
            default_export_binding_name: None,
            var_declared_names: Vec::new(),
            function_names: Vec::new(),
            lexical_bindings: Vec::new(),
        };

        collect_module_imports_and_exports(scope, &mut metadata);

        let mut function_index = 0;
        for child in &scope.children {
            collect_module_var_names(&child.inner, arena, &mut metadata.var_declared_names);

            let (declaration, is_exported) = match &child.inner {
                ast::StatementKind::Export(export_data) => {
                    if let Some(ref statement) = export_data.statement {
                        (&statement.inner, true)
                    } else {
                        continue;
                    }
                }
                other => (other, false),
            };
            collect_module_declaration(declaration, is_exported, function_index, arena, &mut metadata);
            if matches!(declaration, ast::StatementKind::FunctionDeclaration(_)) {
                function_index += 1;
            }
        }

        metadata
    }

    fn decode_payload(decoder: &mut Decoder<'_>) -> Option<Self> {
        Some(Self {
            import_entries: ModuleImportEntryTable::decode(decoder)?,
            local_exports: ModuleExportEntryTable::decode(decoder)?,
            indirect_exports: ModuleExportEntryTable::decode(decoder)?,
            star_exports: ModuleExportEntryTable::decode(decoder)?,
            requested_modules: ModuleRequestTable::decode(decoder)?,
            default_export_binding_name: Option::<ast::Utf16String>::decode(decoder)?,
            var_declared_names: Utf16Vector::decode(decoder)?,
            function_names: Utf16Vector::decode(decoder)?,
            lexical_bindings: ModuleLexicalBindingTable::decode(decoder)?,
        })
    }

    fn validate(&self) {
        let _ = self.import_entries.len()
            + self.local_exports.len()
            + self.indirect_exports.len()
            + self.star_exports.len()
            + self.requested_modules.len()
            + self.var_declared_names.len()
            + self.function_names.len()
            + self.lexical_bindings.len();
        let _ = self
            .default_export_binding_name
            .as_ref()
            .map(|name| name.as_slice().len());
    }
}

impl Encode for ModuleDeclarationMetadata {
    fn encode(&self, encoder: &mut Encoder) {
        MetadataKind::Module.encode(encoder);
        ModuleImportEntryTable(&self.import_entries).encode(encoder);
        ModuleExportEntryTable(&self.local_exports).encode(encoder);
        ModuleExportEntryTable(&self.indirect_exports).encode(encoder);
        ModuleExportEntryTable(&self.star_exports).encode(encoder);
        ModuleRequestTable(&self.requested_modules).encode(encoder);
        self.default_export_binding_name
            .as_ref()
            .map(|name| Utf16(name))
            .encode(encoder);
        Utf16Vector(&self.var_declared_names).encode(encoder);
        Utf16Vector(&self.function_names).encode(encoder);
        ModuleLexicalBindingTable(&self.lexical_bindings).encode(encoder);
    }
}

struct Utf16Vector<'a>(&'a [ast::Utf16String]);

impl Encode for Utf16Vector<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.sequence(self.0, |value, encoder| Utf16(value).encode(encoder));
    }
}

impl Utf16Vector<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Vec<ast::Utf16String>> {
        decoder.sequence_values(ast::Utf16String::decode)
    }
}

struct LexicalBindingRecord {
    name: ast::Utf16String,
    is_constant: bool,
}

impl Encode for LexicalBindingRecord {
    fn encode(&self, encoder: &mut Encoder) {
        Utf16(&self.name).encode(encoder);
        self.is_constant.encode(encoder);
    }
}

struct LexicalBindingTable<'a>(&'a [LexicalBindingRecord]);

impl Encode for LexicalBindingTable<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.sequence(self.0, |binding, encoder| binding.encode(encoder));
    }
}

impl LexicalBindingTable<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Vec<LexicalBindingRecord>> {
        decoder.sequence_values(|decoder| {
            Some(LexicalBindingRecord {
                name: ast::Utf16String::decode(decoder)?,
                is_constant: bool::decode(decoder)?,
            })
        })
    }
}

struct ModuleLexicalBindingRecord {
    name: ast::Utf16String,
    is_constant: bool,
    function_index: i32,
}

impl Encode for ModuleLexicalBindingRecord {
    fn encode(&self, encoder: &mut Encoder) {
        Utf16(&self.name).encode(encoder);
        self.is_constant.encode(encoder);
        self.function_index.encode(encoder);
    }
}

struct ModuleLexicalBindingTable<'a>(&'a [ModuleLexicalBindingRecord]);

impl Encode for ModuleLexicalBindingTable<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.sequence(self.0, |binding, encoder| binding.encode(encoder));
    }
}

impl ModuleLexicalBindingTable<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Vec<ModuleLexicalBindingRecord>> {
        decoder.sequence_values(|decoder| {
            Some(ModuleLexicalBindingRecord {
                name: ast::Utf16String::decode(decoder)?,
                is_constant: bool::decode(decoder)?,
                function_index: i32::decode(decoder)?,
            })
        })
    }
}

#[derive(Clone)]
struct ModuleRequestRecord {
    specifier: ast::Utf16String,
    attributes: Vec<ast::ImportAttribute>,
}

impl From<&ast::ModuleRequest> for ModuleRequestRecord {
    fn from(request: &ast::ModuleRequest) -> Self {
        Self {
            specifier: request.module_specifier.clone(),
            attributes: request.attributes.clone(),
        }
    }
}

impl Encode for ModuleRequestRecord {
    fn encode(&self, encoder: &mut Encoder) {
        Utf16(&self.specifier).encode(encoder);
        ImportAttributeTable(&self.attributes).encode(encoder);
    }
}

impl Decode for ModuleRequestRecord {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Self> {
        Some(Self {
            specifier: ast::Utf16String::decode(decoder)?,
            attributes: ImportAttributeTable::decode(decoder)?,
        })
    }
}

struct ModuleRequestTable<'a>(&'a [ModuleRequestRecord]);

impl Encode for ModuleRequestTable<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.sequence(self.0, |request, encoder| request.encode(encoder));
    }
}

impl ModuleRequestTable<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Vec<ModuleRequestRecord>> {
        decoder.sequence_values(ModuleRequestRecord::decode)
    }
}

struct ImportAttributeTable<'a>(&'a [ast::ImportAttribute]);

impl Encode for ImportAttributeTable<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.sequence(self.0, |attribute, encoder| {
            Utf16(&attribute.key).encode(encoder);
            Utf16(&attribute.value).encode(encoder);
        });
    }
}

impl ImportAttributeTable<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Vec<ast::ImportAttribute>> {
        decoder.sequence_values(|decoder| {
            Some(ast::ImportAttribute {
                key: ast::Utf16String::decode(decoder)?,
                value: ast::Utf16String::decode(decoder)?,
            })
        })
    }
}

struct ModuleImportEntryRecord {
    import_name: Option<ast::Utf16String>,
    local_name: ast::Utf16String,
    module_request: ModuleRequestRecord,
}

impl Encode for ModuleImportEntryRecord {
    fn encode(&self, encoder: &mut Encoder) {
        self.import_name.as_ref().map(|name| Utf16(name)).encode(encoder);
        Utf16(&self.local_name).encode(encoder);
        self.module_request.encode(encoder);
    }
}

struct ModuleImportEntryTable<'a>(&'a [ModuleImportEntryRecord]);

impl Encode for ModuleImportEntryTable<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.sequence(self.0, |entry, encoder| entry.encode(encoder));
    }
}

impl ModuleImportEntryTable<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Vec<ModuleImportEntryRecord>> {
        decoder.sequence_values(|decoder| {
            Some(ModuleImportEntryRecord {
                import_name: Option::<ast::Utf16String>::decode(decoder)?,
                local_name: ast::Utf16String::decode(decoder)?,
                module_request: ModuleRequestRecord::decode(decoder)?,
            })
        })
    }
}

struct ModuleExportEntryRecord {
    kind: ast::ExportEntryKind,
    export_name: Option<ast::Utf16String>,
    local_or_import_name: Option<ast::Utf16String>,
    module_request: Option<ModuleRequestRecord>,
}

impl Encode for ModuleExportEntryRecord {
    fn encode(&self, encoder: &mut Encoder) {
        (self.kind as u8).encode(encoder);
        self.export_name.as_ref().map(|name| Utf16(name)).encode(encoder);
        self.local_or_import_name
            .as_ref()
            .map(|name| Utf16(name))
            .encode(encoder);
        self.module_request.encode(encoder);
    }
}

struct ModuleExportEntryTable<'a>(&'a [ModuleExportEntryRecord]);

impl Encode for ModuleExportEntryTable<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.sequence(self.0, |entry, encoder| entry.encode(encoder));
    }
}

impl ModuleExportEntryTable<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Vec<ModuleExportEntryRecord>> {
        decoder.sequence_values(|decoder| {
            Some(ModuleExportEntryRecord {
                kind: ast::ExportEntryKind::decode(decoder)?,
                export_name: Option::<ast::Utf16String>::decode(decoder)?,
                local_or_import_name: Option::<ast::Utf16String>::decode(decoder)?,
                module_request: Option::<ModuleRequestRecord>::decode(decoder)?,
            })
        })
    }
}

fn collect_script_lexical_names(
    statement: &ast::StatementKind,
    arena: &ast::AstArena,
    names: &mut Vec<ast::Utf16String>,
) {
    match statement {
        ast::StatementKind::VariableDeclaration(declaration) if declaration.kind != ast::DeclarationKind::Var => {
            for declarator in &declaration.declarations {
                for_each_bound_name(&declarator.target, arena, &mut |name| names.push(name.to_vec().into()));
            }
        }
        ast::StatementKind::UsingDeclaration(declarations) => {
            for declarator in declarations.iter() {
                for_each_bound_name(&declarator.target, arena, &mut |name| names.push(name.to_vec().into()));
            }
        }
        ast::StatementKind::ClassDeclaration(class_data) => {
            if let Some(name) = class_data.name {
                names.push(arena.name_of(name).clone());
            }
        }
        _ => {}
    }
}

fn collect_script_lexical_bindings(
    statement: &ast::StatementKind,
    arena: &ast::AstArena,
    bindings: &mut Vec<LexicalBindingRecord>,
) {
    match statement {
        ast::StatementKind::VariableDeclaration(declaration) if declaration.kind != ast::DeclarationKind::Var => {
            let is_constant = declaration.kind == ast::DeclarationKind::Const;
            for declarator in &declaration.declarations {
                for_each_bound_name(&declarator.target, arena, &mut |name| {
                    bindings.push(LexicalBindingRecord {
                        name: name.to_vec().into(),
                        is_constant,
                    });
                });
            }
        }
        ast::StatementKind::UsingDeclaration(declarations) => {
            for declarator in declarations.iter() {
                for_each_bound_name(&declarator.target, arena, &mut |name| {
                    bindings.push(LexicalBindingRecord {
                        name: name.to_vec().into(),
                        is_constant: false,
                    });
                });
            }
        }
        ast::StatementKind::ClassDeclaration(class_data) => {
            if let Some(name) = class_data.name {
                bindings.push(LexicalBindingRecord {
                    name: arena.name_of(name).clone(),
                    is_constant: false,
                });
            }
        }
        _ => {}
    }
}

fn script_function_names(scope: &ast::ScopeData, arena: &ast::AstArena) -> Vec<ast::Utf16String> {
    let mut last_position = HashMap::new();
    for (index, child) in scope.children.iter().enumerate() {
        if let ast::StatementKind::FunctionDeclaration(ref function) = child.inner
            && let Some(name) = function.name
        {
            last_position.insert(arena.identifiers[name].name, index);
        }
    }

    let mut names = Vec::new();
    for (index, child) in scope.children.iter().enumerate() {
        if let ast::StatementKind::FunctionDeclaration(ref function) = child.inner
            && let Some(name) = function.name
            && last_position.get(&arena.identifiers[name].name).copied() == Some(index)
        {
            names.push(arena.name_of(name).clone());
        }
    }
    names
}

fn collect_module_imports_and_exports(scope: &ast::ScopeData, metadata: &mut ModuleDeclarationMetadata) {
    struct ImportEntryWithRequest {
        import_name: Option<ast::Utf16String>,
        local_name: ast::Utf16String,
        module_request: ModuleRequestRecord,
    }

    let mut all_import_entries = Vec::new();

    for child in &scope.children {
        if let ast::StatementKind::Import(import_data) = &child.inner {
            for entry in &import_data.entries {
                let module_request = ModuleRequestRecord::from(&import_data.module_request);
                metadata.import_entries.push(ModuleImportEntryRecord {
                    import_name: entry.import_name.clone(),
                    local_name: entry.local_name.clone(),
                    module_request: module_request.clone(),
                });
                all_import_entries.push(ImportEntryWithRequest {
                    import_name: entry.import_name.clone(),
                    local_name: entry.local_name.clone(),
                    module_request,
                });
            }
        }
    }

    for child in &scope.children {
        let ast::StatementKind::Export(export_data) = &child.inner else {
            continue;
        };

        if export_data.is_default_export && export_data.entries.len() == 1 {
            let entry = &export_data.entries[0];
            let is_declaration = export_data.statement.as_ref().is_some_and(|statement| {
                matches!(
                    statement.inner,
                    ast::StatementKind::FunctionDeclaration(_) | ast::StatementKind::ClassDeclaration(_)
                )
            });
            if !is_declaration {
                metadata.default_export_binding_name = entry.local_or_import_name.clone();
            }
        }

        for entry in &export_data.entries {
            if entry.kind == ast::ExportEntryKind::EmptyNamedExport {
                break;
            }

            let has_module_request = export_data.module_request.is_some();
            if !has_module_request {
                let matching_import = all_import_entries
                    .iter()
                    .find(|import| entry.local_or_import_name.as_ref() == Some(&import.local_name));
                if let Some(import_entry) = matching_import {
                    if import_entry.import_name.is_none() {
                        metadata.local_exports.push(export_record(entry, None));
                    } else {
                        metadata
                            .indirect_exports
                            .push(export_record(entry, Some(&import_entry.module_request)));
                    }
                } else {
                    metadata.local_exports.push(export_record(entry, None));
                }
            } else if entry.kind == ast::ExportEntryKind::ModuleRequestAllButDefault {
                let module_request = export_data.module_request.as_ref().map(ModuleRequestRecord::from);
                metadata
                    .star_exports
                    .push(export_record(entry, module_request.as_ref()));
            } else {
                let module_request = export_data.module_request.as_ref().map(ModuleRequestRecord::from);
                metadata
                    .indirect_exports
                    .push(export_record(entry, module_request.as_ref()));
            }
        }
    }
}

fn export_record(entry: &ast::ExportEntry, module_request: Option<&ModuleRequestRecord>) -> ModuleExportEntryRecord {
    ModuleExportEntryRecord {
        kind: entry.kind,
        export_name: entry.export_name.clone(),
        local_or_import_name: entry.local_or_import_name.clone(),
        module_request: module_request.cloned(),
    }
}

fn requested_modules(scope: &ast::ScopeData) -> Vec<ModuleRequestRecord> {
    let mut modules = Vec::new();
    for child in &scope.children {
        match &child.inner {
            ast::StatementKind::Import(import_data) => {
                modules.push((
                    child.range.start.offset,
                    ModuleRequestRecord::from(&import_data.module_request),
                ));
            }
            ast::StatementKind::Export(export_data) => {
                if let Some(module_request) = &export_data.module_request {
                    modules.push((child.range.start.offset, ModuleRequestRecord::from(module_request)));
                }
            }
            _ => {}
        }
    }
    modules.sort_by_key(|(source_offset, _)| *source_offset);
    modules.into_iter().map(|(_, module_request)| module_request).collect()
}

fn collect_module_declaration(
    declaration: &ast::StatementKind,
    is_exported: bool,
    function_index: i32,
    arena: &ast::AstArena,
    metadata: &mut ModuleDeclarationMetadata,
) {
    let default_name: ast::Utf16String = utf16!("*default*").into();
    match declaration {
        ast::StatementKind::FunctionDeclaration(function) => {
            let Some(name) = function.name else {
                return;
            };
            let is_default = is_exported && arena.name_slice(name) == default_name.as_slice();
            let function_name = if is_default {
                utf16!("default").into()
            } else {
                arena.name_of(name).clone()
            };
            metadata.function_names.push(function_name);
            metadata.lexical_bindings.push(ModuleLexicalBindingRecord {
                name: arena.name_of(name).clone(),
                is_constant: false,
                function_index,
            });
        }
        ast::StatementKind::ClassDeclaration(class_data) => {
            if let Some(name) = class_data.name {
                metadata.lexical_bindings.push(ModuleLexicalBindingRecord {
                    name: arena.name_of(name).clone(),
                    is_constant: false,
                    function_index: -1,
                });
            }
        }
        ast::StatementKind::VariableDeclaration(declaration) if declaration.kind != ast::DeclarationKind::Var => {
            let is_constant = declaration.kind == ast::DeclarationKind::Const;
            for declarator in &declaration.declarations {
                for_each_bound_name(&declarator.target, arena, &mut |name| {
                    metadata.lexical_bindings.push(ModuleLexicalBindingRecord {
                        name: name.to_vec().into(),
                        is_constant,
                        function_index: -1,
                    });
                });
            }
        }
        ast::StatementKind::UsingDeclaration(declarations) => {
            for declarator in declarations.iter() {
                for_each_bound_name(&declarator.target, arena, &mut |name| {
                    metadata.lexical_bindings.push(ModuleLexicalBindingRecord {
                        name: name.to_vec().into(),
                        is_constant: false,
                        function_index: -1,
                    });
                });
            }
        }
        _ => {}
    }
}

fn collect_var_names_recursive(
    statement: &ast::StatementKind,
    arena: &ast::AstArena,
    names: &mut Vec<ast::Utf16String>,
) {
    match statement {
        ast::StatementKind::VariableDeclaration(declaration) if declaration.kind == ast::DeclarationKind::Var => {
            for declarator in &declaration.declarations {
                for_each_bound_name(&declarator.target, arena, &mut |name| names.push(name.to_vec().into()));
            }
        }
        _ => {
            for_each_child_statement(statement, arena, &mut |child| {
                collect_var_names_recursive(child, arena, names);
            });
        }
    }
}

fn collect_module_var_names(statement: &ast::StatementKind, arena: &ast::AstArena, names: &mut Vec<ast::Utf16String>) {
    match statement {
        ast::StatementKind::VariableDeclaration(declaration) if declaration.kind == ast::DeclarationKind::Var => {
            for declarator in &declaration.declarations {
                for_each_bound_name(&declarator.target, arena, &mut |name| names.push(name.to_vec().into()));
            }
        }
        ast::StatementKind::Export(export_data) => {
            if let Some(ref statement) = export_data.statement {
                collect_module_var_names(&statement.inner, arena, names);
            }
        }
        _ => {
            for_each_child_statement(statement, arena, &mut |child| {
                collect_module_var_names(child, arena, names);
            });
        }
    }
}

fn for_each_bound_name(
    target: &ast::VariableDeclaratorTarget,
    arena: &ast::AstArena,
    callback: &mut dyn FnMut(&[u16]),
) {
    match target {
        ast::VariableDeclaratorTarget::Identifier(identifier) => callback(arena.name_slice(*identifier)),
        ast::VariableDeclaratorTarget::BindingPattern(pattern) => {
            for_each_bound_name_in_pattern(pattern, arena, callback);
        }
    }
}

fn for_each_bound_name_in_pattern(
    pattern: &ast::BindingPattern,
    arena: &ast::AstArena,
    callback: &mut dyn FnMut(&[u16]),
) {
    for entry in &pattern.entries {
        match &entry.alias {
            Some(ast::BindingEntryAlias::Identifier(identifier)) => callback(arena.name_slice(*identifier)),
            Some(ast::BindingEntryAlias::BindingPattern(pattern)) => {
                for_each_bound_name_in_pattern(pattern, arena, callback);
            }
            Some(ast::BindingEntryAlias::MemberExpression(_)) => {}
            None => {
                if let Some(ast::BindingEntryName::Identifier(identifier)) = &entry.name {
                    callback(arena.name_slice(*identifier));
                }
            }
        }
    }
}

fn for_each_child_statement(
    statement: &ast::StatementKind,
    arena: &ast::AstArena,
    callback: &mut dyn FnMut(&ast::StatementKind),
) {
    match statement {
        ast::StatementKind::Block(scope) => {
            for child in &arena.scopes[*scope].children {
                callback(&child.inner);
            }
        }
        ast::StatementKind::If(data) => {
            callback(&data.consequent.inner);
            if let Some(alternate) = &data.alternate {
                callback(&alternate.inner);
            }
        }
        ast::StatementKind::While(data) | ast::StatementKind::DoWhile(data) => {
            callback(&data.body.inner);
        }
        ast::StatementKind::With(data) => callback(&data.body.inner),
        ast::StatementKind::For(data) => {
            if let Some(ast::ForInit::Declaration(declaration)) = &data.init {
                callback(&declaration.inner);
            }
            callback(&data.body.inner);
        }
        ast::StatementKind::ForInOf(data) => {
            if let ast::ForInOfLhs::Declaration(declaration) = &data.lhs {
                callback(&declaration.inner);
            }
            callback(&data.body.inner);
        }
        ast::StatementKind::Switch(data) => {
            for case in &data.cases {
                for child in &arena.scopes[case.scope].children {
                    callback(&child.inner);
                }
            }
        }
        ast::StatementKind::Labelled(data) => callback(&data.item.inner),
        ast::StatementKind::Try(data) => {
            callback(&data.block.inner);
            if let Some(catch) = &data.handler {
                callback(&catch.body.inner);
            }
            if let Some(finalizer) = &data.finalizer {
                callback(&finalizer.inner);
            }
        }
        ast::StatementKind::Export(export_data) => {
            if let Some(statement) = &export_data.statement {
                callback(&statement.inner);
            }
        }
        _ => {}
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

impl ProgramRecord<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<DecodedProgramRecord> {
        Some(DecodedProgramRecord {
            kind: ProgramKind::decode(decoder)?,
            executable: ExecutableRecord::decode(decoder)?,
        })
    }
}

struct DecodedProgramRecord {
    kind: ProgramKind,
    executable: DecodedExecutableRecord,
}

impl DecodedProgramRecord {
    fn validate(&self) {
        let _ = self.kind as u8;
        self.executable.validate();
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

impl Decode for ProgramKind {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Self> {
        match u8::decode(decoder)? {
            0 => Some(Self::ScriptOrModule),
            1 => Some(Self::AsyncModule),
            _ => None,
        }
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

impl ExecutableRecord<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<DecodedExecutableRecord> {
        Some(DecodedExecutableRecord {
            strict: bool::decode(decoder)?,
            number_of_registers: u32::decode(decoder)?,
            number_of_arguments: u32::decode(decoder)?,
            cache_counters: CacheCounters::decode(decoder)?,
            this_value_needs_environment_resolution: bool::decode(decoder)?,
            length_identifier: Option::<u32>::decode(decoder)?,
            bytecode: ByteVector::decode(decoder)?,
            identifier_table: Utf16Table::decode(decoder)?,
            property_key_table: Utf16Table::decode(decoder)?,
            string_table: Utf16Table::decode(decoder)?,
            constants: ConstantTable::decode(decoder)?,
            exception_handlers: ExceptionHandlerTable::decode(decoder)?,
            source_map: SourceMapTable::decode(decoder)?,
            basic_block_start_offsets: BasicBlockOffsetTable::decode(decoder)?,
            local_variables: LocalVariableTable::decode(decoder)?,
            shared_functions: SharedFunctionTable::decode(decoder)?,
            class_blueprints: ClassBlueprintTable::decode(decoder)?,
        })
    }
}

struct DecodedExecutableRecord {
    strict: bool,
    number_of_registers: u32,
    number_of_arguments: u32,
    cache_counters: DecodedCacheCounters,
    this_value_needs_environment_resolution: bool,
    length_identifier: Option<u32>,
    bytecode: Vec<u8>,
    identifier_table: Vec<ast::Utf16String>,
    property_key_table: Vec<ast::Utf16String>,
    string_table: Vec<ast::Utf16String>,
    constants: Vec<ConstantValue>,
    exception_handlers: Vec<ExceptionHandler>,
    source_map: Vec<SourceMapEntry>,
    basic_block_start_offsets: Vec<usize>,
    local_variables: Vec<LocalVariable>,
    shared_functions: Vec<DecodedFunctionRecord>,
    class_blueprints: Vec<DecodedClassBlueprintRecord>,
}

impl DecodedExecutableRecord {
    fn validate(&self) {
        let _ = self.strict;
        let _ = self.number_of_registers + self.number_of_arguments;
        self.cache_counters.validate();
        let _ = self.this_value_needs_environment_resolution;
        let _ = self.length_identifier;
        let _ = self.bytecode.len()
            + self.identifier_table.len()
            + self.property_key_table.len()
            + self.string_table.len()
            + self.constants.len()
            + self.exception_handlers.len()
            + self.source_map.len()
            + self.basic_block_start_offsets.len()
            + self.local_variables.len()
            + self.shared_functions.len()
            + self.class_blueprints.len();
        for function in &self.shared_functions {
            function.validate();
        }
        for blueprint in &self.class_blueprints {
            blueprint.validate();
        }
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

impl CacheCounters<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<DecodedCacheCounters> {
        Some(DecodedCacheCounters {
            property_lookup_cache_count: u32::decode(decoder)?,
            global_variable_cache_count: u32::decode(decoder)?,
            template_object_cache_count: u32::decode(decoder)?,
            object_shape_cache_count: u32::decode(decoder)?,
            object_property_iterator_cache_count: u32::decode(decoder)?,
        })
    }
}

struct DecodedCacheCounters {
    property_lookup_cache_count: u32,
    global_variable_cache_count: u32,
    template_object_cache_count: u32,
    object_shape_cache_count: u32,
    object_property_iterator_cache_count: u32,
}

impl DecodedCacheCounters {
    fn validate(&self) {
        let _ = self.property_lookup_cache_count
            + self.global_variable_cache_count
            + self.template_object_cache_count
            + self.object_shape_cache_count
            + self.object_property_iterator_cache_count;
    }
}

struct Utf16Table<'a>(&'a [ast::Utf16String]);

impl Encode for Utf16Table<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.sequence(self.0, |value, encoder| Utf16(value).encode(encoder));
    }
}

impl Utf16Table<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Vec<ast::Utf16String>> {
        decoder.sequence_values(ast::Utf16String::decode)
    }
}

struct ConstantTable<'a>(&'a [ConstantValue]);

impl Encode for ConstantTable<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.sequence(self.0, |constant, encoder| constant.encode(encoder));
    }
}

impl ConstantTable<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Vec<ConstantValue>> {
        decoder.sequence_values(ConstantValue::decode)
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

impl Decode for ConstantValue {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Self> {
        match u8::decode(decoder)? {
            tag if tag == ConstantTag::Number as u8 => Some(Self::Number(f64::decode(decoder)?)),
            tag if tag == ConstantTag::BooleanTrue as u8 => Some(Self::Boolean(true)),
            tag if tag == ConstantTag::BooleanFalse as u8 => Some(Self::Boolean(false)),
            tag if tag == ConstantTag::Null as u8 => Some(Self::Null),
            tag if tag == ConstantTag::Undefined as u8 => Some(Self::Undefined),
            tag if tag == ConstantTag::Empty as u8 => Some(Self::Empty),
            tag if tag == ConstantTag::String as u8 => Some(Self::String(ast::Utf16String::decode(decoder)?)),
            tag if tag == ConstantTag::BigInt as u8 => {
                Some(Self::BigInt(String::from_utf8(ByteVector::decode(decoder)?).ok()?))
            }
            tag if tag == ConstantTag::WellKnownSymbol as u8 => match u8::decode(decoder)? {
                0 => Some(Self::WellKnownSymbol(WellKnownSymbolKind::SymbolIterator)),
                1 => Some(Self::WellKnownSymbol(WellKnownSymbolKind::SymbolAsyncIterator)),
                _ => None,
            },
            tag if tag == ConstantTag::AbstractOperation as u8 => match u8::decode(decoder)? {
                0 => Some(Self::AbstractOperation(AbstractOperationKind::AsyncIteratorClose)),
                1 => Some(Self::AbstractOperation(AbstractOperationKind::GetMethod)),
                2 => Some(Self::AbstractOperation(AbstractOperationKind::GetIteratorDirect)),
                3 => Some(Self::AbstractOperation(AbstractOperationKind::GetIteratorFromMethod)),
                4 => Some(Self::AbstractOperation(AbstractOperationKind::IteratorComplete)),
                _ => None,
            },
            _ => None,
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

impl ExceptionHandlerTable<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Vec<ExceptionHandler>> {
        decoder.sequence_values(|decoder| {
            Some(ExceptionHandler {
                start_offset: u32::decode(decoder)?,
                end_offset: u32::decode(decoder)?,
                handler_offset: u32::decode(decoder)?,
            })
        })
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

impl SourceMapTable<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Vec<SourceMapEntry>> {
        decoder.sequence_values(|decoder| {
            Some(SourceMapEntry {
                bytecode_offset: u32::decode(decoder)?,
                source_start: ast::Position::decode(decoder)?,
                source_end: ast::Position::decode(decoder)?,
            })
        })
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

impl BasicBlockOffsetTable<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Vec<usize>> {
        decoder.sequence_values(usize::decode)
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

impl LocalVariableTable<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Vec<LocalVariable>> {
        decoder.sequence_values(|decoder| {
            Some(LocalVariable {
                name: ast::Utf16String::decode(decoder)?,
                is_lexically_declared: bool::decode(decoder)?,
                is_initialized_during_declaration_instantiation: bool::decode(decoder)?,
            })
        })
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

impl SharedFunctionTable<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Vec<DecodedFunctionRecord>> {
        decoder.sequence_values(FunctionRecord::decode)
    }
}

struct DeclarationFunctionTable<'a>(&'a [PendingSharedFunctionData]);

impl Encode for DeclarationFunctionTable<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.sequence(self.0, |shared_data, encoder| {
            let arena = shared_data
                .arena
                .as_deref()
                .expect("bytecode cache declaration function is missing its AST arena");
            FunctionRecord { shared_data, arena }.encode(encoder);
        });
    }
}

impl DeclarationFunctionTable<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Vec<DecodedFunctionRecord>> {
        decoder.sequence_values(FunctionRecord::decode)
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

impl FunctionRecord<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<DecodedFunctionRecord> {
        Some(DecodedFunctionRecord {
            name: Option::<ast::Utf16String>::decode(decoder)?,
            source_text_start: u32::decode(decoder)?,
            source_text_end: u32::decode(decoder)?,
            function_length: i32::decode(decoder)?,
            formal_parameter_count: u32::decode(decoder)?,
            kind: ast::FunctionKind::decode(decoder)?,
            is_strict_mode: bool::decode(decoder)?,
            is_arrow_function: bool::decode(decoder)?,
            parameter_names: SimpleParameterList::decode(decoder)?,
            uses_this: bool::decode(decoder)?,
            uses_this_from_environment: bool::decode(decoder)?,
            class_field_initializer_name: ClassFieldInitializerName::decode(decoder)?,
            metadata: FunctionSfdMetadata::decode(decoder)?,
            precompiled: PrecompiledFunctionRecord::decode(decoder)?,
        })
    }
}

struct DecodedFunctionRecord {
    name: Option<ast::Utf16String>,
    source_text_start: u32,
    source_text_end: u32,
    function_length: i32,
    formal_parameter_count: u32,
    kind: ast::FunctionKind,
    is_strict_mode: bool,
    is_arrow_function: bool,
    parameter_names: Option<Vec<ast::Utf16String>>,
    uses_this: bool,
    uses_this_from_environment: bool,
    class_field_initializer_name: Option<(ast::Utf16String, bool)>,
    metadata: FunctionSfdMetadata,
    precompiled: DecodedExecutableRecord,
}

impl DecodedFunctionRecord {
    fn validate(&self) {
        let _ = self.name.as_ref().map(|name| name.as_slice().len());
        let _ = self.source_text_start + self.source_text_end + self.formal_parameter_count;
        let _ = self.function_length;
        let _ = self.kind as u8;
        let _ = self.is_strict_mode || self.is_arrow_function || self.uses_this || self.uses_this_from_environment;
        let _ = self.parameter_names.as_ref().map(|names| names.len());
        let _ = self
            .class_field_initializer_name
            .as_ref()
            .map(|(name, _)| name.as_slice().len());
        self.precompiled.validate();
        validate_function_metadata(&self.metadata);
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

impl SimpleParameterList<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Option<Vec<ast::Utf16String>>> {
        if bool::decode(decoder)? {
            Some(Some(decoder.sequence_values(ast::Utf16String::decode)?))
        } else {
            Some(None)
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

impl ClassFieldInitializerName<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Option<(ast::Utf16String, bool)>> {
        Option::<(ast::Utf16String, bool)>::decode(decoder)
    }
}

impl Encode for (Utf16<'_>, bool) {
    fn encode(&self, encoder: &mut Encoder) {
        self.0.encode(encoder);
        self.1.encode(encoder);
    }
}

impl Decode for (ast::Utf16String, bool) {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Self> {
        Some((ast::Utf16String::decode(decoder)?, bool::decode(decoder)?))
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

impl FunctionSfdMetadata {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Self> {
        Some(Self {
            uses_this: bool::decode(decoder)?,
            this_value_needs_environment_resolution: bool::decode(decoder)?,
            function_environment_needed: bool::decode(decoder)?,
            function_environment_bindings_count: usize::decode(decoder)?,
            var_environment_bindings_count: usize::decode(decoder)?,
            might_need_arguments: bool::decode(decoder)?,
            contains_eval: bool::decode(decoder)?,
        })
    }
}

fn validate_function_metadata(metadata: &FunctionSfdMetadata) {
    let _ = metadata.uses_this
        || metadata.this_value_needs_environment_resolution
        || metadata.function_environment_needed
        || metadata.might_need_arguments
        || metadata.contains_eval;
    let _ = metadata.function_environment_bindings_count + metadata.var_environment_bindings_count;
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

impl PrecompiledFunctionRecord<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<DecodedExecutableRecord> {
        ExecutableRecord::decode(decoder)
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

impl ClassBlueprintTable<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Vec<DecodedClassBlueprintRecord>> {
        decoder.sequence_values(ClassBlueprintRecord::decode)
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

impl ClassBlueprintRecord<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<DecodedClassBlueprintRecord> {
        Some(DecodedClassBlueprintRecord {
            name: Option::<ast::Utf16String>::decode(decoder)?,
            source_text_offset: usize::decode(decoder)?,
            source_text_length: usize::decode(decoder)?,
            constructor_sfd_index: u32::decode(decoder)?,
            has_super_class: bool::decode(decoder)?,
            has_name: bool::decode(decoder)?,
            elements: decoder.sequence_values(ClassElementRecord::decode)?,
        })
    }
}

struct DecodedClassBlueprintRecord {
    name: Option<ast::Utf16String>,
    source_text_offset: usize,
    source_text_length: usize,
    constructor_sfd_index: u32,
    has_super_class: bool,
    has_name: bool,
    elements: Vec<DecodedClassElementRecord>,
}

impl DecodedClassBlueprintRecord {
    fn validate(&self) {
        let _ = self.name.as_ref().map(|name| name.as_slice().len());
        let _ = self.source_text_offset + self.source_text_length;
        let _ = self.constructor_sfd_index;
        let _ = self.has_super_class || self.has_name;
        for element in &self.elements {
            element.validate();
        }
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

impl ClassElementRecord<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<DecodedClassElementRecord> {
        Some(DecodedClassElementRecord {
            kind: u8::decode(decoder)?,
            is_static: bool::decode(decoder)?,
            is_private: bool::decode(decoder)?,
            private_identifier: Option::<ast::Utf16String>::decode(decoder)?,
            shared_function_data_index: Option::<u32>::decode(decoder)?,
            has_initializer: bool::decode(decoder)?,
            literal_value_kind: PendingLiteralValueKind::decode(decoder)?,
            literal_value_number: f64::decode(decoder)?,
            literal_value_string: Option::<ast::Utf16String>::decode(decoder)?,
        })
    }
}

struct DecodedClassElementRecord {
    kind: u8,
    is_static: bool,
    is_private: bool,
    private_identifier: Option<ast::Utf16String>,
    shared_function_data_index: Option<u32>,
    has_initializer: bool,
    literal_value_kind: PendingLiteralValueKind,
    literal_value_number: f64,
    literal_value_string: Option<ast::Utf16String>,
}

impl DecodedClassElementRecord {
    fn validate(&self) {
        let _ = self.kind;
        let _ = self.is_static || self.is_private || self.has_initializer;
        let _ = self.private_identifier.as_ref().map(|name| name.as_slice().len());
        let _ = self.shared_function_data_index;
        let _ = literal_value_kind_tag(self.literal_value_kind);
        let _ = self.literal_value_number;
        let _ = self.literal_value_string.as_ref().map(|value| value.as_slice().len());
    }
}

impl Decode for PendingLiteralValueKind {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Self> {
        match u8::decode(decoder)? {
            0 => Some(Self::None),
            1 => Some(Self::Number),
            2 => Some(Self::BooleanTrue),
            3 => Some(Self::BooleanFalse),
            4 => Some(Self::Null),
            5 => Some(Self::String),
            _ => None,
        }
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sequence_decode_rejects_lengths_larger_than_remaining_bytes() {
        let bytes = u32::MAX.to_le_bytes();

        let mut decoder = Decoder::new(&bytes);
        assert!(decoder.sequence_values(u8::decode).is_none());
    }

    #[test]
    fn sequence_decode_rejects_truncated_items_without_large_allocation() {
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&4u32.to_le_bytes());
        bytes.extend_from_slice(&[1, 2, 3]);

        let mut decoder = Decoder::new(&bytes);
        assert!(decoder.sequence_values(u8::decode).is_none());
    }
}
