/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Versioned serialization for fully compiled JavaScript bytecode cache blobs.
//!
//! [`serialize_compiled_program()`] writes a program that was compiled with
//! [`FunctionPrecompileMode::All`](crate::compile::FunctionPrecompileMode::All), and [`decode_blob()`] reads it back
//! into views that borrow strings and bytecode from the blob. A runtime builds its executables from those views once
//! [`DecodedCacheBlob::validate_for_materialization()`] has accepted them.
//!
//! Every blob names the [`BytecodeCacheRuntime`] it was written for, and decoding it for the other one fails, so a
//! profile that a build of either runtime filled is safe to use with the other: its blobs only miss.
//!
//! The format is expressed as small record types with `Encode`
//! implementations. The matching decoder should mirror these records instead
//! of growing a separate procedural parser.

use std::borrow::Cow;
use std::collections::HashMap;
use std::ffi::c_void;
use std::ops::Range;
use std::rc::Rc;

use crate::ast;
use crate::bytecode::basic_block::SourceMapEntry;
use crate::bytecode::constant::AbstractOperationKind;
use crate::bytecode::constant::ConstantTag;
use crate::bytecode::constant::WellKnownSymbolKind;
use crate::bytecode::executable_data::ExecutableCacheCounts;
use crate::bytecode::executable_data::ExecutableData;
use crate::bytecode::generator::ConstantValue;
use crate::bytecode::generator::ExceptionHandler;
use crate::bytecode::generator::FunctionSfdMetadata;
use crate::bytecode::generator::LocalVariable;
use crate::bytecode::generator::PendingClassBlueprint;
use crate::bytecode::generator::PendingClassElement;
use crate::bytecode::generator::PendingLiteralValueKind;
use crate::bytecode::generator::PendingSharedFunctionData;
use crate::bytecode::generator::PrecompiledFunction;
use crate::bytecode::validator::FFIExceptionHandlerOffsets;
use crate::bytecode::validator::FFIValidatorBounds;
use crate::bytecode::validator::ValidationErrorKind;
use crate::bytecode::validator::validate_bytecode;
use crate::compile::CompiledProgram;
use crate::compile::CompiledProgramBytecode;
use crate::u32_from_usize;

const MAGIC: &[u8; 8] = b"LBJSBC\0\0";
const FORMAT_VERSION: u32 = 20;
/// The size of the source hash a blob is keyed by.
pub(crate) const SOURCE_HASH_SIZE: usize = 32;
const BYTECODE_ALIGNMENT: usize = 8;
const COMPLETION_TYPE_VARIANT_COUNT: u32 = 6;
const ITERATOR_HINT_VARIANT_COUNT: u32 = 2;
const ENVIRONMENT_MODE_VARIANT_COUNT: u32 = 2;
const PUT_KIND_VARIANT_COUNT: u32 = 5;
const ARGUMENTS_KIND_VARIANT_COUNT: u32 = 2;
const FUNCTION_NAME_PREFIX_VARIANT_COUNT: u32 = 3;

fn source_span_is_valid(start: u32, end: u32, source_len: usize) -> bool {
    let start = start as usize;
    let end = end as usize;
    start <= end && end <= source_len
}

fn source_range_is_valid(offset: usize, length: usize, source_len: usize) -> bool {
    offset <= source_len && length <= source_len - offset
}

/// The runtime that materializes the executables of a blob. Both runtimes run the frontend's bytecode, but each turns
/// a blob into executables of its own, so a blob is only accepted by the runtime it was written for.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BytecodeCacheRuntime {
    Cpp,
    Rust,
}

impl BytecodeCacheRuntime {
    fn tag(self) -> u8 {
        match self {
            Self::Cpp => b'C',
            Self::Rust => b'R',
        }
    }
}

/// Serializes, for `runtime`, a program compiled with
/// [`FunctionPrecompileMode::All`](crate::compile::FunctionPrecompileMode::All) from source that hashes to
/// `source_hash`.
///
/// # Panics
/// Panics if a function of the program was not precompiled.
pub fn serialize_compiled_program(
    compiled: &CompiledProgram,
    program_type: ast::ProgramType,
    source_hash: &[u8; SOURCE_HASH_SIZE],
    runtime: BytecodeCacheRuntime,
) -> Vec<u8> {
    let mut encoder = Encoder::new();
    CacheBlob {
        compiled,
        program_type,
        source_hash,
        runtime,
    }
    .encode(&mut encoder);
    encoder.finish()
}

pub type FreeBytecodeCacheBlobOwner = unsafe extern "C" fn(*mut c_void);

/// An embedder's handle on the bytes of a bytecode cache blob.
pub struct ForeignBytecodeCacheBlobOwner {
    pub owner: *mut c_void,
    /// Releases a handle once nothing decoded from the bytes needs them anymore.
    pub free_owner: FreeBytecodeCacheBlobOwner,
}

/// Decodes a blob that [`serialize_compiled_program()`] wrote for `expected_runtime` and a program of
/// `expected_program_type`, from source that hashes to `expected_source_hash`.
///
/// Strings and bytecode stay in `bytes`, which `owner` keeps alive for as long as anything decoded from them does.
/// `owner` is released when the blob is rejected as well. Returns `None` for a blob of another format version, runtime,
/// program type or source, and for a malformed one.
///
/// # Safety
/// `bytes` must stay alive and unchanged until `owner.free_owner` is called with `owner.owner`.
pub unsafe fn decode_blob(
    bytes: &[u8],
    expected_program_type: ast::ProgramType,
    expected_source_hash: &[u8; SOURCE_HASH_SIZE],
    expected_runtime: BytecodeCacheRuntime,
    owner: ForeignBytecodeCacheBlobOwner,
) -> Option<DecodedCacheBlob> {
    let mut decoder = Decoder::new(bytes, Some(owner));
    let blob = CacheBlob::decode(
        &mut decoder,
        expected_program_type,
        expected_source_hash,
        expected_runtime,
    )?;
    decoder.is_empty().then_some(blob)
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

    fn align_to(&mut self, alignment: usize) {
        let padding = self.bytes.len().next_multiple_of(alignment) - self.bytes.len();
        self.bytes.extend(std::iter::repeat_n(0, padding));
    }

    fn align_bytes_payload_to(&mut self, alignment: usize) {
        let payload_offset = self.bytes.len() + size_of::<u32>();
        let padding = payload_offset.next_multiple_of(alignment) - payload_offset;
        self.bytes.extend(std::iter::repeat_n(0, padding));
    }

    fn sequence<T>(&mut self, items: &[T], mut encode_item: impl FnMut(&T, &mut Self)) {
        u32_from_usize(items.len()).encode(self);
        for item in items {
            encode_item(item, self);
        }
    }
}

struct ForeignBytecodeCacheBlob {
    data: *const u8,
    length: usize,
    owner: *mut c_void,
    free_owner: FreeBytecodeCacheBlobOwner,
}

impl Drop for ForeignBytecodeCacheBlob {
    fn drop(&mut self) {
        unsafe {
            (self.free_owner)(self.owner);
        }
    }
}

struct Decoder<'a> {
    bytes: &'a [u8],
    offset: usize,
    foreign_blob: Option<Rc<ForeignBytecodeCacheBlob>>,
}

impl<'a> Decoder<'a> {
    fn new(bytes: &'a [u8], owner: Option<ForeignBytecodeCacheBlobOwner>) -> Self {
        let foreign_blob = owner.map(|owner| {
            Rc::new(ForeignBytecodeCacheBlob {
                data: bytes.as_ptr(),
                length: bytes.len(),
                owner: owner.owner,
                free_owner: owner.free_owner,
            })
        });
        Self {
            bytes,
            offset: 0,
            foreign_blob,
        }
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
        self.offset = self.offset.checked_add(length)?;
        Some(bytes)
    }

    fn align_to(&mut self, alignment: usize) -> Option<()> {
        let padding = self.offset.next_multiple_of(alignment) - self.offset;
        self.bytes(padding)?;
        Some(())
    }

    fn align_bytes_payload_to(&mut self, alignment: usize) -> Option<()> {
        let payload_offset = self.offset.checked_add(size_of::<u32>())?;
        let padding = payload_offset.next_multiple_of(alignment) - payload_offset;
        self.bytes(padding)?;
        Some(())
    }

    fn bytecode_bytes(&mut self, length: usize) -> Option<DecodedBytecodeBytes> {
        let offset = self.offset;
        self.bytes(length)?;
        let foreign_blob = self.foreign_blob.as_ref()?;
        Some(DecodedBytecodeBytes {
            blob: foreign_blob.clone(),
            range: offset..offset + length,
        })
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
        decoder.align_to(align_of::<u16>())?;
        let length: usize = u32::decode(decoder)?.try_into().ok()?;
        let bytes = decoder.bytes(length.checked_mul(size_of::<u16>())?)?;
        let mut code_units = Vec::with_capacity(length);
        for chunk in bytes.as_chunks::<{ size_of::<u16>() }>().0 {
            code_units.push(u16::from_le_bytes(*chunk));
        }
        Some(code_units.into())
    }
}

