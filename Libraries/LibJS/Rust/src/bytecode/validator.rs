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

use super::instruction::{NUM_OPCODES, instruction_length_from_bytes};

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
    OperandRegisterOutOfRange = 6,
    OperandLocalOutOfRange = 7,
    OperandConstantOutOfRange = 8,
    OperandArgumentOutOfRange = 9,
    OperandInvalid = 10,
    LabelNotAtInstructionBoundary = 11,
    IdentifierIndexOutOfRange = 12,
    StringIndexOutOfRange = 13,
    PropertyKeyIndexOutOfRange = 14,
    RegexIndexOutOfRange = 15,
    PropertyLookupCacheIndexOutOfRange = 16,
    GlobalVariableCacheIndexOutOfRange = 17,
    TemplateObjectCacheIndexOutOfRange = 18,
    ObjectShapeCacheIndexOutOfRange = 19,
    ObjectPropertyIteratorCacheIndexOutOfRange = 20,
    SharedFunctionDataIndexOutOfRange = 21,
    ClassBlueprintIndexOutOfRange = 22,
    EnumOutOfRange = 23,
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

    // Pass 2 lands in a follow-up commit. Suppress unused warnings for now.
    let _ = bounds;
    let _ = valid_offsets;

    Ok(())
}
