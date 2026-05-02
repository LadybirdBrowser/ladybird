/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Bytecode validator.
//!
//! Walks a packed bytecode buffer and verifies its structural integrity
//! against a set of bounds (table sizes, register/local/constant/argument
//! counts, cache-array sizes). Intended for two purposes:
//!   1. A debug/sanitizer-build sanity check that runs after every successful
//!      bytecode compilation.
//!   2. A safety net for loading bytecode from on-disk caches.
//!
//! Operates on the wire-format bytes only, without round-tripping through
//! the Rust `Instruction` enum, so it works on freshly-encoded as well as
//! freshly-deserialized bytecode.

use super::instruction::{NUM_OPCODES, instruction_length_from_bytes, validate_instruction};

/// Sentinel u32 used by `Operand::INVALID` and by `Optional<*TableIndex>` for
/// "no value". Mirrors the C++ `0xFFFFFFFF` constant used throughout
/// `Bytecode/Operand.h` and the per-table index types.
const INVALID_INDEX_U32: u32 = 0xFFFF_FFFF;

/// Sentinel u64 written into cache fields by the bytecode encoder when no
/// cache slot was reserved. The fixup pass replaces real indices with
/// pointers, leaving the sentinel as `0`.
const NO_CACHE_INDEX: u64 = u32::MAX as u64;

/// Bounds against which bytecode references are checked.
#[repr(C)]
pub struct FFIValidatorBounds {
    pub number_of_registers: u32,
    pub number_of_locals: u32,
    pub number_of_constants: u32,
    pub number_of_arguments: u32,
    pub identifier_table_size: u32,
    pub string_table_size: u32,
    pub property_key_table_size: u32,
    pub regex_table_size: u32,
    pub property_lookup_cache_count: u32,
    pub global_variable_cache_count: u32,
    pub template_object_cache_count: u32,
    pub object_shape_cache_count: u32,
    pub object_property_iterator_cache_count: u32,
    pub class_blueprint_count: u32,
    pub shared_function_data_count: u32,
    /// If true, m_cache fields hold indices that should be range-checked
    /// against the corresponding cache_count. If false, the cache fixup
    /// pass has already replaced them with real pointers and they are
    /// skipped during validation.
    pub before_cache_fixup: bool,
}

/// Categorization of validation failures, mirrored to C++ as an enum class.
#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ValidationErrorKind {
    Ok = 0,
    BufferNotAligned = 1,
    InstructionMisaligned = 2,
    UnknownOpcode = 3,
    TruncatedInstruction = 4,
    InvalidLength = 5,
    OperandOutOfRange = 6,
    OperandInvalid = 7,
    LabelNotAtInstructionBoundary = 8,
    IdentifierIndexOutOfRange = 9,
    StringIndexOutOfRange = 10,
    PropertyKeyIndexOutOfRange = 11,
    RegexIndexOutOfRange = 12,
    PropertyLookupCacheIndexOutOfRange = 13,
    GlobalVariableCacheIndexOutOfRange = 14,
    TemplateObjectCacheIndexOutOfRange = 15,
    ObjectShapeCacheIndexOutOfRange = 16,
    ObjectPropertyIteratorCacheIndexOutOfRange = 17,
    SharedFunctionDataIndexOutOfRange = 18,
    ClassBlueprintIndexOutOfRange = 19,
    EnumOutOfRange = 20,
}

/// Detail returned to the C++ caller on validation failure.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct FFIValidationError {
    pub kind: ValidationErrorKind,
    pub offset: u32,
    pub opcode: u32,
}

impl FFIValidationError {
    pub const fn new(kind: ValidationErrorKind, offset: usize, opcode: u8) -> Self {
        Self {
            kind,
            offset: offset as u32,
            opcode: opcode as u32,
        }
    }
}

/// Borrowed state passed to the generated per-instruction validator.
pub struct ValidationContext<'a> {
    pub bounds: &'a FFIValidatorBounds,
    pub bytes: &'a [u8],
    /// Sorted byte offsets of valid instruction starts, populated by Pass 1.
    pub valid_offsets: &'a [u32],
}

#[inline]
pub fn read_u32(bytes: &[u8], at: usize) -> u32 {
    u32::from_ne_bytes(bytes[at..at + 4].try_into().unwrap())
}