pub(crate) enum DecodedUtf16String {
    Owned(ast::Utf16String),
    // The surrounding decoded executable keeps the mapped blob alive through its
    // DecodedBytecodeBytes. Store only the payload pointer here so large string
    // tables do not clone an owner handle for every string.
    Foreign { data: *const u16, length: usize },
}

impl Decode for DecodedUtf16String {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Self> {
        decoder.align_to(align_of::<u16>())?;
        let length: usize = u32::decode(decoder)?.try_into().ok()?;
        let byte_length = length.checked_mul(size_of::<u16>())?;
        let bytes = decoder.bytes(byte_length)?;
        if decoder.foreign_blob.is_some() {
            debug_assert_eq!((bytes.as_ptr() as usize) % align_of::<u16>(), 0);
            return Some(Self::Foreign {
                data: bytes.as_ptr().cast(),
                length,
            });
        }

        let mut code_units = Vec::with_capacity(length);
        for chunk in bytes.as_chunks::<{ size_of::<u16>() }>().0 {
            code_units.push(u16::from_le_bytes(*chunk));
        }
        Some(Self::Owned(code_units.into()))
    }
}

impl From<ast::Utf16String> for DecodedUtf16String {
    fn from(value: ast::Utf16String) -> Self {
        Self::Owned(value)
    }
}

impl DecodedUtf16String {
    pub(crate) fn to_vec(&self) -> Vec<u16> {
        self.code_units().into_owned()
    }

    /// The code units, in place where the blob stores them in the native byte order. A foreign string is only valid
    /// while its blob is, so callers only reach one through a record that keeps the blob alive.
    fn code_units(&self) -> Cow<'_, [u16]> {
        match self {
            Self::Owned(value) => Cow::Borrowed(value.as_slice()),
            #[cfg(target_endian = "little")]
            // SAFETY: The record this string was reached through keeps the blob alive, and the decoder checked that
            //         the code units lie within it and are aligned.
            Self::Foreign { data, length } => Cow::Borrowed(unsafe { std::slice::from_raw_parts(*data, *length) }),
            #[cfg(not(target_endian = "little"))]
            // SAFETY: As above.
            Self::Foreign { data, length } => Cow::Owned(unsafe {
                std::slice::from_raw_parts(*data, *length)
                    .iter()
                    .map(|code_unit| u16::from_le(*code_unit))
                    .collect()
            }),
        }
    }

    fn to_utf16_string(&self) -> ast::Utf16String {
        self.to_vec().into()
    }

    fn to_fly_string(&self) -> ak::Utf16FlyString {
        ak::Utf16FlyString::from_utf16(&self.code_units())
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

/// Bytes of a decoded blob, in place, which keep the whole blob alive.
#[derive(Clone)]
pub struct DecodedBytecodeBytes {
    blob: Rc<ForeignBytecodeCacheBlob>,
    range: Range<usize>,
}

impl DecodedBytecodeBytes {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Self> {
        let length: usize = u32::decode(decoder)?.try_into().ok()?;
        decoder.bytecode_bytes(length)
    }

    pub fn as_slice(&self) -> &[u8] {
        debug_assert!(self.range.end <= self.blob.length);
        // SAFETY: The decoder checked that the range lies within the blob, which the Rc keeps alive.
        unsafe { std::slice::from_raw_parts(self.blob.data.add(self.range.start), self.range.len()) }
    }

    fn decoder(&self) -> Decoder<'_> {
        Decoder {
            bytes: self.as_slice(),
            offset: self.range.start,
            foreign_blob: Some(self.blob.clone()),
        }
    }
}

struct DecodedRecordSequence {
    count: usize,
    bytes: DecodedBytecodeBytes,
}

impl DecodedRecordSequence {
    fn encode<T>(encoder: &mut Encoder, items: &[T], mut encode_item: impl FnMut(&T, &mut Encoder)) {
        Self::encode_with_alignment(encoder, items, align_of::<u16>(), |item, encoder| {
            encode_item(item, encoder);
        });
    }

    fn encode_with_alignment<T>(
        encoder: &mut Encoder,
        items: &[T],
        alignment: usize,
        mut encode_item: impl FnMut(&T, &mut Encoder),
    ) {
        u32_from_usize(items.len()).encode(encoder);

        let mut payload_encoder = Encoder::new();
        for item in items {
            encode_item(item, &mut payload_encoder);
        }

        encoder.align_bytes_payload_to(alignment);
        Bytes(&payload_encoder.finish()).encode(encoder);
    }

    fn decode(decoder: &mut Decoder<'_>) -> Option<Self> {
        Self::decode_with_alignment(decoder, align_of::<u16>())
    }

    fn decode_with_alignment(decoder: &mut Decoder<'_>, alignment: usize) -> Option<Self> {
        let count: usize = u32::decode(decoder)?.try_into().ok()?;
        decoder.align_bytes_payload_to(alignment)?;
        let byte_length: usize = u32::decode(decoder)?.try_into().ok()?;
        // Every record currently has at least one byte in the payload. Reject
        // impossible counts up front so corrupt cache files cannot ask later
        // materialization to reserve huge vectors for tiny payloads.
        if count > byte_length {
            return None;
        }
        Some(Self {
            count,
            bytes: decoder.bytecode_bytes(byte_length)?,
        })
    }

    fn len(&self) -> usize {
        self.count
    }

    fn decoder(&self) -> Decoder<'_> {
        self.bytes.decoder()
    }
}

struct Utf16<'a>(&'a [u16]);

impl Encode for Utf16<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.align_to(align_of::<u16>());
        u32_from_usize(self.0.len()).encode(encoder);
        for code_unit in self.0 {
            encoder.bytes(&code_unit.to_le_bytes());
        }
    }
}

struct CacheBlob<'a> {
    compiled: &'a CompiledProgram,
    program_type: ast::ProgramType,
    // Fingerprint of the encoded source bytes the blob was generated from. Cache writes happen asynchronously after the
    // HTTP response has been served, so the entry on disk may have been replaced for the same (URL, vary key) by the
    // time we go to attach the sidecar. Embedding the source hash makes a stale write harmless: a later read whose
    // source no longer matches will reject the blob and fall through to source compilation.
    source_hash: &'a [u8; SOURCE_HASH_SIZE],
    runtime: BytecodeCacheRuntime,
}

impl Encode for CacheBlob<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        encoder.bytes(MAGIC);
        FORMAT_VERSION.encode(encoder);
        self.runtime.tag().encode(encoder);
        self.program_type.encode(encoder);
        encoder.bytes(self.source_hash);
        u32_from_usize(self.compiled.source_len).encode(encoder);
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
    fn decode(
        decoder: &mut Decoder<'_>,
        expected_program_type: ast::ProgramType,
        expected_source_hash: &[u8; SOURCE_HASH_SIZE],
        expected_runtime: BytecodeCacheRuntime,
    ) -> Option<DecodedCacheBlob> {
        decoder.expect_bytes(MAGIC)?;
        (u32::decode(decoder)? == FORMAT_VERSION).then_some(())?;
        (u8::decode(decoder)? == expected_runtime.tag()).then_some(())?;
        let program_type = ast::ProgramType::decode(decoder)?;
        (program_type == expected_program_type).then_some(())?;
        (decoder.bytes(SOURCE_HASH_SIZE)? == expected_source_hash).then_some(())?;
        let source_len = u32::decode(decoder)? as usize;
        Some(DecodedCacheBlob {
            program_type,
            source_len,
            has_top_level_await: bool::decode(decoder)?,
            is_strict_mode: bool::decode(decoder)?,
            metadata: DeclarationMetadataRecord::decode(decoder)?,
            program: ProgramRecord::decode(decoder)?,
            has_been_validated_for_materialization: false,
        })
    }
}

/// A decoded bytecode cache blob, whose strings and bytecode stay in the blob.
pub struct DecodedCacheBlob {
    pub(crate) program_type: ast::ProgramType,
    pub(crate) source_len: usize,
    pub(crate) has_top_level_await: bool,
    pub(crate) is_strict_mode: bool,
    pub(crate) metadata: DecodedDeclarationMetadata,
    pub(crate) program: DecodedProgramRecord,
    has_been_validated_for_materialization: bool,
}

/// Witnesses that the blob a cached executable came from passed validation for materialization.
#[derive(Clone, Copy)]
pub(crate) enum CachedBytecodeValidation {
    Validated,
}

impl DecodedCacheBlob {
    pub fn program_type(&self) -> ast::ProgramType {
        self.program_type
    }

    pub fn has_top_level_await(&self) -> bool {
        self.has_top_level_await
    }

    pub fn is_strict_mode(&self) -> bool {
        self.is_strict_mode
    }

    /// Checks everything that bytecode built from this blob may rely on for source code of `source_len` code units:
    /// source ranges, the tables and the indices into them, and the bytecode of every executable, including those of
    /// nested functions.
    pub fn validate_for_materialization(&mut self, source_len: usize) -> Result<(), ValidationErrorKind> {
        if self.source_len != source_len {
            return Err(ValidationErrorKind::InvalidLength);
        }
        if self.has_been_validated_for_materialization {
            return Ok(());
        }
        self.metadata.validate_for_materialization(source_len)?;
        self.program.validate_for_materialization(source_len)?;
        self.has_been_validated_for_materialization = true;
        Ok(())
    }

