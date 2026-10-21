/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! How the vector and atomic instructions are typed, by sub-opcode.

use crate::types::ValType;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SimdKind {
    /// Loads `size` bytes from memory into a vector.
    Load {
        size: u32,
    },
    Store {
        size: u32,
    },
    /// Loads `size` bytes into one lane of a vector operand.
    LoadLane {
        size: u32,
    },
    StoreLane {
        size: u32,
    },
    Const,
    Shuffle,
    ExtractLane {
        lanes: u8,
        result: ValType,
    },
    ReplaceLane {
        lanes: u8,
        operand: ValType,
    },
    Splat(ValType),
    /// A vector and an i32 shift count.
    Shift,
    /// A vector reduced to an i32.
    Test,
    Unary,
    Binary,
    Ternary,
}

pub fn simd_kind(sub_opcode: u32) -> Option<SimdKind> {
    Some(match sub_opcode {
        0x0 => SimdKind::Load { size: 16 },    // v128_load
        0x1 => SimdKind::Load { size: 8 },     // v128_load8x8_s
        0x2 => SimdKind::Load { size: 8 },     // v128_load8x8_u
        0x3 => SimdKind::Load { size: 8 },     // v128_load16x4_s
        0x4 => SimdKind::Load { size: 8 },     // v128_load16x4_u
        0x5 => SimdKind::Load { size: 8 },     // v128_load32x2_s
        0x6 => SimdKind::Load { size: 8 },     // v128_load32x2_u
        0x7 => SimdKind::Load { size: 1 },     // v128_load8_splat
        0x8 => SimdKind::Load { size: 2 },     // v128_load16_splat
        0x9 => SimdKind::Load { size: 4 },     // v128_load32_splat
        0xa => SimdKind::Load { size: 8 },     // v128_load64_splat
        0xb => SimdKind::Store { size: 16 },   // v128_store
        0xc => SimdKind::Const,                // v128_const
        0xd => SimdKind::Shuffle,              // i8x16_shuffle
        0xe => SimdKind::Binary,               // i8x16_swizzle
        0xf => SimdKind::Splat(ValType::I32),  // i8x16_splat
        0x10 => SimdKind::Splat(ValType::I32), // i16x8_splat
        0x11 => SimdKind::Splat(ValType::I32), // i32x4_splat
        0x12 => SimdKind::Splat(ValType::I64), // i64x2_splat
        0x13 => SimdKind::Splat(ValType::F32), // f32x4_splat
        0x14 => SimdKind::Splat(ValType::F64), // f64x2_splat
        0x15 => SimdKind::ExtractLane {
            lanes: 16,
            result: ValType::I32,
        }, // i8x16_extract_lane_s
        0x16 => SimdKind::ExtractLane {
            lanes: 16,
            result: ValType::I32,
        }, // i8x16_extract_lane_u
        0x17 => SimdKind::ReplaceLane {
            lanes: 16,
            operand: ValType::I32,
        }, // i8x16_replace_lane
        0x18 => SimdKind::ExtractLane {
            lanes: 8,
            result: ValType::I32,
        }, // i16x8_extract_lane_s
        0x19 => SimdKind::ExtractLane {
            lanes: 8,
            result: ValType::I32,
        }, // i16x8_extract_lane_u
        0x1a => SimdKind::ReplaceLane {
            lanes: 8,
            operand: ValType::I32,
        }, // i16x8_replace_lane
        0x1b => SimdKind::ExtractLane {
            lanes: 4,
            result: ValType::I32,
        }, // i32x4_extract_lane
        0x1c => SimdKind::ReplaceLane {
            lanes: 4,
            operand: ValType::I32,
        }, // i32x4_replace_lane
        0x1d => SimdKind::ExtractLane {
            lanes: 2,
            result: ValType::I64,
        }, // i64x2_extract_lane
        0x1e => SimdKind::ReplaceLane {
            lanes: 2,
            operand: ValType::I64,
        }, // i64x2_replace_lane
        0x1f => SimdKind::ExtractLane {
            lanes: 4,
            result: ValType::F32,
        }, // f32x4_extract_lane
        0x20 => SimdKind::ReplaceLane {
            lanes: 4,
            operand: ValType::F32,
        }, // f32x4_replace_lane
        0x21 => SimdKind::ExtractLane {
            lanes: 2,
            result: ValType::F64,
        }, // f64x2_extract_lane
        0x22 => SimdKind::ReplaceLane {
            lanes: 2,
            operand: ValType::F64,
        }, // f64x2_replace_lane
        0x23 => SimdKind::Binary,              // i8x16_eq
        0x24 => SimdKind::Binary,              // i8x16_ne
        0x25 => SimdKind::Binary,              // i8x16_lt_s
        0x26 => SimdKind::Binary,              // i8x16_lt_u
        0x27 => SimdKind::Binary,              // i8x16_gt_s
        0x28 => SimdKind::Binary,              // i8x16_gt_u
        0x29 => SimdKind::Binary,              // i8x16_le_s
        0x2a => SimdKind::Binary,              // i8x16_le_u
        0x2b => SimdKind::Binary,              // i8x16_ge_s
        0x2c => SimdKind::Binary,              // i8x16_ge_u
        0x2d => SimdKind::Binary,              // i16x8_eq
        0x2e => SimdKind::Binary,              // i16x8_ne
        0x2f => SimdKind::Binary,              // i16x8_lt_s
        0x30 => SimdKind::Binary,              // i16x8_lt_u
        0x31 => SimdKind::Binary,              // i16x8_gt_s
        0x32 => SimdKind::Binary,              // i16x8_gt_u
        0x33 => SimdKind::Binary,              // i16x8_le_s
        0x34 => SimdKind::Binary,              // i16x8_le_u
        0x35 => SimdKind::Binary,              // i16x8_ge_s
        0x36 => SimdKind::Binary,              // i16x8_ge_u
        0x37 => SimdKind::Binary,              // i32x4_eq
        0x38 => SimdKind::Binary,              // i32x4_ne
        0x39 => SimdKind::Binary,              // i32x4_lt_s
        0x3a => SimdKind::Binary,              // i32x4_lt_u
        0x3b => SimdKind::Binary,              // i32x4_gt_s
        0x3c => SimdKind::Binary,              // i32x4_gt_u
        0x3d => SimdKind::Binary,              // i32x4_le_s
        0x3e => SimdKind::Binary,              // i32x4_le_u
        0x3f => SimdKind::Binary,              // i32x4_ge_s
        0x40 => SimdKind::Binary,              // i32x4_ge_u
        0x41 => SimdKind::Binary,              // f32x4_eq
        0x42 => SimdKind::Binary,              // f32x4_ne
        0x43 => SimdKind::Binary,              // f32x4_lt
        0x44 => SimdKind::Binary,              // f32x4_gt
        0x45 => SimdKind::Binary,              // f32x4_le
        0x46 => SimdKind::Binary,              // f32x4_ge
        0x47 => SimdKind::Binary,              // f64x2_eq
        0x48 => SimdKind::Binary,              // f64x2_ne
        0x49 => SimdKind::Binary,              // f64x2_lt
        0x4a => SimdKind::Binary,              // f64x2_gt
        0x4b => SimdKind::Binary,              // f64x2_le
        0x4c => SimdKind::Binary,              // f64x2_ge
        0x4d => SimdKind::Unary,               // v128_not
        0x4e => SimdKind::Binary,              // v128_and
        0x4f => SimdKind::Binary,              // v128_andnot
        0x50 => SimdKind::Binary,              // v128_or
        0x51 => SimdKind::Binary,              // v128_xor
        0x52 => SimdKind::Ternary,             // v128_bitselect
        0x53 => SimdKind::Test,                // v128_any_true
        0x54 => SimdKind::LoadLane { size: 1 }, // v128_load8_lane
        0x55 => SimdKind::LoadLane { size: 2 }, // v128_load16_lane
        0x56 => SimdKind::LoadLane { size: 4 }, // v128_load32_lane
        0x57 => SimdKind::LoadLane { size: 8 }, // v128_load64_lane
        0x58 => SimdKind::StoreLane { size: 1 }, // v128_store8_lane
        0x59 => SimdKind::StoreLane { size: 2 }, // v128_store16_lane
        0x5a => SimdKind::StoreLane { size: 4 }, // v128_store32_lane
        0x5b => SimdKind::StoreLane { size: 8 }, // v128_store64_lane
        0x5c => SimdKind::Load { size: 4 },    // v128_load32_zero
        0x5d => SimdKind::Load { size: 8 },    // v128_load64_zero
        0x5e => SimdKind::Unary,               // f32x4_demote_f64x2_zero
        0x5f => SimdKind::Unary,               // f64x2_promote_low_f32x4
        0x60 => SimdKind::Unary,               // i8x16_abs
        0x61 => SimdKind::Unary,               // i8x16_neg
        0x62 => SimdKind::Unary,               // i8x16_popcnt
        0x63 => SimdKind::Test,                // i8x16_all_true
        0x64 => SimdKind::Test,                // i8x16_bitmask
        0x65 => SimdKind::Binary,              // i8x16_narrow_i16x8_s
        0x66 => SimdKind::Binary,              // i8x16_narrow_i16x8_u
        0x67 => SimdKind::Unary,               // f32x4_ceil
        0x68 => SimdKind::Unary,               // f32x4_floor
        0x69 => SimdKind::Unary,               // f32x4_trunc
        0x6a => SimdKind::Unary,               // f32x4_nearest
        0x6b => SimdKind::Shift,               // i8x16_shl
        0x6c => SimdKind::Shift,               // i8x16_shr_s
        0x6d => SimdKind::Shift,               // i8x16_shr_u
        0x6e => SimdKind::Binary,              // i8x16_add
        0x6f => SimdKind::Binary,              // i8x16_add_sat_s
        0x70 => SimdKind::Binary,              // i8x16_add_sat_u
        0x71 => SimdKind::Binary,              // i8x16_sub
        0x72 => SimdKind::Binary,              // i8x16_sub_sat_s
        0x73 => SimdKind::Binary,              // i8x16_sub_sat_u
        0x74 => SimdKind::Unary,               // f64x2_ceil
        0x75 => SimdKind::Unary,               // f64x2_floor
        0x76 => SimdKind::Binary,              // i8x16_min_s
        0x77 => SimdKind::Binary,              // i8x16_min_u
        0x78 => SimdKind::Binary,              // i8x16_max_s
        0x79 => SimdKind::Binary,              // i8x16_max_u
        0x7a => SimdKind::Unary,               // f64x2_trunc
        0x7b => SimdKind::Binary,              // i8x16_avgr_u
        0x7c => SimdKind::Unary,               // i16x8_extadd_pairwise_i8x16_s
        0x7d => SimdKind::Unary,               // i16x8_extadd_pairwise_i8x16_u
        0x7e => SimdKind::Unary,               // i32x4_extadd_pairwise_i16x8_s
        0x7f => SimdKind::Unary,               // i32x4_extadd_pairwise_i16x8_u
        0x80 => SimdKind::Unary,               // i16x8_abs
        0x81 => SimdKind::Unary,               // i16x8_neg
        0x82 => SimdKind::Binary,              // i16x8_q15mulr_sat_s
        0x83 => SimdKind::Test,                // i16x8_all_true
        0x84 => SimdKind::Test,                // i16x8_bitmask
        0x85 => SimdKind::Binary,              // i16x8_narrow_i32x4_s
        0x86 => SimdKind::Binary,              // i16x8_narrow_i32x4_u
        0x87 => SimdKind::Unary,               // i16x8_extend_low_i8x16_s
        0x88 => SimdKind::Unary,               // i16x8_extend_high_i8x16_s
        0x89 => SimdKind::Unary,               // i16x8_extend_low_i8x16_u
        0x8a => SimdKind::Unary,               // i16x8_extend_high_i8x16_u
        0x8b => SimdKind::Shift,               // i16x8_shl
        0x8c => SimdKind::Shift,               // i16x8_shr_s
        0x8d => SimdKind::Shift,               // i16x8_shr_u
        0x8e => SimdKind::Binary,              // i16x8_add
        0x8f => SimdKind::Binary,              // i16x8_add_sat_s
        0x90 => SimdKind::Binary,              // i16x8_add_sat_u
        0x91 => SimdKind::Binary,              // i16x8_sub
        0x92 => SimdKind::Binary,              // i16x8_sub_sat_s
        0x93 => SimdKind::Binary,              // i16x8_sub_sat_u
        0x94 => SimdKind::Unary,               // f64x2_nearest
        0x95 => SimdKind::Binary,              // i16x8_mul
        0x96 => SimdKind::Binary,              // i16x8_min_s
        0x97 => SimdKind::Binary,              // i16x8_min_u
        0x98 => SimdKind::Binary,              // i16x8_max_s
        0x99 => SimdKind::Binary,              // i16x8_max_u
        0x9b => SimdKind::Binary,              // i16x8_avgr_u
        0x9c => SimdKind::Binary,              // i16x8_extmul_low_i8x16_s
        0x9d => SimdKind::Binary,              // i16x8_extmul_high_i8x16_s
        0x9e => SimdKind::Binary,              // i16x8_extmul_low_i8x16_u
        0x9f => SimdKind::Binary,              // i16x8_extmul_high_i8x16_u
        0xa0 => SimdKind::Unary,               // i32x4_abs
        0xa1 => SimdKind::Unary,               // i32x4_neg
        0xa3 => SimdKind::Test,                // i32x4_all_true
        0xa4 => SimdKind::Test,                // i32x4_bitmask
        0xa7 => SimdKind::Unary,               // i32x4_extend_low_i16x8_s
        0xa8 => SimdKind::Unary,               // i32x4_extend_high_i16x8_s
        0xa9 => SimdKind::Unary,               // i32x4_extend_low_i16x8_u
        0xaa => SimdKind::Unary,               // i32x4_extend_high_i16x8_u
        0xab => SimdKind::Shift,               // i32x4_shl
        0xac => SimdKind::Shift,               // i32x4_shr_s
        0xad => SimdKind::Shift,               // i32x4_shr_u
        0xae => SimdKind::Binary,              // i32x4_add
        0xb1 => SimdKind::Binary,              // i32x4_sub
        0xb5 => SimdKind::Binary,              // i32x4_mul
        0xb6 => SimdKind::Binary,              // i32x4_min_s
        0xb7 => SimdKind::Binary,              // i32x4_min_u
        0xb8 => SimdKind::Binary,              // i32x4_max_s
        0xb9 => SimdKind::Binary,              // i32x4_max_u
        0xba => SimdKind::Binary,              // i32x4_dot_i16x8_s
        0xbc => SimdKind::Binary,              // i32x4_extmul_low_i16x8_s
        0xbd => SimdKind::Binary,              // i32x4_extmul_high_i16x8_s
        0xbe => SimdKind::Binary,              // i32x4_extmul_low_i16x8_u
        0xbf => SimdKind::Binary,              // i32x4_extmul_high_i16x8_u
        0xc0 => SimdKind::Unary,               // i64x2_abs
        0xc1 => SimdKind::Unary,               // i64x2_neg
        0xc3 => SimdKind::Test,                // i64x2_all_true
        0xc4 => SimdKind::Test,                // i64x2_bitmask
        0xc7 => SimdKind::Unary,               // i64x2_extend_low_i32x4_s
        0xc8 => SimdKind::Unary,               // i64x2_extend_high_i32x4_s
        0xc9 => SimdKind::Unary,               // i64x2_extend_low_i32x4_u
        0xca => SimdKind::Unary,               // i64x2_extend_high_i32x4_u
        0xcb => SimdKind::Shift,               // i64x2_shl
        0xcc => SimdKind::Shift,               // i64x2_shr_s
        0xcd => SimdKind::Shift,               // i64x2_shr_u
        0xce => SimdKind::Binary,              // i64x2_add
        0xd1 => SimdKind::Binary,              // i64x2_sub
        0xd5 => SimdKind::Binary,              // i64x2_mul
        0xd6 => SimdKind::Binary,              // i64x2_eq
        0xd7 => SimdKind::Binary,              // i64x2_ne
        0xd8 => SimdKind::Binary,              // i64x2_lt_s
        0xd9 => SimdKind::Binary,              // i64x2_gt_s
        0xda => SimdKind::Binary,              // i64x2_le_s
        0xdb => SimdKind::Binary,              // i64x2_ge_s
        0xdc => SimdKind::Binary,              // i64x2_extmul_low_i32x4_s
        0xdd => SimdKind::Binary,              // i64x2_extmul_high_i32x4_s
        0xde => SimdKind::Binary,              // i64x2_extmul_low_i32x4_u
        0xdf => SimdKind::Binary,              // i64x2_extmul_high_i32x4_u
        0xe0 => SimdKind::Unary,               // f32x4_abs
        0xe1 => SimdKind::Unary,               // f32x4_neg
        0xe3 => SimdKind::Unary,               // f32x4_sqrt
        0xe4 => SimdKind::Binary,              // f32x4_add
        0xe5 => SimdKind::Binary,              // f32x4_sub
        0xe6 => SimdKind::Binary,              // f32x4_mul
        0xe7 => SimdKind::Binary,              // f32x4_div
        0xe8 => SimdKind::Binary,              // f32x4_min
        0xe9 => SimdKind::Binary,              // f32x4_max
        0xea => SimdKind::Binary,              // f32x4_pmin
        0xeb => SimdKind::Binary,              // f32x4_pmax
        0xec => SimdKind::Unary,               // f64x2_abs
        0xed => SimdKind::Unary,               // f64x2_neg
        0xef => SimdKind::Unary,               // f64x2_sqrt
        0xf0 => SimdKind::Binary,              // f64x2_add
        0xf1 => SimdKind::Binary,              // f64x2_sub
        0xf2 => SimdKind::Binary,              // f64x2_mul
        0xf3 => SimdKind::Binary,              // f64x2_div
        0xf4 => SimdKind::Binary,              // f64x2_min
        0xf5 => SimdKind::Binary,              // f64x2_max
        0xf6 => SimdKind::Binary,              // f64x2_pmin
        0xf7 => SimdKind::Binary,              // f64x2_pmax
        0xf8 => SimdKind::Unary,               // i32x4_trunc_sat_f32x4_s
        0xf9 => SimdKind::Unary,               // i32x4_trunc_sat_f32x4_u
        0xfa => SimdKind::Unary,               // f32x4_convert_i32x4_s
        0xfb => SimdKind::Unary,               // f32x4_convert_i32x4_u
        0xfc => SimdKind::Unary,               // i32x4_trunc_sat_f64x2_s_zero
        0xfd => SimdKind::Unary,               // i32x4_trunc_sat_f64x2_u_zero
        0xfe => SimdKind::Unary,               // f64x2_convert_low_i32x4_s
        0xff => SimdKind::Unary,               // f64x2_convert_low_i32x4_u
        0x100 => SimdKind::Binary,             // i8x16_relaxed_swizzle
        0x101 => SimdKind::Unary,              // i32x4_relaxed_trunc_f32x4_s
        0x102 => SimdKind::Unary,              // i32x4_relaxed_trunc_f32x4_u
        0x103 => SimdKind::Unary,              // i32x4_relaxed_trunc_f64x2_s_zero
        0x104 => SimdKind::Unary,              // i32x4_relaxed_trunc_f64x2_u_zero
        0x105 => SimdKind::Ternary,            // f32x4_relaxed_madd
        0x106 => SimdKind::Ternary,            // f32x4_relaxed_nmadd
        0x107 => SimdKind::Ternary,            // f64x2_relaxed_madd
        0x108 => SimdKind::Ternary,            // f64x2_relaxed_nmadd
        0x109 => SimdKind::Ternary,            // i8x16_relaxed_laneselect
        0x10a => SimdKind::Ternary,            // i16x8_relaxed_laneselect
        0x10b => SimdKind::Ternary,            // i32x4_relaxed_laneselect
        0x10c => SimdKind::Ternary,            // i64x2_relaxed_laneselect
        0x10d => SimdKind::Binary,             // f32x4_relaxed_min
        0x10e => SimdKind::Binary,             // f32x4_relaxed_max
        0x10f => SimdKind::Binary,             // f64x2_relaxed_min
        0x110 => SimdKind::Binary,             // f64x2_relaxed_max
        0x111 => SimdKind::Binary,             // i16x8_relaxed_q15mulr_s
        0x112 => SimdKind::Binary,             // i16x8_relaxed_dot_i8x16_i7x16_s
        0x113 => SimdKind::Ternary,            // i32x4_relaxed_dot_i8x16_i7x16_add_s
        _ => return None,
    })
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AtomicKind {
    Notify,
    Wait { value: ValType },
    Fence,
    Load { value: ValType, size: u32 },
    Store { value: ValType, size: u32 },
    ReadModifyWrite { value: ValType, size: u32 },
    CompareExchange { value: ValType, size: u32 },
}

pub fn atomic_kind(sub_opcode: u32) -> Option<AtomicKind> {
    Some(match sub_opcode {
        0x0 => AtomicKind::Notify,
        0x1 => AtomicKind::Wait { value: ValType::I32 },
        0x2 => AtomicKind::Wait { value: ValType::I64 },
        0x3 => AtomicKind::Fence,
        0x10 => AtomicKind::Load {
            value: ValType::I32,
            size: 4,
        },
        0x11 => AtomicKind::Load {
            value: ValType::I64,
            size: 8,
        },
        0x12 => AtomicKind::Load {
            value: ValType::I32,
            size: 1,
        },
        0x13 => AtomicKind::Load {
            value: ValType::I32,
            size: 2,
        },
        0x14 => AtomicKind::Load {
            value: ValType::I64,
            size: 1,
        },
        0x15 => AtomicKind::Load {
            value: ValType::I64,
            size: 2,
        },
        0x16 => AtomicKind::Load {
            value: ValType::I64,
            size: 4,
        },
        0x17 => AtomicKind::Store {
            value: ValType::I32,
            size: 4,
        },
        0x18 => AtomicKind::Store {
            value: ValType::I64,
            size: 8,
        },
        0x19 => AtomicKind::Store {
            value: ValType::I32,
            size: 1,
        },
        0x1a => AtomicKind::Store {
            value: ValType::I32,
            size: 2,
        },
        0x1b => AtomicKind::Store {
            value: ValType::I64,
            size: 1,
        },
        0x1c => AtomicKind::Store {
            value: ValType::I64,
            size: 2,
        },
        0x1d => AtomicKind::Store {
            value: ValType::I64,
            size: 4,
        },
        0x1e => AtomicKind::ReadModifyWrite {
            value: ValType::I32,
            size: 4,
        },
        0x1f => AtomicKind::ReadModifyWrite {
            value: ValType::I64,
            size: 8,
        },
        0x20 => AtomicKind::ReadModifyWrite {
            value: ValType::I32,
            size: 1,
        },
        0x21 => AtomicKind::ReadModifyWrite {
            value: ValType::I32,
            size: 2,
        },
        0x22 => AtomicKind::ReadModifyWrite {
            value: ValType::I64,
            size: 1,
        },
        0x23 => AtomicKind::ReadModifyWrite {
            value: ValType::I64,
            size: 2,
        },
        0x24 => AtomicKind::ReadModifyWrite {
            value: ValType::I64,
            size: 4,
        },
        0x25 => AtomicKind::ReadModifyWrite {
            value: ValType::I32,
            size: 4,
        },
        0x26 => AtomicKind::ReadModifyWrite {
            value: ValType::I64,
            size: 8,
        },
        0x27 => AtomicKind::ReadModifyWrite {
            value: ValType::I32,
            size: 1,
        },
        0x28 => AtomicKind::ReadModifyWrite {
            value: ValType::I32,
            size: 2,
        },
        0x29 => AtomicKind::ReadModifyWrite {
            value: ValType::I64,
            size: 1,
        },
        0x2a => AtomicKind::ReadModifyWrite {
            value: ValType::I64,
            size: 2,
        },
        0x2b => AtomicKind::ReadModifyWrite {
            value: ValType::I64,
            size: 4,
        },
        0x2c => AtomicKind::ReadModifyWrite {
            value: ValType::I32,
            size: 4,
        },
        0x2d => AtomicKind::ReadModifyWrite {
            value: ValType::I64,
            size: 8,
        },
        0x2e => AtomicKind::ReadModifyWrite {
            value: ValType::I32,
            size: 1,
        },
        0x2f => AtomicKind::ReadModifyWrite {
            value: ValType::I32,
            size: 2,
        },
        0x30 => AtomicKind::ReadModifyWrite {
            value: ValType::I64,
            size: 1,
        },
        0x31 => AtomicKind::ReadModifyWrite {
            value: ValType::I64,
            size: 2,
        },
        0x32 => AtomicKind::ReadModifyWrite {
            value: ValType::I64,
            size: 4,
        },
        0x33 => AtomicKind::ReadModifyWrite {
            value: ValType::I32,
            size: 4,
        },
        0x34 => AtomicKind::ReadModifyWrite {
            value: ValType::I64,
            size: 8,
        },
        0x35 => AtomicKind::ReadModifyWrite {
            value: ValType::I32,
            size: 1,
        },
        0x36 => AtomicKind::ReadModifyWrite {
            value: ValType::I32,
            size: 2,
        },
        0x37 => AtomicKind::ReadModifyWrite {
            value: ValType::I64,
            size: 1,
        },
        0x38 => AtomicKind::ReadModifyWrite {
            value: ValType::I64,
            size: 2,
        },
        0x39 => AtomicKind::ReadModifyWrite {
            value: ValType::I64,
            size: 4,
        },
        0x3a => AtomicKind::ReadModifyWrite {
            value: ValType::I32,
            size: 4,
        },
        0x3b => AtomicKind::ReadModifyWrite {
            value: ValType::I64,
            size: 8,
        },
        0x3c => AtomicKind::ReadModifyWrite {
            value: ValType::I32,
            size: 1,
        },
        0x3d => AtomicKind::ReadModifyWrite {
            value: ValType::I32,
            size: 2,
        },
        0x3e => AtomicKind::ReadModifyWrite {
            value: ValType::I64,
            size: 1,
        },
        0x3f => AtomicKind::ReadModifyWrite {
            value: ValType::I64,
            size: 2,
        },
        0x40 => AtomicKind::ReadModifyWrite {
            value: ValType::I64,
            size: 4,
        },
        0x41 => AtomicKind::ReadModifyWrite {
            value: ValType::I32,
            size: 4,
        },
        0x42 => AtomicKind::ReadModifyWrite {
            value: ValType::I64,
            size: 8,
        },
        0x43 => AtomicKind::ReadModifyWrite {
            value: ValType::I32,
            size: 1,
        },
        0x44 => AtomicKind::ReadModifyWrite {
            value: ValType::I32,
            size: 2,
        },
        0x45 => AtomicKind::ReadModifyWrite {
            value: ValType::I64,
            size: 1,
        },
        0x46 => AtomicKind::ReadModifyWrite {
            value: ValType::I64,
            size: 2,
        },
        0x47 => AtomicKind::ReadModifyWrite {
            value: ValType::I64,
            size: 4,
        },
        0x48 => AtomicKind::CompareExchange {
            value: ValType::I32,
            size: 4,
        },
        0x49 => AtomicKind::CompareExchange {
            value: ValType::I64,
            size: 8,
        },
        0x4a => AtomicKind::CompareExchange {
            value: ValType::I32,
            size: 1,
        },
        0x4b => AtomicKind::CompareExchange {
            value: ValType::I32,
            size: 2,
        },
        0x4c => AtomicKind::CompareExchange {
            value: ValType::I64,
            size: 1,
        },
        0x4d => AtomicKind::CompareExchange {
            value: ValType::I64,
            size: 2,
        },
        0x4e => AtomicKind::CompareExchange {
            value: ValType::I64,
            size: 4,
        },
        _ => return None,
    })
}