#[inline]
pub fn read_u64(bytes: &[u8], at: usize) -> u64 {
    u64::from_ne_bytes(bytes[at..at + 8].try_into().unwrap())
}

#[inline]
pub fn validate_operand(raw: u32, ctx: &ValidationContext) -> Result<(), ValidationErrorKind> {
    // After the assembler runs, operands in the encoded instruction stream
    // are flat indices into the runtime [registers | locals | constants |
    // arguments] array; the original 3-bit type tag has been zeroed out by
    // Operand::offset_index_by. The runtime indexes the combined array
    // directly with raw, so the validator just needs to keep raw inside that
    // array's bounds.
    if raw == INVALID_INDEX_U32 {
        return Err(ValidationErrorKind::OperandInvalid);
    }
    let max = ctx
        .bounds
        .number_of_registers
        .saturating_add(ctx.bounds.number_of_locals)
        .saturating_add(ctx.bounds.number_of_constants)
        .saturating_add(ctx.bounds.number_of_arguments);
    if raw >= max {
        return Err(ValidationErrorKind::OperandOutOfRange);
    }
    Ok(())
}

#[inline]
pub fn validate_optional_operand(raw: u32, ctx: &ValidationContext) -> Result<(), ValidationErrorKind> {
    if raw == INVALID_INDEX_U32 {
        return Ok(());
    }
    validate_operand(raw, ctx)
}

#[inline]
pub fn validate_label(addr: u32, ctx: &ValidationContext) -> Result<(), ValidationErrorKind> {
    if ctx.valid_offsets.binary_search(&addr).is_err() {
        return Err(ValidationErrorKind::LabelNotAtInstructionBoundary);
    }
    Ok(())
}

#[inline]
pub fn validate_identifier_index(raw: u32, ctx: &ValidationContext) -> Result<(), ValidationErrorKind> {
    if raw == INVALID_INDEX_U32 || raw >= ctx.bounds.identifier_table_size {
        return Err(ValidationErrorKind::IdentifierIndexOutOfRange);
    }
    Ok(())
}

#[inline]
pub fn validate_optional_identifier_index(raw: u32, ctx: &ValidationContext) -> Result<(), ValidationErrorKind> {
    if raw == INVALID_INDEX_U32 {
        return Ok(());
    }
    if raw >= ctx.bounds.identifier_table_size {
        return Err(ValidationErrorKind::IdentifierIndexOutOfRange);
    }
    Ok(())
}

#[inline]
pub fn validate_string_index(raw: u32, ctx: &ValidationContext) -> Result<(), ValidationErrorKind> {
    if raw == INVALID_INDEX_U32 || raw >= ctx.bounds.string_table_size {
        return Err(ValidationErrorKind::StringIndexOutOfRange);
    }
    Ok(())
}

#[inline]
pub fn validate_optional_string_index(raw: u32, ctx: &ValidationContext) -> Result<(), ValidationErrorKind> {
    if raw == INVALID_INDEX_U32 {
        return Ok(());
    }
    if raw >= ctx.bounds.string_table_size {
        return Err(ValidationErrorKind::StringIndexOutOfRange);
    }
    Ok(())
}

#[inline]
pub fn validate_property_key_index(raw: u32, ctx: &ValidationContext) -> Result<(), ValidationErrorKind> {
    if raw == INVALID_INDEX_U32 || raw >= ctx.bounds.property_key_table_size {
        return Err(ValidationErrorKind::PropertyKeyIndexOutOfRange);
    }
    Ok(())
}

#[inline]
pub fn validate_property_lookup_cache_index(raw: u64, ctx: &ValidationContext) -> Result<(), ValidationErrorKind> {
    if !ctx.bounds.before_cache_fixup || raw == NO_CACHE_INDEX {
        return Ok(());
    }
    if raw >= ctx.bounds.property_lookup_cache_count as u64 {
        return Err(ValidationErrorKind::PropertyLookupCacheIndexOutOfRange);
    }
    Ok(())
}

#[inline]
pub fn validate_global_variable_cache_index(raw: u64, ctx: &ValidationContext) -> Result<(), ValidationErrorKind> {
    if !ctx.bounds.before_cache_fixup || raw == NO_CACHE_INDEX {
        return Ok(());
    }
    if raw >= ctx.bounds.global_variable_cache_count as u64 {
        return Err(ValidationErrorKind::GlobalVariableCacheIndexOutOfRange);
    }
    Ok(())
}