    pub(crate) fn verify_has_been_validated_for_materialization(&self) {
        assert!(
            self.has_been_validated_for_materialization,
            "decoded bytecode cache blob must be validated before materialization"
        );
    }

    /// What declaration instantiation of the program needs. Every record reached from here passed validation.
    ///
    /// # Panics
    /// Panics if the blob has not passed [`DecodedCacheBlob::validate_for_materialization()`].
    pub fn declaration_metadata(&self) -> &DecodedDeclarationMetadata {
        self.verify_has_been_validated_for_materialization();
        &self.metadata
    }

    /// The program's body. Every record reached from here passed validation.
    ///
    /// # Panics
    /// Panics if the blob has not passed [`DecodedCacheBlob::validate_for_materialization()`].
    pub fn program(&self) -> &DecodedProgramRecord {
        self.verify_has_been_validated_for_materialization();
        &self.program
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

/// What declaration instantiation of a cached script or module needs, with the functions it declares.
pub enum DecodedDeclarationMetadata {
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
    fn validate_for_materialization(&self, source_len: usize) -> Result<(), ValidationErrorKind> {
        match self {
            Self::Script {
                declaration_functions, ..
            } => {
                for function in declaration_functions {
                    function.validate_for_materialization(source_len)?;
                }
                Ok(())
            }
            Self::Module {
                metadata,
                declaration_functions,
            } => {
                if declaration_functions.len() != metadata.function_names.len()
                    || metadata.lexical_bindings.iter().any(|binding| {
                        binding.function_index >= 0
                            && !usize::try_from(binding.function_index)
                                .is_ok_and(|index| index < declaration_functions.len())
                    })
                {
                    return Err(ValidationErrorKind::InvalidLength);
                }

                for function in declaration_functions {
                    function.validate_for_materialization(source_len)?;
                }
                Ok(())
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

pub struct ScriptDeclarationMetadata {
    pub lexical_names: Vec<ast::Utf16String>,
    pub var_names: Vec<ast::Utf16String>,
    pub function_names: Vec<ast::Utf16String>,
    pub var_scoped_names: Vec<ast::Utf16String>,
    pub annex_b_candidate_names: Vec<ast::Utf16String>,
    pub lexical_bindings: Vec<LexicalBindingRecord>,
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
            if let Some(function) = child.inner.function_declaration_for_labelled_item()
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

pub struct ModuleDeclarationMetadata {
    pub import_entries: Vec<ModuleImportEntryRecord>,
    pub local_exports: Vec<ModuleExportEntryRecord>,
    pub indirect_exports: Vec<ModuleExportEntryRecord>,
    pub star_exports: Vec<ModuleExportEntryRecord>,
    pub requested_modules: Vec<ModuleRequestRecord>,
    pub default_export_binding_name: Option<ast::Utf16String>,
    pub var_declared_names: Vec<ast::Utf16String>,
    pub function_names: Vec<ast::Utf16String>,
    pub lexical_bindings: Vec<ModuleLexicalBindingRecord>,
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

pub struct LexicalBindingRecord {
    pub name: ast::Utf16String,
    pub is_constant: bool,
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

/// A lexical binding of a module, which the declared function at `function_index` initializes, unless that is -1.
pub struct ModuleLexicalBindingRecord {
    pub name: ast::Utf16String,
    pub is_constant: bool,
    pub function_index: i32,
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
pub struct ModuleRequestRecord {
    pub specifier: ast::Utf16String,
    pub attributes: Vec<ast::ImportAttribute>,
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

impl From<&ModuleRequestRecord> for ast::ModuleRequest {
    fn from(record: &ModuleRequestRecord) -> Self {
        Self {
            module_specifier: record.specifier.clone(),
            attributes: record.attributes.clone(),
        }
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

pub struct ModuleImportEntryRecord {
    pub import_name: Option<ast::Utf16String>,
    pub local_name: ast::Utf16String,
    pub module_request: ModuleRequestRecord,
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

pub struct ModuleExportEntryRecord {
    pub kind: ast::ExportEntryKind,
    pub export_name: Option<ast::Utf16String>,
    pub local_or_import_name: Option<ast::Utf16String>,
    pub module_request: Option<ModuleRequestRecord>,
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
        if let Some(function) = child.inner.function_declaration_for_labelled_item()
            && let Some(name) = function.name
        {
            last_position.insert(arena.identifiers[name].name, index);
        }
    }

    let mut names = Vec::new();
    for (index, child) in scope.children.iter().enumerate() {
        if let Some(function) = child.inner.function_declaration_for_labelled_item()
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
            let is_specific_import_export = all_import_entries.iter().any(|import| {
                entry.local_or_import_name.as_ref() == Some(&import.local_name) && import.import_name.is_some()
            });
            if !is_declaration && !is_specific_import_export {
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
                        metadata.indirect_exports.push(ModuleExportEntryRecord {
                            kind: ast::ExportEntryKind::ModuleRequestAll,
                            export_name: entry.export_name.clone(),
                            local_or_import_name: None,
                            module_request: Some(import_entry.module_request.clone()),
                        });
                    } else {
                        metadata.indirect_exports.push(ModuleExportEntryRecord {
                            kind: entry.kind,
                            export_name: entry.export_name.clone(),
                            local_or_import_name: import_entry.import_name.clone(),
                            module_request: Some(import_entry.module_request.clone()),
                        });
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
            CompiledProgramBytecode::Program(executable) => Self {
                kind: ProgramKind::ScriptOrModule,
                executable: ExecutableRecord(executable),
            },
            CompiledProgramBytecode::AsyncModule(executable) => Self {
                kind: ProgramKind::AsyncModule,
                executable: ExecutableRecord(executable),
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

/// The body of a cached script or module.
pub struct DecodedProgramRecord {
    pub(crate) kind: ProgramKind,
    pub(crate) executable: DecodedExecutableRecord,
}

impl DecodedProgramRecord {
    fn validate_for_materialization(&self, source_len: usize) -> Result<(), ValidationErrorKind> {
        self.executable.validate_for_materialization(source_len)
    }

    /// Whether this is the body of a module with top-level await, which is compiled as that of an async function.
    pub fn is_async_module(&self) -> bool {
        matches!(self.kind, ProgramKind::AsyncModule)
    }

    pub fn executable(&self) -> &DecodedExecutableRecord {
        &self.executable
    }
}

#[repr(u8)]
#[derive(Clone, Copy)]
pub(crate) enum ProgramKind {
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

struct ExecutableRecord<'a>(&'a ExecutableData);

impl Encode for ExecutableRecord<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        self.0.is_strict.encode(encoder);
        self.0.number_of_registers.encode(encoder);
        self.0.number_of_arguments.encode(encoder);
        CacheCounters(&self.0.cache_counts).encode(encoder);
        self.0.this_value_needs_environment_resolution.encode(encoder);
        self.0.length_identifier.map(|index| index.0).encode(encoder);

        encoder.align_bytes_payload_to(BYTECODE_ALIGNMENT);
        Bytes(&self.0.bytecode).encode(encoder);
        for table in [
            &self.0.identifier_table,
            &self.0.property_key_table,
            &self.0.string_table,
        ] {
            DecodedRecordSequence::encode(encoder, table, |value, encoder| {
                Utf16(&value.to_utf16()).encode(encoder);
            });
        }
        ConstantTable(&self.0.constants).encode(encoder);
        ExceptionHandlerTable(&self.0.exception_handlers).encode(encoder);
        SourceMapTable(&self.0.source_map).encode(encoder);
        LocalVariableTable(&self.0.local_variables).encode(encoder);
        DecodedRecordSequence::encode(encoder, &self.0.argument_variable_names, |value, encoder| {
            Utf16(&value.to_utf16()).encode(encoder);
        });
        SharedFunctionTable(&self.0.shared_function_data).encode(encoder);
        ClassBlueprintTable(&self.0.class_blueprints).encode(encoder);
    }
}

impl ExecutableRecord<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<DecodedExecutableRecord> {
        Some(DecodedExecutableRecord {
            strict: bool::decode(decoder)?,
            number_of_registers: u32::decode(decoder)?,
            number_of_arguments: u32::decode(decoder)?,
            cache_counters: CacheCounters::decode(decoder)?,
            length_identifier: {
                // this_value_needs_environment_resolution, which the runtime takes from the metadata of the function
                // that owns the executable instead.
                bool::decode(decoder)?;
                Option::<u32>::decode(decoder)?
            },
            bytecode: {
                decoder.align_bytes_payload_to(BYTECODE_ALIGNMENT)?;
                DecodedBytecodeBytes::decode(decoder)?
            },
            identifier_table: Utf16Table::decode(decoder)?,
            property_key_table: Utf16Table::decode(decoder)?,
            string_table: Utf16Table::decode(decoder)?,
            constants: ConstantTable::decode(decoder)?,
            exception_handlers: ExceptionHandlerTable::decode(decoder)?,
            source_map: SourceMapTable::decode(decoder)?,
            local_variables: LocalVariableTable::decode(decoder)?,
            argument_variable_names: Utf16Table::decode(decoder)?,
            shared_functions: SharedFunctionTable::decode(decoder)?,
            class_blueprints: ClassBlueprintTable::decode(decoder)?,
        })
    }
}

/// One cached executable, whose bytecode stays in the blob. Its tables are decoded on request, into what a runtime
/// creates an executable from; each returns `None` if the blob turns out to be malformed there.
pub struct DecodedExecutableRecord {
    pub(crate) strict: bool,
    pub(crate) number_of_registers: u32,
    pub(crate) number_of_arguments: u32,
    pub(crate) cache_counters: DecodedCacheCounters,
    pub(crate) length_identifier: Option<u32>,
    pub(crate) bytecode: DecodedBytecodeBytes,
    pub(crate) identifier_table: DecodedUtf16Table,
    pub(crate) property_key_table: DecodedUtf16Table,
    pub(crate) string_table: DecodedUtf16Table,
    pub(crate) constants: DecodedConstantTable,
    pub(crate) exception_handlers: DecodedExceptionHandlerTable,
    pub(crate) source_map: DecodedSourceMapTable,
    pub(crate) local_variables: DecodedLocalVariableTable,
    pub(crate) argument_variable_names: DecodedUtf16Table,
    pub(crate) shared_functions: DecodedFunctionTable,
    pub(crate) class_blueprints: DecodedClassBlueprintTable,
}

impl DecodedExecutableRecord {
    fn validate_for_materialization(&self, source_len: usize) -> Result<(), ValidationErrorKind> {
        if self
            .length_identifier
            .is_some_and(|index| (index as usize) >= self.property_key_table.len())
        {
            return Err(ValidationErrorKind::InvalidLength);
        }
        if !self.tables_are_well_formed() {
            return Err(ValidationErrorKind::InvalidLength);
        }
        self.shared_functions.validate_for_materialization(source_len)?;
        self.class_blueprints
            .validate_for_materialization(source_len, self.shared_functions.len())?;

        let bounds = FFIValidatorBounds {
            number_of_registers: self.number_of_registers,
            number_of_locals: self.local_variables.len() as u32,
            number_of_constants: self.constants.len() as u32,
            number_of_arguments: self.number_of_arguments,
            identifier_table_size: self.identifier_table.len() as u32,
            string_table_size: self.string_table.len() as u32,
            property_key_table_size: self.property_key_table.len() as u32,
            regex_table_size: 0,
            property_lookup_cache_count: self.cache_counters.property_lookup_cache_count,
            global_variable_cache_count: self.cache_counters.global_variable_cache_count,
            environment_coordinate_cache_count: self.cache_counters.environment_coordinate_cache_count,
            template_object_cache_count: self.cache_counters.template_object_cache_count,
            object_shape_cache_count: self.cache_counters.object_shape_cache_count,
            object_property_iterator_cache_count: self.cache_counters.object_property_iterator_cache_count,
            environment_shape_cache_count: self.cache_counters.environment_shape_cache_count,
            class_blueprint_count: self.class_blueprints.len() as u32,
            shared_function_data_count: self.shared_functions.len() as u32,
            completion_type_variant_count: COMPLETION_TYPE_VARIANT_COUNT,
            iterator_hint_variant_count: ITERATOR_HINT_VARIANT_COUNT,
            environment_mode_variant_count: ENVIRONMENT_MODE_VARIANT_COUNT,
            put_kind_variant_count: PUT_KIND_VARIANT_COUNT,
            arguments_kind_variant_count: ARGUMENTS_KIND_VARIANT_COUNT,
            function_name_prefix_variant_count: FUNCTION_NAME_PREFIX_VARIANT_COUNT,
        };

        let exception_handlers = self
            .exception_handlers
            .values()
            .ok_or(ValidationErrorKind::InvalidLength)?;
        let exception_handlers: Vec<FFIExceptionHandlerOffsets> = exception_handlers
            .iter()
            .map(|handler| FFIExceptionHandlerOffsets {
                start: handler.start_offset,
                end: handler.end_offset,
                handler: handler.handler_offset,
            })
            .collect();
        let source_map = self.source_map.values().ok_or(ValidationErrorKind::InvalidLength)?;
        let source_map_offsets: Vec<u32> = source_map.iter().map(|entry| entry.bytecode_offset).collect();

        validate_bytecode(
            self.bytecode.as_slice(),
            &bounds,
            &[],
            &exception_handlers,
            &source_map_offsets,
        )
        .map_err(|error| error.kind)?;

        Ok(())
    }

    /// Whether every table that an executable is created from decodes. A function's executable is only created on its
    /// first call, when a malformed table can no longer make the host compile the source instead.
    fn tables_are_well_formed(&self) -> bool {
        self.identifier_table.is_well_formed()
            && self.property_key_table.is_well_formed()
            && self.string_table.is_well_formed()
            && self.argument_variable_names.is_well_formed()
            && self.constants.is_well_formed()
            && self.local_variables.values().is_some()
    }

    pub fn is_strict(&self) -> bool {
        self.strict
    }

    pub fn number_of_registers(&self) -> u32 {
        self.number_of_registers
    }

    pub fn number_of_arguments(&self) -> u32 {
        self.number_of_arguments
    }

    pub fn cache_counts(&self) -> ExecutableCacheCounts {
        let counters = &self.cache_counters;
        ExecutableCacheCounts {
            property_lookup: counters.property_lookup_cache_count,
            global_variable: counters.global_variable_cache_count,
            environment_coordinate: counters.environment_coordinate_cache_count,
            template_object: counters.template_object_cache_count,
            object_shape: counters.object_shape_cache_count,
            object_property_iterator: counters.object_property_iterator_cache_count,
            environment_shape: counters.environment_shape_cache_count,
        }
    }

    /// The index in the property key table of the name the bytecode reads a length through, if any.
    pub fn length_identifier(&self) -> Option<u32> {
        self.length_identifier
    }

    /// The bytecode, in the blob, which these bytes keep alive.
    pub fn bytecode(&self) -> &DecodedBytecodeBytes {
        &self.bytecode
    }

    pub fn identifiers(&self) -> Option<Vec<ak::Utf16FlyString>> {
        self.identifier_table.fly_strings()
    }

    pub fn property_keys(&self) -> Option<Vec<ak::Utf16FlyString>> {
        self.property_key_table.fly_strings()
    }

    pub fn strings(&self) -> Option<Vec<ak::Utf16FlyString>> {
        self.string_table.fly_strings()
    }

    pub fn argument_names(&self) -> Option<Vec<ak::Utf16FlyString>> {
        self.argument_variable_names.fly_strings()
    }

    pub fn constant_values(&self) -> Option<Vec<ConstantValue>> {
        self.constants.values()
    }

    pub fn exception_handler_entries(&self) -> Option<Vec<ExceptionHandler>> {
        self.exception_handlers.values()
    }

    pub fn source_map_entries(&self) -> Option<Vec<SourceMapEntry>> {
        self.source_map.values()
    }

    pub fn locals(&self) -> Option<Vec<LocalVariable>> {
        self.local_variables.local_variables()
    }

    /// The functions the bytecode creates, which refer to it by index.
    pub fn functions(&self) -> Option<Vec<DecodedFunctionRecord>> {
        self.shared_functions.values()
    }

    pub fn classes(&self) -> Option<Vec<PendingClassBlueprint>> {
        Some(
            self.class_blueprints
                .values()?
                .iter()
                .map(PendingClassBlueprint::from)
                .collect(),
        )
    }
}

/// The executable of a cached function, kept in the blob until the function is first called.
pub struct DecodedCachedExecutableRecord {
    bytes: DecodedBytecodeBytes,
    has_been_validated_for_materialization: bool,
}

impl DecodedCachedExecutableRecord {
    /// The executable, decoded.
    ///
    /// # Panics
    /// Panics unless the record came from a blob that passed validation.
    pub fn decode_executable(&self) -> Option<DecodedExecutableRecord> {
        self.verify_has_been_validated_for_materialization();
        self.decode_validated_executable(CachedBytecodeValidation::Validated)
    }

    pub(crate) fn decode_validated_executable(&self, _: CachedBytecodeValidation) -> Option<DecodedExecutableRecord> {
        let mut decoder = self.bytes.decoder();
        let executable = ExecutableRecord::decode(&mut decoder)?;
        decoder.is_empty().then_some(executable)
    }

    pub(crate) fn validated_copy(&self, _: CachedBytecodeValidation) -> Self {
        Self {
            bytes: self.bytes.clone(),
            has_been_validated_for_materialization: true,
        }
    }

    pub(crate) fn verify_has_been_validated_for_materialization(&self) {
        assert!(
            self.has_been_validated_for_materialization,
            "cached bytecode executable must be validated before materialization"
        );
    }

    fn validate_for_materialization(&self, source_len: usize) -> Result<(), ValidationErrorKind> {
        let mut decoder = self.bytes.decoder();
        let executable = ExecutableRecord::decode(&mut decoder).ok_or(ValidationErrorKind::InvalidLength)?;
        if !decoder.is_empty() {
            return Err(ValidationErrorKind::InvalidLength);
        }
        executable.validate_for_materialization(source_len)
    }
}

struct CacheCounters<'a>(&'a ExecutableCacheCounts);

impl Encode for CacheCounters<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        self.0.property_lookup.encode(encoder);
        self.0.global_variable.encode(encoder);
        self.0.environment_coordinate.encode(encoder);
        self.0.template_object.encode(encoder);
        self.0.object_shape.encode(encoder);
        self.0.object_property_iterator.encode(encoder);
        self.0.environment_shape.encode(encoder);
    }
}

impl CacheCounters<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<DecodedCacheCounters> {
        Some(DecodedCacheCounters {
            property_lookup_cache_count: u32::decode(decoder)?,
            global_variable_cache_count: u32::decode(decoder)?,
            environment_coordinate_cache_count: u32::decode(decoder)?,
            template_object_cache_count: u32::decode(decoder)?,
            object_shape_cache_count: u32::decode(decoder)?,
            object_property_iterator_cache_count: u32::decode(decoder)?,
            environment_shape_cache_count: u32::decode(decoder)?,
        })
    }
}

pub(crate) struct DecodedCacheCounters {
    pub(crate) property_lookup_cache_count: u32,
    pub(crate) global_variable_cache_count: u32,
    pub(crate) environment_coordinate_cache_count: u32,
    pub(crate) template_object_cache_count: u32,
    pub(crate) object_shape_cache_count: u32,
    pub(crate) object_property_iterator_cache_count: u32,
    pub(crate) environment_shape_cache_count: u32,
}

struct Utf16Table<'a>(&'a [ast::Utf16String]);

impl Encode for Utf16Table<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        DecodedRecordSequence::encode(encoder, self.0, |value, encoder| Utf16(value).encode(encoder));
    }
}

impl Utf16Table<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<DecodedUtf16Table> {
        Some(DecodedUtf16Table {
            sequence: DecodedRecordSequence::decode(decoder)?,
        })
    }
}

pub(crate) struct DecodedUtf16Table {
    sequence: DecodedRecordSequence,
}

impl DecodedUtf16Table {
    fn len(&self) -> usize {
        self.sequence.len()
    }

    pub(crate) fn values(&self) -> Option<Vec<DecodedUtf16String>> {
        let mut decoder = self.sequence.decoder();
        let mut values = Vec::with_capacity(self.sequence.len());
        for _ in 0..self.sequence.len() {
            values.push(DecodedUtf16String::decode(&mut decoder)?);
        }
        decoder.is_empty().then_some(values)
    }

    fn is_well_formed(&self) -> bool {
        let mut decoder = self.sequence.decoder();
        (0..self.sequence.len()).all(|_| DecodedUtf16String::decode(&mut decoder).is_some()) && decoder.is_empty()
    }

    fn fly_strings(&self) -> Option<Vec<ak::Utf16FlyString>> {
        // The strings point into the bytes of this table, which keep the blob alive while they are converted.
        Some(self.values()?.iter().map(DecodedUtf16String::to_fly_string).collect())
    }
}

struct ConstantTable<'a>(&'a [ConstantValue]);

impl Encode for ConstantTable<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        u32_from_usize(self.0.len()).encode(encoder);

        let mut constant_encoder = Encoder::new();
        for constant in self.0 {
            constant.encode(&mut constant_encoder);
        }
        Bytes(&constant_encoder.finish()).encode(encoder);
    }
}

impl ConstantTable<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<DecodedConstantTable> {
        let count: usize = u32::decode(decoder)?.try_into().ok()?;
        let byte_length: usize = u32::decode(decoder)?.try_into().ok()?;
        // Constants are encoded as at least their one-byte tag.
        if count > byte_length {
            return None;
        }
        Some(DecodedConstantTable {
            count,
            bytes: decoder.bytecode_bytes(byte_length)?,
        })
    }
}

pub(crate) struct DecodedConstantTable {
    count: usize,
    bytes: DecodedBytecodeBytes,
}

impl DecodedConstantTable {
    fn len(&self) -> usize {
        self.count
    }

    /// Whether every constant is one that the runtime creates a value from.
    fn is_well_formed(&self) -> bool {
        let mut decoder = Decoder::new(self.bytes.as_slice(), None);
        (0..self.count).all(|_| validate_constant_value(&mut decoder).is_some()) && decoder.is_empty()
    }

    fn values(&self) -> Option<Vec<ConstantValue>> {
        let mut decoder = Decoder::new(self.bytes.as_slice(), None);
        let mut values = Vec::with_capacity(self.count);
        for _ in 0..self.count {
            values.push(ConstantValue::decode(&mut decoder)?);
        }
        decoder.is_empty().then_some(values)
    }
}

fn validate_constant_value(decoder: &mut Decoder<'_>) -> Option<()> {
    match u8::decode(decoder)? {
        tag if tag == ConstantTag::Number as u8 => {
            f64::decode(decoder)?;
        }
        tag if tag == ConstantTag::BooleanTrue as u8 => {}
        tag if tag == ConstantTag::BooleanFalse as u8 => {}
        tag if tag == ConstantTag::Null as u8 => {}
        tag if tag == ConstantTag::Undefined as u8 => {}
        tag if tag == ConstantTag::Empty as u8 => {}
        tag if tag == ConstantTag::String as u8 => {
            decoder.align_to(align_of::<u16>())?;
            let length: usize = u32::decode(decoder)?.try_into().ok()?;
            decoder.bytes(length.checked_mul(size_of::<u16>())?)?;
        }
        tag if tag == ConstantTag::BigInt as u8 => {
            let length: usize = u32::decode(decoder)?.try_into().ok()?;
            is_big_int_constant(decoder.bytes(length)?).then_some(())?;
        }
        tag if tag == ConstantTag::WellKnownSymbol as u8 => match u8::decode(decoder)? {
            0 | 1 => {}
            _ => return None,
        },
        tag if tag == ConstantTag::AbstractOperation as u8 => match u8::decode(decoder)? {
            0..=4 => {}
            _ => return None,
        },
        _ => return None,
    }

    Some(())
}

/// Whether `literal` is a BigInt as the frontend writes one into a constant table, which the runtime parses: the
/// digits of a literal, after its 0x, 0o or 0b prefix and with the numeric separators of its source text, or the
/// decimal digits of a folded value, which may be negative.
fn is_big_int_constant(literal: &[u8]) -> bool {
    let (radix, digits) = match literal {
        [b'0', b'x' | b'X', digits @ ..] if !digits.is_empty() => (16, digits),
        [b'0', b'o' | b'O', digits @ ..] if !digits.is_empty() => (8, digits),
        [b'0', b'b' | b'B', digits @ ..] if !digits.is_empty() => (2, digits),
        [b'-', digits @ ..] => (10, digits),
        digits => (10, digits),
    };
    let is_digit = |byte: &u8| char::from(*byte).is_digit(radix);
    digits.first().is_some_and(is_digit)
        && digits.last().is_some_and(is_digit)
        && digits.windows(2).all(|pair| pair != b"__")
        && digits.iter().all(|byte| *byte == b'_' || is_digit(byte))
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
                let bytes = ByteVector::decode(decoder)?;
                bytes.is_ascii().then_some(())?;
                Some(Self::BigInt(bytes.into_iter().map(char::from).collect()))
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

struct ExceptionHandlerTable<'a>(&'a [ExceptionHandler]);

impl Encode for ExceptionHandlerTable<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        DecodedRecordSequence::encode(encoder, self.0, |handler, encoder| {
            handler.start_offset.encode(encoder);
            handler.end_offset.encode(encoder);
            handler.handler_offset.encode(encoder);
            handler.catches_exception.encode(encoder);
        });
    }
}

