/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Local folding: constant folding and algebraic simplification of a node about to be created, from its inputs
//! alone. Floats are left alone: their results depend on NaN payloads the code may observe.

use crate::ir::{FLAG_CANNOT_TRAP, Func, IntCondition, Op, Type, Value};

/// What a node about to be created simplifies to.
pub enum Folded {
    /// An existing value.
    Value(Value),
    /// A constant of the node's type.
    Constant(u64),
    /// The node, possibly with its inputs swapped or its flags changed.
    Node { op: Op, flags: u8, a: Value, b: Value },
}

fn mask(ty: Type, bits: u64) -> u64 {
    if ty == Type::I32 { bits & 0xffff_ffff } else { bits }
}

fn width(ty: Type) -> u32 {
    if ty == Type::I32 { 32 } else { 64 }
}

/// The signed value of `bits` of type `ty`.
fn signed(ty: Type, bits: u64) -> i64 {
    if ty == Type::I32 {
        i64::from(bits as u32 as i32)
    } else {
        bits as i64
    }
}

fn evaluate_binary(op: Op, ty: Type, a: u64, b: u64, flags: u8) -> Option<u64> {
    let bits = width(ty);
    let shift = (b as u32) & (bits - 1);
    let result = match op {
        Op::Add => a.wrapping_add(b),
        Op::Sub => a.wrapping_sub(b),
        Op::Mul => a.wrapping_mul(b),
        Op::And => a & b,
        Op::Or => a | b,
        Op::Xor => a ^ b,
        Op::Shl => a << shift,
        Op::ShrS => (signed(ty, a) >> shift) as u64,
        Op::ShrU => mask(ty, a) >> shift,
        Op::Rotl if ty == Type::I32 => u64::from((a as u32).rotate_left(shift)),
        Op::Rotl => a.rotate_left(shift),
        Op::Rotr if ty == Type::I32 => u64::from((a as u32).rotate_right(shift)),
        Op::Rotr => a.rotate_right(shift),
        Op::DivU | Op::RemU | Op::DivS | Op::RemS if mask(ty, b) == 0 => return None,
        Op::DivU => mask(ty, a) / mask(ty, b),
        Op::RemU => mask(ty, a) % mask(ty, b),
        Op::DivS => {
            let (a, b) = (signed(ty, a), signed(ty, b));
            if b == -1 && a == signed(ty, 1 << (bits - 1)) {
                return None;
            }
            (a / b) as u64
        }
        Op::RemS => {
            let (a, b) = (signed(ty, a), signed(ty, b));
            if b == -1 { 0 } else { (a % b) as u64 }
        }
        Op::Compare => u64::from(IntCondition::from_flags(flags).evaluate(a, b, ty.is_64())),
        _ => return None,
    };
    Some(mask(ty, result))
}

/// Folds an integer binary op of type `ty` (for `Compare`, the operands' type) on `a` and `b`.
pub fn binary(func: &Func, op: Op, ty: Type, flags: u8, a: Value, b: Value) -> Folded {
    let constant_a = func.node(a).constant();
    let constant_b = func.node(b).constant();
    if let (Some(x), Some(y)) = (constant_a, constant_b)
        && let Some(result) = evaluate_binary(op, ty, x, y, flags)
    {
        return Folded::Constant(result);
    }
    // Constants go to the right.
    if constant_a.is_some() && constant_b.is_none() {
        if op.is_commutative() {
            return binary(func, op, ty, flags, b, a);
        }
        if op == Op::Compare {
            let swapped = IntCondition::from_flags(flags).swap() as u8;
            return binary(func, op, ty, swapped, b, a);
        }
    }
    let all_ones = mask(ty, u64::MAX);
    if let Some(y) = constant_b {
        let shift = (y as u32) & (width(ty) - 1);
        match op {
            Op::Add | Op::Sub | Op::Or | Op::Xor if y == 0 => return Folded::Value(a),
            Op::Shl | Op::ShrS | Op::ShrU | Op::Rotl | Op::Rotr if shift == 0 => return Folded::Value(a),
            Op::Mul | Op::DivS | Op::DivU if y == 1 => return Folded::Value(a),
            Op::Mul | Op::And if y == 0 => return Folded::Constant(0),
            Op::And if y == all_ones => return Folded::Value(a),
            Op::Or if y == all_ones => return Folded::Constant(all_ones),
            Op::RemU if y == 1 => return Folded::Constant(0),
            Op::RemS if y == 1 || y == all_ones => return Folded::Constant(0),
            Op::DivU | Op::RemU if y != 0 => {
                return Folded::Node {
                    op,
                    flags: flags | FLAG_CANNOT_TRAP,
                    a,
                    b,
                };
            }
            Op::DivS | Op::RemS if y != 0 && y != all_ones => {
                return Folded::Node {
                    op,
                    flags: flags | FLAG_CANNOT_TRAP,
                    a,
                    b,
                };
            }
            Op::Compare if y == 0 => match IntCondition::from_flags(flags) {
                IntCondition::LtU => return Folded::Constant(0),
                IntCondition::GeU => return Folded::Constant(1),
                _ => {}
            },
            _ => {}
        }
    }
    if a == b {
        match op {
            Op::Sub | Op::Xor => return Folded::Constant(0),
            Op::And | Op::Or => return Folded::Value(a),
            Op::Compare => {
                let holds = matches!(
                    IntCondition::from_flags(flags),
                    IntCondition::Eq | IntCondition::LeS | IntCondition::LeU | IntCondition::GeS | IntCondition::GeU
                );
                return Folded::Constant(u64::from(holds));
            }
            _ => {}
        }
    }
    Folded::Node { op, flags, a, b }
}

/// Folds an integer unary op producing `ty` on `a`.
pub fn unary(func: &Func, op: Op, ty: Type, a: Value) -> Option<Folded> {
    let input = func.node(a);
    if let Some(x) = input.constant() {
        let input_type = input.ty;
        let result = match op {
            Op::Clz if ty == Type::I32 => u64::from((x as u32).leading_zeros()),
            Op::Clz => u64::from(x.leading_zeros()),
            Op::Ctz if ty == Type::I32 => u64::from((x as u32).trailing_zeros()),
            Op::Ctz => u64::from(x.trailing_zeros()),
            Op::Popcnt => u64::from(x.count_ones()),
            Op::Eqz => u64::from(mask(input_type, x) == 0),
            Op::Extend8S => x as u8 as i8 as i64 as u64,
            Op::Extend16S => x as u16 as i16 as i64 as u64,
            Op::Extend32S | Op::ExtendI32S => x as u32 as i32 as i64 as u64,
            Op::ExtendI32U => u64::from(x as u32),
            Op::Wrap => x,
            _ => return None,
        };
        return Some(Folded::Constant(mask(ty, result)));
    }
    match (op, input.op) {
        (Op::Eqz, Op::Compare) => Some(Folded::Node {
            op: Op::Compare,
            flags: IntCondition::from_flags(input.flags).invert() as u8,
            a: input.args[0],
            b: input.args[1],
        }),
        (Op::Wrap, Op::ExtendI32S | Op::ExtendI32U) => Some(Folded::Value(input.args[0])),
        _ => None,
    }
}
