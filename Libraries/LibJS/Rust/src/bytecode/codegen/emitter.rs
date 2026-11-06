/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the emission core writes instructions with, and what it leaves when it is done.

use crate::bytecode::executable_data::ExceptionHandler;
use crate::bytecode::operand::Operand;
use crate::bytecode::operand::OperandType;
use crate::bytecode::source_map::SourceOffsetEntry;
use crate::layout::buffer::BlockSlice;
use crate::u32_from_usize;

/// The result of assembling bytecode.
pub struct AssembledBytecode {
    pub bytecode: BlockSlice<u8>,
    pub source_map: BlockSlice<SourceOffsetEntry>,
    pub exception_handlers: BlockSlice<ExceptionHandler>,
    pub number_of_registers: u32,
    /// One past the highest `Operand::argument` index referenced by any
    /// instruction, or 0 if the bytecode never reads an argument.
    pub number_of_arguments: u32,
}

/// A place in the bytes of a block.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub(super) struct BlockPosition {
    pub(super) block: u32,
    pub(super) offset: u32,
}

/// What the generated encoder writes an instruction with: the bytes of its block, and the notes for `finish()`.
pub struct BlockEncoder<'a> {
    pub bytes: &'a mut Vec<u8>,
    block: u32,
    operand_positions: &'a mut Vec<BlockPosition>,
    label_positions: &'a mut Vec<BlockPosition>,
    /// One past the highest argument that an instruction refers to so far.
    number_of_arguments: &'a mut u32,
    /// The register to note the position of the first time it comes up.
    watched: Operand,
    pub(super) watched_position: u32,
}

impl<'a> BlockEncoder<'a> {
    #[inline(always)]
    pub(super) fn for_stream(
        bytes: &'a mut Vec<u8>,
        operand_positions: &'a mut Vec<BlockPosition>,
        label_positions: &'a mut Vec<BlockPosition>,
        number_of_arguments: &'a mut u32,
        watched: Operand,
    ) -> Self {
        Self {
            bytes,
            block: 0,
            operand_positions,
            label_positions,
            number_of_arguments,
            watched,
            watched_position: u32::MAX,
        }
    }
}

impl BlockEncoder<'_> {
    /// An operand of the instruction about to be written, `offset` bytes into it. Only registers have their place in
    /// the frame at this point: the others come after the registers, whose number is not known yet.
    #[inline(always)]
    pub fn operand(&mut self, operand: Operand, offset: usize) -> u32 {
        match operand.operand_type() {
            OperandType::Register => {
                if operand == self.watched {
                    self.watched = Operand::invalid();
                    self.watched_position = u32_from_usize(self.bytes.len() + offset);
                }
            }
            OperandType::Argument => {
                *self.number_of_arguments = (*self.number_of_arguments).max(operand.index() + 1);
                self.note_operand(offset);
            }
            OperandType::Local | OperandType::Constant => self.note_operand(offset),
        }
        operand.raw()
    }

    #[inline(always)]
    fn note_operand(&mut self, offset: usize) {
        self.operand_positions.push(BlockPosition {
            block: self.block,
            offset: u32_from_usize(self.bytes.len() + offset),
        });
    }

    /// Notes that the instruction about to be written has a label `offset` bytes in.
    #[inline(always)]
    pub fn note_label(&mut self, offset: usize) {
        self.label_positions.push(BlockPosition {
            block: self.block,
            offset: u32_from_usize(self.bytes.len() + offset),
        });
    }
}

/// Where the parts of a frame start: `[registers | locals | constants | arguments]`.
#[derive(Clone, Copy)]
pub(super) struct FrameLayout {
    pub(super) register_count: u32,
    pub(super) local_count: u32,
    pub(super) constant_count: u32,
}

impl FrameLayout {
    #[inline(always)]
    pub(super) fn start_of(self, operand_type: OperandType) -> u32 {
        match operand_type {
            OperandType::Register => 0,
            OperandType::Local => self.register_count,
            OperandType::Constant => self.register_count + self.local_count,
            OperandType::Argument => self.register_count + self.local_count + self.constant_count,
        }
    }
}