impl ExceptionHandlerTable<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<DecodedExceptionHandlerTable> {
        Some(DecodedExceptionHandlerTable {
            sequence: DecodedRecordSequence::decode(decoder)?,
        })
    }
}

pub(crate) struct DecodedExceptionHandlerTable {
    sequence: DecodedRecordSequence,
}

impl DecodedExceptionHandlerTable {
    pub(crate) fn values(&self) -> Option<Vec<ExceptionHandler>> {
        let mut decoder = self.sequence.decoder();
        let mut values = Vec::with_capacity(self.sequence.len());
        for _ in 0..self.sequence.len() {
            values.push(ExceptionHandler {
                start_offset: u32::decode(&mut decoder)?,
                end_offset: u32::decode(&mut decoder)?,
                handler_offset: u32::decode(&mut decoder)?,
                catches_exception: bool::decode(&mut decoder)?,
            });
        }
        decoder.is_empty().then_some(values)
    }
}

struct SourceMapTable<'a>(&'a [SourceMapEntry]);

impl Encode for SourceMapTable<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        DecodedRecordSequence::encode(encoder, self.0, |entry, encoder| {
            entry.bytecode_offset.encode(encoder);
            entry.line.encode(encoder);
            entry.column.encode(encoder);
        });
    }
}

impl SourceMapTable<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<DecodedSourceMapTable> {
        Some(DecodedSourceMapTable {
            sequence: DecodedRecordSequence::decode(decoder)?,
        })
    }
}

