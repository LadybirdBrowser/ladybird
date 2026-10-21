/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The SIMD instructions. They keep their sub-opcode in the IR (`VectorUnary`, `VectorBinary`, ...), so that one
//! node kind per shape carries all of them and code generation picks each one's instructions.

use super::{Builder, unsupported};
use crate::CompileFailure;
use crate::decode::Decoder;
use crate::ir::{Func, Node, Op, Type, Value};
use libwasm_module::opcode::simd::*;

/// What the builder knows about a SIMD instruction it compiles.
enum Shape {
    /// A load of 16 bytes or fewer into a vector.
    Load,
    Store,
    Const,
    Shuffle,
    /// A unary operation, and its result's type.
    Unary(Type),
    /// A binary operation of vectors, or of a vector and an i32 shift count.
    Binary,
    Ternary,
    /// An extraction of a lane, of the lane's type.
    ExtractLane(Type),
    ReplaceLane,
}

/// The shape of a SIMD instruction the JIT compiles, or None.
fn shape(sub_opcode: u32) -> Option<Shape> {
    use Type::{F32, F64, I32, I64, V128};
    Some(match sub_opcode {
        V128_LOAD..=V128_LOAD64_SPLAT | V128_LOAD32_ZERO | V128_LOAD64_ZERO => Shape::Load,
        V128_STORE => Shape::Store,
        V128_CONST => Shape::Const,
        I8X16_SHUFFLE => Shape::Shuffle,
        I8X16_SPLAT | I16X8_SPLAT | I32X4_SPLAT => Shape::Unary(V128),
        I64X2_SPLAT => Shape::Unary(V128),
        F32X4_SPLAT => Shape::Unary(V128),
        F64X2_SPLAT => Shape::Unary(V128),
        I8X16_EXTRACT_LANE_S | I8X16_EXTRACT_LANE_U | I16X8_EXTRACT_LANE_S | I16X8_EXTRACT_LANE_U
        | I32X4_EXTRACT_LANE => Shape::ExtractLane(I32),
        I64X2_EXTRACT_LANE => Shape::ExtractLane(I64),
        F32X4_EXTRACT_LANE => Shape::ExtractLane(F32),
        F64X2_EXTRACT_LANE => Shape::ExtractLane(F64),
        I8X16_REPLACE_LANE | I16X8_REPLACE_LANE | I32X4_REPLACE_LANE => Shape::ReplaceLane,
        I64X2_REPLACE_LANE => Shape::ReplaceLane,
        F32X4_REPLACE_LANE => Shape::ReplaceLane,
        F64X2_REPLACE_LANE => Shape::ReplaceLane,
        V128_NOT
        | I8X16_ABS
        | I16X8_ABS
        | I32X4_ABS
        | I8X16_NEG
        | I16X8_NEG
        | I32X4_NEG
        | I64X2_NEG
        | F32X4_ABS
        | F32X4_NEG
        | F32X4_SQRT
        | F64X2_ABS
        | F64X2_NEG
        | F64X2_SQRT
        | F32X4_CONVERT_I32X4_S => Shape::Unary(V128),
        V128_ANY_TRUE | I8X16_ALL_TRUE | I16X8_ALL_TRUE | I32X4_ALL_TRUE | I64X2_ALL_TRUE | I8X16_BITMASK
        | I32X4_BITMASK | I64X2_BITMASK => Shape::Unary(I32),
        V128_AND | V128_ANDNOT | V128_OR | V128_XOR | I8X16_SWIZZLE | I8X16_EQ | I8X16_NE | I16X8_EQ | I16X8_NE
        | I32X4_EQ | I32X4_NE | I64X2_EQ | I64X2_NE | I8X16_GT_S | I16X8_GT_S | I32X4_GT_S | I64X2_GT_S
        | I8X16_LT_S | I16X8_LT_S | I32X4_LT_S | I64X2_LT_S | F32X4_EQ | F32X4_NE | F32X4_LT | F32X4_GT | F32X4_LE
        | F32X4_GE | F64X2_EQ | F64X2_NE | F64X2_LT | F64X2_GT | F64X2_LE | F64X2_GE | I8X16_ADD | I8X16_SUB
        | I8X16_ADD_SAT_S | I8X16_ADD_SAT_U | I8X16_SUB_SAT_S | I8X16_SUB_SAT_U | I16X8_ADD | I16X8_SUB | I16X8_MUL
        | I32X4_ADD | I32X4_SUB | I32X4_MUL | I64X2_ADD | I64X2_SUB | I8X16_MIN_S | I8X16_MIN_U | I8X16_MAX_S
        | I8X16_MAX_U | I16X8_MIN_S | I16X8_MIN_U | I16X8_MAX_S | I16X8_MAX_U | I32X4_MIN_S | I32X4_MIN_U
        | I32X4_MAX_S | I32X4_MAX_U | F32X4_ADD | F32X4_SUB | F32X4_MUL | F32X4_DIV | F64X2_ADD | F64X2_SUB
        | F64X2_MUL | F64X2_DIV => Shape::Binary,
        I16X8_SHL | I16X8_SHR_S | I16X8_SHR_U | I32X4_SHL | I32X4_SHR_S | I32X4_SHR_U | I64X2_SHL | I64X2_SHR_U => {
            Shape::Binary
        }
        V128_BITSELECT => Shape::Ternary,
        _ => return None,
    })
}

