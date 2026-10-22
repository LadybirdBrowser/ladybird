/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The SIMD instructions. They keep their sub-opcode in the IR (`VectorUnary`, `VectorBinary`, ...), so that one
//! node kind per shape carries all of them and code generation picks each one's instructions.
//!
//! The relaxed instructions get the semantics the interpreter chose for them: all but the multiply-adds and the dot
//! products are their deterministic counterparts, which they become here, so that they share those nodes' value
//! numbers and code.

use super::Builder;
use crate::decode::{Decoder, MemoryArgument};
use crate::ir::{Func, LoadKind, Node, Op, Type, Value};
use libwasm_module::Module;
use libwasm_module::instruction_tables::{SimdKind, simd_kind};
use libwasm_module::opcode::simd::*;

/// The deterministic instruction a relaxed one behaves as, or the instruction itself.
fn canonical(sub_opcode: u32) -> u32 {
    match sub_opcode {
        I8X16_RELAXED_SWIZZLE => I8X16_SWIZZLE,
        I32X4_RELAXED_TRUNC_F32X4_S => I32X4_TRUNC_SAT_F32X4_S,
        I32X4_RELAXED_TRUNC_F32X4_U => I32X4_TRUNC_SAT_F32X4_U,
        I32X4_RELAXED_TRUNC_F64X2_S_ZERO => I32X4_TRUNC_SAT_F64X2_S_ZERO,
        I32X4_RELAXED_TRUNC_F64X2_U_ZERO => I32X4_TRUNC_SAT_F64X2_U_ZERO,
        I8X16_RELAXED_LANESELECT | I16X8_RELAXED_LANESELECT | I32X4_RELAXED_LANESELECT | I64X2_RELAXED_LANESELECT => {
            V128_BITSELECT
        }
        F32X4_RELAXED_MIN => F32X4_MIN,
        F32X4_RELAXED_MAX => F32X4_MAX,
        F64X2_RELAXED_MIN => F64X2_MIN,
        F64X2_RELAXED_MAX => F64X2_MAX,
        I16X8_RELAXED_Q15MULR_S => I16X8_Q15MULR_SAT_S,
        _ => sub_opcode,
    }
}

/// The type of a lane of `lane.extract` or `lane.replace` instruction, as values carry it.
fn lane_type(ty: libwasm_module::types::ValType) -> Type {
    use libwasm_module::types::ValType;
    match ty {
        ValType::I64 => Type::I64,
        ValType::F32 => Type::F32,
        ValType::F64 => Type::F64,
        _ => Type::I32,
    }
}

impl Builder {
    pub(super) fn vector_constant(&mut self, func: &mut Func, bits: u128) -> Value {
        let index = func.constants.len() as u32;
        func.constants.push(bits);
        Self::append(func, Node::new(Op::Const128, Type::V128, 0, [index, 0, 0]))
    }

    /// The base, index and offset of an access of `1 << size_log2` bytes at `index` in `argument`'s memory: memory 0
    /// with guard pages, or else the address after checking it (quickening rule 2).
    fn simd_address(
        &mut self,
        func: &mut Func,
        module: &Module,
        checked: bool,
        argument: MemoryArgument,
        index: Value,
        size_log2: u8,
    ) -> (Value, Value, u32) {
        if !checked {
            debug_assert_eq!(argument.memory, 0, "plain accesses are of memory 0");
            return (self.memory_base, index, argument.offset as u32);
        }
        let address = self.checked_address(func, module, argument, index, size_log2, false);
        let zero = self.constant(func, Type::I32, 0);
        (address, zero, 0)
    }

    fn store(&mut self, func: &mut Func, (base, index, offset): (Value, Value, u32), value: Value, size_log2: u8) {
        let start = func.operands.len() as u32;
        func.operands.extend_from_slice(&[base, index, value]);
        Self::append(func, Node::new(Op::Store, Type::None, size_log2, [start, 3, offset]));
    }