pub(crate) struct DecodedSourceMapTable {
    sequence: DecodedRecordSequence,
}

impl DecodedSourceMapTable {
    pub(crate) fn values(&self) -> Option<Vec<SourceMapEntry>> {
        let mut decoder = self.sequence.decoder();
        let mut values = Vec::with_capacity(self.sequence.len());
        for _ in 0..self.sequence.len() {
            values.push(SourceMapEntry {
                bytecode_offset: u32::decode(&mut decoder)?,
                line: u32::decode(&mut decoder)?,
                column: u32::decode(&mut decoder)?,
            });
        }
        decoder.is_empty().then_some(values)
    }
}

struct LocalVariableTable<'a>(&'a [LocalVariable]);

impl Encode for LocalVariableTable<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        DecodedRecordSequence::encode(encoder, self.0, |local_variable, encoder| {
            Utf16(&local_variable.name.to_utf16()).encode(encoder);
            local_variable.is_lexically_declared.encode(encoder);
            local_variable
                .is_initialized_during_declaration_instantiation
                .encode(encoder);
            local_variable.is_mutable.encode(encoder);
            local_variable.scope_range.is_some().encode(encoder);
            if let Some(range) = local_variable.scope_range {
                range.start.line.encode(encoder);
                range.start.column.encode(encoder);
                range.end.line.encode(encoder);
                range.end.column.encode(encoder);
            }
        });
    }
}