impl Builder {
    pub(super) fn vector_constant(&mut self, func: &mut Func, bits: u128) -> Value {
        let index = func.constants.len() as u32;
        func.constants.push(bits);
        Self::append(func, Node::new(Op::Const128, Type::V128, 0, [index, 0, 0]))
    }

    pub(super) fn simd(
        &mut self,
        func: &mut Func,
        sub_opcode: u32,
        decoder: &Decoder<'_>,
        pc: usize,
    ) -> Result<(), CompileFailure> {
        let Some(shape) = shape(sub_opcode) else {
            return unsupported(pc, "a SIMD instruction");
        };
        let value = match shape {
            Shape::Load => {
                let argument = decoder.simd_argument();
                debug_assert_eq!(argument.memory, 0, "plain accesses are of memory 0");
                let index = Self::resolve(func, self.pop());
                Self::append(
                    func,
                    Node::new(
                        Op::VectorLoad,
                        Type::V128,
                        sub_opcode as u8,
                        [self.memory_base, index, argument.offset as u32],
                    ),
                )
            }
            Shape::Store => {
                let argument = decoder.simd_argument();
                let value = Self::resolve(func, self.pop());
                let index = Self::resolve(func, self.pop());
                let start = func.operands.len() as u32;
                func.operands.extend_from_slice(&[self.memory_base, index, value]);
                Self::append(
                    func,
                    Node::new(Op::Store, Type::None, 4, [start, 3, argument.offset as u32]),
                );
                return Ok(());
            }
            Shape::Const => {
                let bits = u128::from_le_bytes(decoder.simd_bytes());
                self.vector_constant(func, bits)
            }
            Shape::Shuffle => {
                let b = Self::resolve(func, self.pop());
                let a = Self::resolve(func, self.pop());
                let index = func.constants.len() as u32;
                func.constants.push(u128::from_le_bytes(decoder.simd_bytes()));
                Self::append(func, Node::new(Op::Shuffle, Type::V128, 0, [a, b, index]))
            }
            Shape::Unary(result) => {
                let a = Self::resolve(func, self.pop());
                self.pure(func, Node::new(Op::VectorUnary, result, 0, [a, sub_opcode, 0]))
            }
            Shape::Binary => {
                let b = Self::resolve(func, self.pop());
                let a = Self::resolve(func, self.pop());
                self.pure(func, Node::new(Op::VectorBinary, Type::V128, 0, [a, b, sub_opcode]))
            }
            Shape::Ternary => {
                let c = Self::resolve(func, self.pop());
                let b = Self::resolve(func, self.pop());
                let a = Self::resolve(func, self.pop());
                self.pure(
                    func,
                    Node::new(Op::VectorTernary, Type::V128, sub_opcode as u8, [a, b, c]),
                )
            }
            Shape::ExtractLane(ty) => {
                let a = Self::resolve(func, self.pop());
                let lane = u32::from(decoder.simd_lane());
                self.pure(func, Node::new(Op::ExtractLane, ty, 0, [a, sub_opcode, lane]))
            }
            Shape::ReplaceLane => {
                let b = Self::resolve(func, self.pop());
                let a = Self::resolve(func, self.pop());
                let lane = u32::from(decoder.simd_lane());
                self.pure(
                    func,
                    Node::new(Op::ReplaceLane, Type::V128, 0, [a, b, sub_opcode | (lane << 16)]),
                )
            }
        };
        self.push(value);
        Ok(())
    }
}
