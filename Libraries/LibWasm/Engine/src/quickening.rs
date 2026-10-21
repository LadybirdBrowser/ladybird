/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Quickening: the validator rewrites some instructions in the engine's own copy of a module's code into forms that
//! say what validation found out about them, so that the interpreter's handlers for the common forms need no checks.
//! These rules are a contract between the validator and everything that executes code: the interpreter and the JIT.
//!
//! A rewrite never changes the length of an instruction or moves any other one, so branch targets and side-table
//! entries stay valid. Quickened forms use bytes WebAssembly leaves unused:
//!
//! - The prefix 0xFF, followed by a one-byte sub-opcode, for instructions with an immediate byte to spare.
//! - Single-byte opcodes from 0xF4 to 0xFA, for instructions with none (0xF0 to 0xF3 are the runtime's entry and exit
//!   pseudo-instructions).
//!
//! The rules:
//!
//! 1. A core load or store (opcodes 0x28 to 0x3E) that cannot use the guard pages of memory 0, because it accesses
//!    another memory or memory 0 is a memory64, becomes an explicitly bounds-checked access:
//!
//!    `op align [memory] offset` becomes `0xFF sub [memory] offset 0x01*`, where sub is the original opcode, plus 0x40
//!    if a memory index follows. The memory index and offset keep their original encodings. The alignment, which only
//!    matters to validation, is dropped; if it took more than one byte, `nop` instructions (0x01) fill the bytes it
//!    leaves over, after the access.
//!
//!    A SIMD load or store (0xFD followed by sub-opcode 0x00 to 0x0B or 0x54 to 0x5D) on such a memory works the same
//!    way, with its own sub-opcode: 0x80 plus the position of its SIMD sub-opcode in `SIMD_ACCESSES`, plus 0x20 if a
//!    memory index follows. A lane index, if the instruction has one, comes after the offset, and `nop` instructions
//!    fill what the SIMD sub-opcode and alignment leave over.
//!
//! 2. An instruction whose operand or local type is v128, where the plain form assumes a single 8-byte slot, becomes
//!    its v128 form with the same immediates (see `V128_FORMS`).
//!
//! Executing code may assume these rules have been applied: plain loads and stores access memory 0, which is a
//! memory32 with guard pages, and the plain forms of the instructions in rule 2 never see v128 values.

pub const QUICK_PREFIX: u8 = 0xff;

/// The sub-opcode bit that says a memory index follows.
pub const CHECKED_ACCESS_HAS_MEMORY_INDEX: u8 = 0x40;

/// The first sub-opcode of checked SIMD accesses, and the bit that says a memory index follows.
pub const CHECKED_SIMD_ACCESS: u8 = 0x80;
pub const CHECKED_SIMD_ACCESS_HAS_MEMORY_INDEX: u8 = 0x20;

/// The SIMD sub-opcodes of the loads and stores, in the order of their checked sub-opcodes.
pub const SIMD_ACCESSES: [u32; 22] = [
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a,
    0x5b, 0x5c, 0x5d,
];

pub const LOCAL_GET_V128: u8 = 0xf4;
pub const LOCAL_SET_V128: u8 = 0xf5;
pub const LOCAL_TEE_V128: u8 = 0xf6;
pub const GLOBAL_GET_V128: u8 = 0xf7;
pub const GLOBAL_SET_V128: u8 = 0xf8;
pub const DROP_V128: u8 = 0xf9;
pub const SELECT_V128: u8 = 0xfa;

/// Each plain opcode with a v128 form, and that form.
pub const V128_FORMS: [(u8, u8); 7] = [
    (crate::opcode::LOCAL_GET, LOCAL_GET_V128),
    (crate::opcode::LOCAL_SET, LOCAL_SET_V128),
    (crate::opcode::LOCAL_TEE, LOCAL_TEE_V128),
    (crate::opcode::GLOBAL_GET, GLOBAL_GET_V128),
    (crate::opcode::GLOBAL_SET, GLOBAL_SET_V128),
    (crate::opcode::DROP, DROP_V128),
    (crate::opcode::SELECT, SELECT_V128),
];

/// The v128 form of a plain opcode.
pub fn v128_form(opcode: u8) -> Option<u8> {
    V128_FORMS
        .iter()
        .find_map(|&(plain, quickened)| (plain == opcode).then_some(quickened))
}

/// Bytes to write over a module's code, starting at `position`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Rewrite {
    pub position: u32,
    pub bytes: Vec<u8>,
}

/// The rewrite of rule 1, for the access whose opcode is at `position`, with the alignment, memory index (if any),
/// and offset immediates encoded in the given byte ranges of `code`.
pub fn checked_access(
    code: &[u8],
    position: usize,
    alignment: std::ops::Range<usize>,
    memory: Option<std::ops::Range<usize>>,
    offset: std::ops::Range<usize>,
) -> Rewrite {
    let opcode = code[position];
    let mut bytes = vec![
        QUICK_PREFIX,
        opcode
            | if memory.is_some() {
                CHECKED_ACCESS_HAS_MEMORY_INDEX
            } else {
                0
            },
    ];
    if let Some(memory) = memory {
        bytes.extend_from_slice(&code[memory]);
    }
    bytes.extend_from_slice(&code[offset]);
    bytes.extend(std::iter::repeat_n(crate::opcode::NOP, alignment.len() - 1));
    Rewrite {
        position: position as u32,
        bytes,
    }
}

/// The rewrite of rule 1 for the SIMD access whose prefix is at `position`, its sub-opcode `sub_opcode`, its
/// alignment, memory index, and offset immediates in the given ranges of `code`, and a lane index at `lane` if it has
/// one. `end` is where the instruction ends.
pub fn checked_simd_access(
    code: &[u8],
    position: usize,
    sub_opcode: u32,
    memory: Option<std::ops::Range<usize>>,
    offset: std::ops::Range<usize>,
    end: usize,
) -> Rewrite {
    let index = SIMD_ACCESSES
        .iter()
        .position(|&access| access == sub_opcode)
        .expect("only SIMD accesses are rewritten") as u8;
    let mut bytes = vec![
        QUICK_PREFIX,
        CHECKED_SIMD_ACCESS
            + index
            + if memory.is_some() {
                CHECKED_SIMD_ACCESS_HAS_MEMORY_INDEX
            } else {
                0
            },
    ];
    if let Some(memory) = memory {
        bytes.extend_from_slice(&code[memory]);
    }
    bytes.extend_from_slice(&code[offset.start..end]);
    let length = end - position;
    bytes.resize(length, crate::opcode::NOP);
    Rewrite {
        position: position as u32,
        bytes,
    }
}

/// Apply rewrites to a module's code.
pub fn apply(code: &mut [u8], rewrites: &[Rewrite]) {
    for rewrite in rewrites {
        let start = rewrite.position as usize;
        code[start..start + rewrite.bytes.len()].copy_from_slice(&rewrite.bytes);
    }
}