impl LocalVariableTable<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<DecodedLocalVariableTable> {
        Some(DecodedLocalVariableTable {
            sequence: DecodedRecordSequence::decode(decoder)?,
        })
    }
}

pub(crate) struct DecodedLocalVariableTable {
    sequence: DecodedRecordSequence,
}

impl DecodedLocalVariableTable {
    fn len(&self) -> usize {
        self.sequence.len()
    }

    pub(crate) fn values(&self) -> Option<Vec<DecodedLocalVariable>> {
        let mut decoder = self.sequence.decoder();
        let mut values = Vec::with_capacity(self.sequence.len());
        for _ in 0..self.sequence.len() {
            values.push(DecodedLocalVariable {
                name: DecodedUtf16String::decode(&mut decoder)?,
                is_lexically_declared: bool::decode(&mut decoder)?,
                is_initialized_during_declaration_instantiation: bool::decode(&mut decoder)?,
                is_mutable: bool::decode(&mut decoder)?,
                scope_range: if bool::decode(&mut decoder)? {
                    Some(crate::ast::SourceRange {
                        start: crate::ast::Position {
                            line: u32::decode(&mut decoder)?,
                            column: u32::decode(&mut decoder)?,
                            offset: 0,
                        },
                        end: crate::ast::Position {
                            line: u32::decode(&mut decoder)?,
                            column: u32::decode(&mut decoder)?,
                            offset: 0,
                        },
                    })
                } else {
                    None
                },
            });
        }
        decoder.is_empty().then_some(values)
    }

    fn local_variables(&self) -> Option<Vec<LocalVariable>> {
        // The names point into the bytes of this table, which keep the blob alive while they are converted.
        Some(
            self.values()?
                .into_iter()
                .map(|local_variable| LocalVariable {
                    name: local_variable.name.to_fly_string(),
                    is_lexically_declared: local_variable.is_lexically_declared,
                    is_initialized_during_declaration_instantiation: local_variable
                        .is_initialized_during_declaration_instantiation,
                    is_mutable: local_variable.is_mutable,
                    scope_range: local_variable.scope_range,
                })
                .collect(),
        )
    }
}

pub(crate) struct DecodedLocalVariable {
    pub(crate) name: DecodedUtf16String,
    pub(crate) is_lexically_declared: bool,
    pub(crate) is_initialized_during_declaration_instantiation: bool,
    pub(crate) is_mutable: bool,
    pub(crate) scope_range: Option<crate::ast::SourceRange>,
}

struct SharedFunctionTable<'a>(&'a [PendingSharedFunctionData]);

impl Encode for SharedFunctionTable<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        DecodedRecordSequence::encode_with_alignment(encoder, self.0, BYTECODE_ALIGNMENT, |shared_data, encoder| {
            FunctionRecord(shared_data).encode(encoder);
        });
    }
}

impl SharedFunctionTable<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<DecodedFunctionTable> {
        Some(DecodedFunctionTable {
            sequence: DecodedRecordSequence::decode_with_alignment(decoder, BYTECODE_ALIGNMENT)?,
        })
    }
}

pub(crate) struct DecodedFunctionTable {
    sequence: DecodedRecordSequence,
}

impl DecodedFunctionTable {
    fn len(&self) -> usize {
        self.sequence.len()
    }

    pub(crate) fn values(&self) -> Option<Vec<DecodedFunctionRecord>> {
        let mut decoder = self.sequence.decoder();
        let mut values = Vec::with_capacity(self.sequence.len());
        for _ in 0..self.sequence.len() {
            values.push(FunctionRecord::decode(&mut decoder)?);
        }
        decoder.is_empty().then_some(values)
    }

    fn validate_for_materialization(&self, source_len: usize) -> Result<(), ValidationErrorKind> {
        let mut decoder = self.sequence.decoder();
        for _ in 0..self.sequence.len() {
            let function = FunctionRecord::decode(&mut decoder).ok_or(ValidationErrorKind::InvalidLength)?;
            function.validate_for_materialization(source_len)?;
        }
        decoder
            .is_empty()
            .then_some(())
            .ok_or(ValidationErrorKind::InvalidLength)
    }
}

struct DeclarationFunctionTable<'a>(&'a [PendingSharedFunctionData]);

impl Encode for DeclarationFunctionTable<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        u32_from_usize(self.0.len()).encode(encoder);
        let mut payload_encoder = Encoder::new();
        for shared_data in self.0 {
            FunctionRecord(shared_data).encode(&mut payload_encoder);
        }
        encoder.align_bytes_payload_to(BYTECODE_ALIGNMENT);
        Bytes(&payload_encoder.finish()).encode(encoder);
    }
}

impl DeclarationFunctionTable<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Vec<DecodedFunctionRecord>> {
        let count: usize = u32::decode(decoder)?.try_into().ok()?;
        decoder.align_bytes_payload_to(BYTECODE_ALIGNMENT)?;
        let byte_length: usize = u32::decode(decoder)?.try_into().ok()?;
        if count > byte_length {
            return None;
        }
        let bytes = decoder.bytecode_bytes(byte_length)?;
        let mut decoder = bytes.decoder();
        let mut values = Vec::with_capacity(count);
        for _ in 0..count {
            values.push(FunctionRecord::decode(&mut decoder)?);
        }
        decoder.is_empty().then_some(values)
    }
}

struct FunctionRecord<'a>(&'a PendingSharedFunctionData);

impl Encode for FunctionRecord<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        let function_data = self
            .0
            .function_data
            .as_ref()
            .expect("bytecode cache requires function data to be retained until serialization");
        let precompiled = self
            .0
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
        ClassFieldInitializerName(self.0).encode(encoder);
        precompiled.metadata.encode(encoder);
        PrecompiledFunctionRecord(precompiled).encode(encoder);
    }
}