    pub(super) fn simd(
        &mut self,
        func: &mut Func,
        module: &Module,
        sub_opcode: u32,
        checked: bool,
        decoder: &Decoder<'_>,
    ) {
        let sub_opcode = canonical(sub_opcode);
        let kind = simd_kind(sub_opcode).expect("validation rejects unknown SIMD instructions");
        let value = match kind {
            SimdKind::Load { size } => {
                let index = func.resolve(self.pop());
                let argument = decoder.simd_argument();
                let size_log2 = size.trailing_zeros() as u8;
                let (base, index, offset) = self.simd_address(func, module, checked, argument, index, size_log2);
                Self::append(
                    func,
                    Node::new(Op::VectorLoad, Type::V128, sub_opcode as u8, [base, index, offset]),
                )
            }
            SimdKind::Store { .. } => {
                let value = func.resolve(self.pop());
                let index = func.resolve(self.pop());
                let address = self.simd_address(func, module, checked, decoder.simd_argument(), index, 4);
                self.store(func, address, value, 4);
                return;
            }
            SimdKind::LoadLane { size } => {
                // A scalar load, and its value replacing the lane.
                let lane = u32::from(decoder.simd_lane());
                let vector = func.resolve(self.pop());
                let index = func.resolve(self.pop());
                let argument = decoder.simd_argument();
                let size_log2 = size.trailing_zeros() as u8;
                let (base, index, offset) = self.simd_address(func, module, checked, argument, index, size_log2);
                let (kind, ty, replace) = match size {
                    1 => (LoadKind::I32U8, Type::I32, I8X16_REPLACE_LANE),
                    2 => (LoadKind::I32U16, Type::I32, I16X8_REPLACE_LANE),
                    4 => (LoadKind::I32, Type::I32, I32X4_REPLACE_LANE),
                    _ => (LoadKind::I64, Type::I64, I64X2_REPLACE_LANE),
                };
                let scalar = Self::append(func, Node::new(Op::Load, ty, kind as u8, [base, index, offset]));
                self.pure(
                    func,
                    Node::new(Op::ReplaceLane, Type::V128, 0, [vector, scalar, replace | (lane << 16)]),
                )
            }
            SimdKind::StoreLane { size } => {
                // The lane extracted, and stored as a scalar.
                let lane = u32::from(decoder.simd_lane());
                let vector = func.resolve(self.pop());
                let index = func.resolve(self.pop());
                let (ty, extract, size_log2) = match size {
                    1 => (Type::I32, I8X16_EXTRACT_LANE_U, 0),
                    2 => (Type::I32, I16X8_EXTRACT_LANE_U, 1),
                    4 => (Type::I32, I32X4_EXTRACT_LANE, 2),
                    _ => (Type::I64, I64X2_EXTRACT_LANE, 3),
                };
                let scalar = self.pure(func, Node::new(Op::ExtractLane, ty, 0, [vector, extract, lane]));
                let address = self.simd_address(func, module, checked, decoder.simd_argument(), index, size_log2);
                self.store(func, address, scalar, size_log2);
                return;
            }
            SimdKind::Const => {
                let bits = u128::from_le_bytes(decoder.simd_bytes());
                self.vector_constant(func, bits)
            }
            SimdKind::Shuffle => {
                let mut b = func.resolve(self.pop());
                let mut a = func.resolve(self.pop());
                let mut lanes = decoder.simd_bytes();
                // A shuffle of one vector names it twice, with lanes 0 to 15, so that the other one need not live
                // on for it, and code generation sees what it is.
                if lanes.iter().all(|lane| *lane < 16) {
                    b = a;
                } else if lanes.iter().all(|lane| *lane >= 16) {
                    a = b;
                }
                if a == b {
                    lanes = lanes.map(|lane| lane & 15);
                }
                let index = func.constants.len() as u32;
                func.constants.push(u128::from_le_bytes(lanes));
                Self::append(func, Node::new(Op::Shuffle, Type::V128, 0, [a, b, index]))
            }
            SimdKind::Splat(_) | SimdKind::Unary | SimdKind::Test => {
                let result = if matches!(kind, SimdKind::Test) {
                    Type::I32
                } else {
                    Type::V128
                };
                let a = func.resolve(self.pop());
                self.pure(func, Node::new(Op::VectorUnary, result, 0, [a, sub_opcode, 0]))
            }
            SimdKind::Binary | SimdKind::Shift => {
                let b = func.resolve(self.pop());
                let a = func.resolve(self.pop());
                self.pure(func, Node::new(Op::VectorBinary, Type::V128, 0, [a, b, sub_opcode]))
            }
            SimdKind::Ternary => {
                let c = func.resolve(self.pop());
                let b = func.resolve(self.pop());
                let a = func.resolve(self.pop());
                self.pure(
                    func,
                    Node::new(Op::VectorTernary, Type::V128, sub_opcode as u8, [a, b, c]),
                )
            }
            SimdKind::ExtractLane { result, .. } => {
                let a = func.resolve(self.pop());
                let lane = u32::from(decoder.simd_lane());
                self.pure(
                    func,
                    Node::new(Op::ExtractLane, lane_type(result), 0, [a, sub_opcode, lane]),
                )
            }
            SimdKind::ReplaceLane { .. } => {
                let b = func.resolve(self.pop());
                let a = func.resolve(self.pop());
                let lane = u32::from(decoder.simd_lane());
                self.pure(
                    func,
                    Node::new(Op::ReplaceLane, Type::V128, 0, [a, b, sub_opcode | (lane << 16)]),
                )
            }
        };
        self.push(value);
    }
}