#[inline]
pub fn validate_template_object_cache_index(raw: u64, ctx: &ValidationContext) -> Result<(), ValidationErrorKind> {
    if !ctx.bounds.before_cache_fixup || raw == NO_CACHE_INDEX {
        return Ok(());
    }
    if raw >= ctx.bounds.template_object_cache_count as u64 {
        return Err(ValidationErrorKind::TemplateObjectCacheIndexOutOfRange);
    }
    Ok(())
}

#[inline]
pub fn validate_object_shape_cache_index(raw: u64, ctx: &ValidationContext) -> Result<(), ValidationErrorKind> {
    if !ctx.bounds.before_cache_fixup || raw == NO_CACHE_INDEX {
        return Ok(());
    }
    if raw >= ctx.bounds.object_shape_cache_count as u64 {
        return Err(ValidationErrorKind::ObjectShapeCacheIndexOutOfRange);
    }
    Ok(())
}

#[inline]
pub fn validate_object_property_iterator_cache_index(
    raw: u64,
    ctx: &ValidationContext,
) -> Result<(), ValidationErrorKind> {
    if !ctx.bounds.before_cache_fixup || raw == NO_CACHE_INDEX {
        return Ok(());
    }
    if raw >= ctx.bounds.object_property_iterator_cache_count as u64 {
        return Err(ValidationErrorKind::ObjectPropertyIteratorCacheIndexOutOfRange);
    }
    Ok(())
}

#[inline]
pub fn validate_shared_function_data_index(raw: u32, ctx: &ValidationContext) -> Result<(), ValidationErrorKind> {
    if raw >= ctx.bounds.shared_function_data_count {
        return Err(ValidationErrorKind::SharedFunctionDataIndexOutOfRange);
    }
    Ok(())
}

#[inline]
pub fn validate_class_blueprint_index(raw: u32, ctx: &ValidationContext) -> Result<(), ValidationErrorKind> {
    if raw >= ctx.bounds.class_blueprint_count {
        return Err(ValidationErrorKind::ClassBlueprintIndexOutOfRange);
    }
    Ok(())
}

/// Walk `bytes` and verify the structural integrity of every instruction.
///
/// Pass 1 verifies that the buffer is a tight sequence of well-formed
/// instructions: each opcode is in range, each instruction is properly
/// aligned, the reported length is sane, and the buffer ends exactly on
/// an instruction boundary. The set of valid instruction-start offsets
/// is collected for use in Pass 2.
///
/// Pass 2 (currently a stub) will dispatch per-opcode field validation.
pub fn validate_bytecode(bytes: &[u8], bounds: &FFIValidatorBounds) -> Result<(), FFIValidationError> {
    let mut valid_offsets: Vec<u32> = Vec::with_capacity(bytes.len() / 16);

    let mut at: usize = 0;
    while at < bytes.len() {
        if !at.is_multiple_of(8) {
            return Err(FFIValidationError::new(
                ValidationErrorKind::InstructionMisaligned,
                at,
                0,
            ));
        }

        let opcode = bytes[at];
        if (opcode as u32) >= NUM_OPCODES {
            return Err(FFIValidationError::new(ValidationErrorKind::UnknownOpcode, at, opcode));
        }

        let size = instruction_length_from_bytes(opcode, bytes, at)
            .map_err(|kind| FFIValidationError::new(kind, at, opcode))?;

        if size == 0 || !size.is_multiple_of(8) {
            return Err(FFIValidationError::new(ValidationErrorKind::InvalidLength, at, opcode));
        }

        let end = at
            .checked_add(size)
            .ok_or_else(|| FFIValidationError::new(ValidationErrorKind::InvalidLength, at, opcode))?;
        if end > bytes.len() {
            return Err(FFIValidationError::new(
                ValidationErrorKind::TruncatedInstruction,
                at,
                opcode,
            ));
        }

        valid_offsets.push(at as u32);
        at = end;
    }

    let ctx = ValidationContext {
        bounds,
        bytes,
        valid_offsets: &valid_offsets,
    };
    for &off_u32 in &valid_offsets {
        let off = off_u32 as usize;
        let opcode = bytes[off];
        validate_instruction(opcode, &ctx, off).map_err(|kind| FFIValidationError::new(kind, off, opcode))?;
    }

    Ok(())
}