impl FunctionRecord<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<DecodedFunctionRecord> {
        Some(DecodedFunctionRecord {
            name: Option::<DecodedUtf16String>::decode(decoder)?,
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

/// One function a cached executable creates or a cached program declares. The strings it returns stay in the blob.
pub struct DecodedFunctionRecord {
    pub(crate) name: Option<DecodedUtf16String>,
    pub(crate) source_text_start: u32,
    pub(crate) source_text_end: u32,
    pub(crate) function_length: i32,
    pub(crate) formal_parameter_count: u32,
    pub(crate) kind: ast::FunctionKind,
    pub(crate) is_strict_mode: bool,
    pub(crate) is_arrow_function: bool,
    pub(crate) parameter_names: Option<Vec<DecodedUtf16String>>,
    pub(crate) uses_this: bool,
    pub(crate) uses_this_from_environment: bool,
    pub(crate) class_field_initializer_name: Option<(DecodedUtf16String, bool)>,
    pub(crate) metadata: FunctionSfdMetadata,
    pub(crate) precompiled: DecodedCachedExecutableRecord,
}

impl DecodedFunctionRecord {
    fn validate_for_materialization(&self, source_len: usize) -> Result<(), ValidationErrorKind> {
        if !source_span_is_valid(self.source_text_start, self.source_text_end, source_len) {
            return Err(ValidationErrorKind::InvalidLength);
        }
        self.precompiled.validate_for_materialization(source_len)
    }

    // NB: The strings of a record point into the blob, which the record's own bytecode keeps alive.
    pub fn function_name(&self) -> Option<Cow<'_, [u16]>> {
        self.name.as_ref().map(DecodedUtf16String::code_units)
    }

    /// Where the function's source text starts and ends in the source code, in code units.
    pub fn source_text_range(&self) -> Range<usize> {
        self.source_text_start as usize..self.source_text_end as usize
    }

    pub fn length(&self) -> i32 {
        self.function_length
    }

    pub fn parameter_count(&self) -> u32 {
        self.formal_parameter_count
    }

    pub fn function_kind(&self) -> ast::FunctionKind {
        self.kind
    }

    /// Whether the function's own code is strict, which does not count code it is nested in.
    pub fn has_strict_code(&self) -> bool {
        self.is_strict_mode
    }

    pub fn is_arrow(&self) -> bool {
        self.is_arrow_function
    }

    /// The names of the parameters if the parameter list is simple, and `None` otherwise.
    pub fn simple_parameter_names(&self) -> Option<Vec<ak::Utf16FlyString>> {
        Some(
            self.parameter_names
                .as_ref()?
                .iter()
                .map(DecodedUtf16String::to_fly_string)
                .collect(),
        )
    }

    /// What parsing found out about the function's use of `this`: whether it does, and whether it resolves `this`
    /// through its environment.
    pub fn parsing_insights_about_this(&self) -> (bool, bool) {
        (self.uses_this, self.uses_this_from_environment)
    }

    /// The name of the field whose initializer the function is, and whether that field is private.
    pub fn field_initializer_name(&self) -> Option<(Cow<'_, [u16]>, bool)> {
        self.class_field_initializer_name
            .as_ref()
            .map(|(name, is_private)| (name.code_units(), *is_private))
    }

    pub fn scope_metadata(&self) -> &FunctionSfdMetadata {
        &self.metadata
    }

    /// The function's own executable, which stays in the blob until the function is first called.
    pub fn cached_executable(&self) -> DecodedCachedExecutableRecord {
        // Records are only reachable from outside this module through a blob that passed validation.
        self.precompiled.validated_copy(CachedBytecodeValidation::Validated)
    }
}

impl<'a> FunctionRecord<'a> {
    fn function_name(&self, function_data: &'a ast::FunctionData) -> Option<Utf16<'a>> {
        self.0
            .name_override
            .as_deref()
            .or_else(|| function_data.name.map(|name| self.function_arena().name_slice(name)))
            .map(Utf16)
    }

    fn function_arena(&self) -> &'a ast::AstArena {
        self.0
            .arena
            .as_deref()
            .expect("bytecode cache function is missing its AST arena")
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
    fn decode(decoder: &mut Decoder<'_>) -> Option<Option<Vec<DecodedUtf16String>>> {
        if bool::decode(decoder)? {
            Some(Some(decoder.sequence_values(DecodedUtf16String::decode)?))
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
    fn decode(decoder: &mut Decoder<'_>) -> Option<Option<(DecodedUtf16String, bool)>> {
        Option::<(DecodedUtf16String, bool)>::decode(decoder)
    }
}

impl Encode for (Utf16<'_>, bool) {
    fn encode(&self, encoder: &mut Encoder) {
        self.0.encode(encoder);
        self.1.encode(encoder);
    }
}

impl Decode for (DecodedUtf16String, bool) {
    fn decode(decoder: &mut Decoder<'_>) -> Option<Self> {
        Some((DecodedUtf16String::decode(decoder)?, bool::decode(decoder)?))
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

struct PrecompiledFunctionRecord<'a>(&'a PrecompiledFunction);

impl Encode for PrecompiledFunctionRecord<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        let mut payload_encoder = Encoder::new();
        ExecutableRecord(&self.0.executable).encode(&mut payload_encoder);
        encoder.align_bytes_payload_to(BYTECODE_ALIGNMENT);
        Bytes(&payload_encoder.finish()).encode(encoder);
    }
}

impl PrecompiledFunctionRecord<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<DecodedCachedExecutableRecord> {
        decoder.align_bytes_payload_to(BYTECODE_ALIGNMENT)?;
        Some(DecodedCachedExecutableRecord {
            bytes: DecodedBytecodeBytes::decode(decoder)?,
            has_been_validated_for_materialization: false,
        })
    }
}

struct ClassBlueprintTable<'a>(&'a [PendingClassBlueprint]);

impl Encode for ClassBlueprintTable<'_> {
    fn encode(&self, encoder: &mut Encoder) {
        DecodedRecordSequence::encode(encoder, self.0, |blueprint, encoder| {
            ClassBlueprintRecord(blueprint).encode(encoder);
        });
    }
}

impl ClassBlueprintTable<'_> {
    fn decode(decoder: &mut Decoder<'_>) -> Option<DecodedClassBlueprintTable> {
        Some(DecodedClassBlueprintTable {
            sequence: DecodedRecordSequence::decode(decoder)?,
        })
    }
}

pub(crate) struct DecodedClassBlueprintTable {
    sequence: DecodedRecordSequence,
}

impl DecodedClassBlueprintTable {
    fn len(&self) -> usize {
        self.sequence.len()
    }

    pub(crate) fn values(&self) -> Option<Vec<DecodedClassBlueprintRecord>> {
        let mut decoder = self.sequence.decoder();
        let mut values = Vec::with_capacity(self.sequence.len());
        for _ in 0..self.sequence.len() {
            values.push(ClassBlueprintRecord::decode(&mut decoder)?);
        }
        decoder.is_empty().then_some(values)
    }

    fn for_each(&self, mut callback: impl FnMut(DecodedClassBlueprintRecord) -> Option<()>) -> Option<()> {
        let mut decoder = self.sequence.decoder();
        for _ in 0..self.sequence.len() {
            callback(ClassBlueprintRecord::decode(&mut decoder)?)?;
        }
        decoder.is_empty().then_some(())
    }

    fn validate_for_materialization(
        &self,
        source_len: usize,
        shared_function_count: usize,
    ) -> Result<(), ValidationErrorKind> {
        self.for_each(|blueprint| {
            (blueprint.source_range_is_valid(source_len) && blueprint.indices_are_valid(shared_function_count))
                .then_some(())
        })
        .ok_or(ValidationErrorKind::InvalidLength)
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
            name: Option::<DecodedUtf16String>::decode(decoder)?,
            source_text_offset: usize::decode(decoder)?,
            source_text_length: usize::decode(decoder)?,
            constructor_sfd_index: u32::decode(decoder)?,
            has_super_class: bool::decode(decoder)?,
            has_name: bool::decode(decoder)?,
            elements: decoder.sequence_values(ClassElementRecord::decode)?,
        })
    }
}

pub(crate) struct DecodedClassBlueprintRecord {
    name: Option<DecodedUtf16String>,
    source_text_offset: usize,
    source_text_length: usize,
    constructor_sfd_index: u32,
    has_super_class: bool,
    has_name: bool,
    elements: Vec<DecodedClassElementRecord>,
}

impl DecodedClassBlueprintRecord {
    fn source_range_is_valid(&self, source_len: usize) -> bool {
        source_range_is_valid(self.source_text_offset, self.source_text_length, source_len)
    }

    fn indices_are_valid(&self, shared_function_count: usize) -> bool {
        (self.constructor_sfd_index as usize) < shared_function_count
            && self
                .elements
                .iter()
                .all(|element| element.indices_are_valid(shared_function_count))
    }
}

impl From<&DecodedClassBlueprintRecord> for PendingClassBlueprint {
    fn from(record: &DecodedClassBlueprintRecord) -> Self {
        Self {
            name: record.name.as_ref().map(DecodedUtf16String::to_utf16_string),
            source_text_offset: record.source_text_offset,
            source_text_length: record.source_text_length,
            constructor_sfd_index: record.constructor_sfd_index,
            has_super_class: record.has_super_class,
            has_name: record.has_name,
            elements: record.elements.iter().map(PendingClassElement::from).collect(),
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
            private_identifier: Option::<DecodedUtf16String>::decode(decoder)?,
            shared_function_data_index: Option::<u32>::decode(decoder)?,
            has_initializer: bool::decode(decoder)?,
            literal_value_kind: PendingLiteralValueKind::decode(decoder)?,
            literal_value_number: f64::decode(decoder)?,
            literal_value_string: Option::<DecodedUtf16String>::decode(decoder)?,
        })
    }
}

pub(crate) struct DecodedClassElementRecord {
    kind: u8,
    is_static: bool,
    is_private: bool,
    private_identifier: Option<DecodedUtf16String>,
    shared_function_data_index: Option<u32>,
    has_initializer: bool,
    literal_value_kind: PendingLiteralValueKind,
    literal_value_number: f64,
    literal_value_string: Option<DecodedUtf16String>,
}

impl DecodedClassElementRecord {
    fn indices_are_valid(&self, shared_function_count: usize) -> bool {
        let shared_function_data_index_is_valid = || {
            self.shared_function_data_index
                .is_some_and(|index| (index as usize) < shared_function_count)
        };

        match self.kind {
            0 | 1 | 2 | 4 => shared_function_data_index_is_valid(),
            3 => {
                if self.has_initializer && matches!(self.literal_value_kind, PendingLiteralValueKind::None) {
                    shared_function_data_index_is_valid()
                } else {
                    self.shared_function_data_index
                        .is_none_or(|index| (index as usize) < shared_function_count)
                }
            }
            _ => false,
        }
    }
}

impl From<&DecodedClassElementRecord> for PendingClassElement {
    fn from(record: &DecodedClassElementRecord) -> Self {
        Self {
            kind: record.kind,
            is_static: record.is_static,
            is_private: record.is_private,
            private_identifier: record
                .private_identifier
                .as_ref()
                .map(DecodedUtf16String::to_utf16_string),
            shared_function_data_index: record.shared_function_data_index,
            has_initializer: record.has_initializer,
            literal_value_kind: record.literal_value_kind,
            literal_value_number: record.literal_value_number,
            literal_value_string: record
                .literal_value_string
                .as_ref()
                .map(DecodedUtf16String::to_utf16_string),
        }
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
    use std::cell::Cell;

    use super::*;

    /// Blob bytes as an embedder owns them: aligned the way an embedder's blob is, and counting their releases.
    struct TestBlobStorage {
        words: Vec<u64>,
        releases: Rc<Cell<usize>>,
    }

    unsafe extern "C" fn release_test_blob(owner: *mut c_void) {
        let storage = unsafe { Box::from_raw(owner.cast::<TestBlobStorage>()) };
        storage.releases.set(storage.releases.get() + 1);
    }

    /// Copies `bytes` into storage of the returned owner, which adds one to `releases` when it is released.
    fn test_blob(bytes: &[u8], releases: &Rc<Cell<usize>>) -> (&'static [u8], ForeignBytecodeCacheBlobOwner) {
        let mut storage = Box::new(TestBlobStorage {
            words: vec![0u64; bytes.len().div_ceil(size_of::<u64>())],
            releases: releases.clone(),
        });
        // SAFETY: The words have room for every byte.
        unsafe {
            std::ptr::copy_nonoverlapping(bytes.as_ptr(), storage.words.as_mut_ptr().cast::<u8>(), bytes.len());
        }
        // SAFETY: The owner keeps the words alive until the decoded blob releases it.
        let view = unsafe { std::slice::from_raw_parts(storage.words.as_ptr().cast::<u8>(), bytes.len()) };
        let owner = ForeignBytecodeCacheBlobOwner {
            owner: Box::into_raw(storage).cast(),
            free_owner: release_test_blob,
        };
        (view, owner)
    }

    fn decode_test_blob(
        bytes: &[u8],
        expected_program_type: ast::ProgramType,
        expected_source_hash: &[u8; SOURCE_HASH_SIZE],
        releases: &Rc<Cell<usize>>,
    ) -> Option<DecodedCacheBlob> {
        decode_test_blob_for_runtime(
            bytes,
            expected_program_type,
            expected_source_hash,
            BytecodeCacheRuntime::Rust,
            releases,
        )
    }

    fn decode_test_blob_for_runtime(
        bytes: &[u8],
        expected_program_type: ast::ProgramType,
        expected_source_hash: &[u8; SOURCE_HASH_SIZE],
        expected_runtime: BytecodeCacheRuntime,
        releases: &Rc<Cell<usize>>,
    ) -> Option<DecodedCacheBlob> {
        let (view, owner) = test_blob(bytes, releases);
        // SAFETY: The owner keeps the view alive.
        unsafe {
            decode_blob(
                view,
                expected_program_type,
                expected_source_hash,
                expected_runtime,
                owner,
            )
        }
    }

    fn empty_record_sequence(encoder: &mut Encoder) {
        DecodedRecordSequence::encode::<u8>(encoder, &[], |_, _| {});
    }

    fn executable_record_with_shared_function(function_payload: Option<Vec<u8>>) -> Vec<u8> {
        let mut encoder = Encoder::new();

        false.encode(&mut encoder); // Strict.
        0u32.encode(&mut encoder); // Number of registers.
        0u32.encode(&mut encoder); // Number of arguments.
        for _ in 0..7 {
            0u32.encode(&mut encoder); // Cache counters.
        }
        false.encode(&mut encoder); // This value needs environment resolution.
        Option::<u32>::None.encode(&mut encoder); // Length identifier.

        encoder.align_bytes_payload_to(BYTECODE_ALIGNMENT);
        Bytes(&[]).encode(&mut encoder); // Bytecode.
        empty_record_sequence(&mut encoder); // Identifier table.
        empty_record_sequence(&mut encoder); // Property key table.
        empty_record_sequence(&mut encoder); // String table.
        0u32.encode(&mut encoder); // Constant count.
        Bytes(&[]).encode(&mut encoder);
        empty_record_sequence(&mut encoder); // Exception handlers.
        empty_record_sequence(&mut encoder); // Source map.
        empty_record_sequence(&mut encoder); // Local variables.
        empty_record_sequence(&mut encoder); // Argument variable names.

        match function_payload {
            Some(payload) => {
                1u32.encode(&mut encoder); // Shared function count.
                encoder.align_bytes_payload_to(BYTECODE_ALIGNMENT);
                Bytes(&payload).encode(&mut encoder);
            }
            None => {
                DecodedRecordSequence::encode_with_alignment::<u8>(&mut encoder, &[], BYTECODE_ALIGNMENT, |_, _| {});
            }
        }

        empty_record_sequence(&mut encoder); // Class blueprints.
        encoder.finish()
    }

    fn function_payload(source_text_start: u32, source_text_end: u32) -> Vec<u8> {
        let mut encoder = Encoder::new();

        Option::<Utf16<'_>>::None.encode(&mut encoder); // Function name.
        source_text_start.encode(&mut encoder);
        source_text_end.encode(&mut encoder);
        0i32.encode(&mut encoder); // Function length.
        0u32.encode(&mut encoder); // Formal parameter count.
        (ast::FunctionKind::Normal as u8).encode(&mut encoder);
        false.encode(&mut encoder); // Strict mode.
        false.encode(&mut encoder); // Arrow function.
        false.encode(&mut encoder); // Simple parameter list.
        false.encode(&mut encoder); // Uses this.
        false.encode(&mut encoder); // Uses this from environment.
        Option::<(Utf16<'_>, bool)>::None.encode(&mut encoder); // Class field initializer name.
        FunctionSfdMetadata {
            uses_this: false,
            this_value_needs_environment_resolution: false,
            function_environment_needed: false,
            function_environment_bindings_count: 0,
            var_environment_bindings_count: 0,
            might_need_arguments: false,
            contains_eval: false,
        }
        .encode(&mut encoder);

        encoder.align_bytes_payload_to(BYTECODE_ALIGNMENT);
        Bytes(&executable_record_with_shared_function(None)).encode(&mut encoder);

        encoder.finish()
    }

    fn cached_executable(bytes: &[u8]) -> DecodedCachedExecutableRecord {
        let (view, owner) = test_blob(bytes, &Rc::default());
        let mut decoder = Decoder::new(view, Some(owner));
        DecodedCachedExecutableRecord {
            bytes: decoder.bytecode_bytes(view.len()).expect("the record fits in its blob"),
            has_been_validated_for_materialization: false,
        }
    }

    #[test]
    fn sequence_decode_rejects_lengths_larger_than_remaining_bytes() {
        let bytes = u32::MAX.to_le_bytes();

        let mut decoder = Decoder::new(&bytes, None);
        assert!(decoder.sequence_values(u8::decode).is_none());
    }

    #[test]
    fn sequence_decode_rejects_truncated_items_without_large_allocation() {
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&4u32.to_le_bytes());
        bytes.extend_from_slice(&[1, 2, 3]);

        let mut decoder = Decoder::new(&bytes, None);
        assert!(decoder.sequence_values(u8::decode).is_none());
    }

    #[test]
    fn record_sequence_decode_rejects_impossible_count_without_large_allocation() {
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&4u32.to_le_bytes());
        bytes.extend_from_slice(&1u32.to_le_bytes());
        bytes.push(0);

        let mut decoder = Decoder::new(&bytes, None);
        assert!(DecodedRecordSequence::decode(&mut decoder).is_none());
    }

    #[test]
    fn constant_table_decode_rejects_impossible_count_without_large_allocation() {
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&4u32.to_le_bytes());
        bytes.extend_from_slice(&1u32.to_le_bytes());
        bytes.push(0);

        let mut decoder = Decoder::new(&bytes, None);
        assert!(ConstantTable::decode(&mut decoder).is_none());
    }

    #[test]
    fn decode_rejects_mismatched_source_hash_before_payload() {
        let stored_source_hash = [1u8; SOURCE_HASH_SIZE];
        let expected_source_hash = [2u8; SOURCE_HASH_SIZE];

        let mut bytes = Vec::new();
        bytes.extend_from_slice(MAGIC);
        bytes.extend_from_slice(&FORMAT_VERSION.to_le_bytes());
        bytes.push(BytecodeCacheRuntime::Rust.tag());
        bytes.push(ast::ProgramType::Script as u8);
        bytes.extend_from_slice(&stored_source_hash);

        let releases = Rc::default();
        assert!(decode_test_blob(&bytes, ast::ProgramType::Script, &expected_source_hash, &releases).is_none());
        assert_eq!(releases.get(), 1);
    }

    #[test]
    fn utf16_decode_borrows_from_foreign_blob() {
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&3u32.to_le_bytes());
        bytes.extend_from_slice(&0x41u16.to_le_bytes());
        bytes.extend_from_slice(&0x2262u16.to_le_bytes());
        bytes.extend_from_slice(&0x0391u16.to_le_bytes());

        let (view, owner) = test_blob(&bytes, &Rc::default());
        let mut decoder = Decoder::new(view, Some(owner));
        let decoded = DecodedUtf16String::decode(&mut decoder).unwrap();
        assert!(matches!(decoded, DecodedUtf16String::Foreign { .. }));
        assert_eq!(decoded.to_vec(), vec![0x41, 0x2262, 0x0391]);
    }

    #[test]
    fn cached_function_validation_includes_nested_source_ranges() {
        let executable = cached_executable(&executable_record_with_shared_function(Some(function_payload(20, 21))));

        assert_eq!(
            executable.validate_for_materialization(10),
            Err(ValidationErrorKind::InvalidLength)
        );
        assert_eq!(executable.validate_for_materialization(30), Ok(()));
    }
}
