/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Unary, binary, logical, conditional and update expressions.

use crate::ast::BinaryOp;
use crate::bytecode::executable_data::ConstantValue;
use crate::bytecode::operand::Operand;

use crate::bytecode::instruction::typed as op;

use super::context::Generator;

pub(super) fn emit_binary_op(generator: &mut Generator<'_>, op: BinaryOp, dst: Operand, lhs: Operand, rhs: Operand) {
    match op {
        BinaryOp::Addition => {
            let arith_feedback = generator.next_arith_feedback();
            generator.put(op::Add {
                arith_feedback,
                dst,
                lhs,
                rhs,
            });
        }
        BinaryOp::Subtraction => {
            let arith_feedback = generator.next_arith_feedback();
            generator.put(op::Sub {
                arith_feedback,
                dst,
                lhs,
                rhs,
            });
        }
        BinaryOp::Multiplication => {
            let arith_feedback = generator.next_arith_feedback();
            generator.put(op::Mul {
                arith_feedback,
                dst,
                lhs,
                rhs,
            });
        }
        BinaryOp::Division => {
            let arith_feedback = generator.next_arith_feedback();
            generator.put(op::Div {
                arith_feedback,
                dst,
                lhs,
                rhs,
            });
        }
        BinaryOp::Modulo => {
            let arith_feedback = generator.next_arith_feedback();
            generator.put(op::Mod {
                arith_feedback,
                dst,
                lhs,
                rhs,
            });
        }
        BinaryOp::Exponentiation => {
            let arith_feedback = generator.next_arith_feedback();
            generator.put(op::Exp {
                arith_feedback,
                dst,
                lhs,
                rhs,
            });
        }
        BinaryOp::StrictlyEquals => {
            let arith_feedback = generator.next_arith_feedback();
            generator.put(op::StrictlyEquals {
                arith_feedback,
                dst,
                lhs,
                rhs,
            });
        }
        BinaryOp::StrictlyInequals => {
            let arith_feedback = generator.next_arith_feedback();
            generator.put(op::StrictlyInequals {
                arith_feedback,
                dst,
                lhs,
                rhs,
            });
        }
        BinaryOp::LooselyEquals => {
            let arith_feedback = generator.next_arith_feedback();
            generator.put(op::LooselyEquals {
                arith_feedback,
                dst,
                lhs,
                rhs,
            });
        }
        BinaryOp::LooselyInequals => {
            let arith_feedback = generator.next_arith_feedback();
            generator.put(op::LooselyInequals {
                arith_feedback,
                dst,
                lhs,
                rhs,
            });
        }
        BinaryOp::GreaterThan => {
            let arith_feedback = generator.next_arith_feedback();
            generator.put(op::GreaterThan {
                arith_feedback,
                dst,
                lhs,
                rhs,
            });
        }
        BinaryOp::GreaterThanEquals => {
            let arith_feedback = generator.next_arith_feedback();
            generator.put(op::GreaterThanEquals {
                arith_feedback,
                dst,
                lhs,
                rhs,
            });
        }
        BinaryOp::LessThan => {
            let arith_feedback = generator.next_arith_feedback();
            generator.put(op::LessThan {
                arith_feedback,
                dst,
                lhs,
                rhs,
            });
        }
        BinaryOp::LessThanEquals => {
            let arith_feedback = generator.next_arith_feedback();
            generator.put(op::LessThanEquals {
                arith_feedback,
                dst,
                lhs,
                rhs,
            });
        }
        BinaryOp::BitwiseAnd => {
            let arith_feedback = generator.next_arith_feedback();
            generator.put(op::BitwiseAnd {
                arith_feedback,
                dst,
                lhs,
                rhs,
            });
        }
        BinaryOp::BitwiseOr => {
            // OPTIMIZATION: x | 0 == ToInt32(x)
            if let Some(ConstantValue::Number(n)) = generator.get_constant(rhs) {
                if *n == 0.0 && n.is_sign_positive() {
                    let arith_feedback = generator.next_arith_feedback();
                    generator.put(op::ToInt32 {
                        arith_feedback,
                        dst,
                        value: lhs,
                    });
                } else {
                    let arith_feedback = generator.next_arith_feedback();
                    generator.put(op::BitwiseOr {
                        arith_feedback,
                        dst,
                        lhs,
                        rhs,
                    });
                }
            } else {
                let arith_feedback = generator.next_arith_feedback();
                generator.put(op::BitwiseOr {
                    arith_feedback,
                    dst,
                    lhs,
                    rhs,
                });
            }
        }
        BinaryOp::BitwiseXor => {
            let arith_feedback = generator.next_arith_feedback();
            generator.put(op::BitwiseXor {
                arith_feedback,
                dst,
                lhs,
                rhs,
            });
        }
        BinaryOp::LeftShift => {
            let arith_feedback = generator.next_arith_feedback();
            generator.put(op::LeftShift {
                arith_feedback,
                dst,
                lhs,
                rhs,
            });
        }
        BinaryOp::RightShift => {
            // OPTIMIZATION: x >> 0 == ToInt32(x) (matches C++)
            if let Some(ConstantValue::Number(n)) = generator.get_constant(rhs) {
                if *n == 0.0 && n.is_sign_positive() {
                    let arith_feedback = generator.next_arith_feedback();
                    generator.put(op::ToInt32 {
                        arith_feedback,
                        dst,
                        value: lhs,
                    });
                } else {
                    let arith_feedback = generator.next_arith_feedback();
                    generator.put(op::RightShift {
                        arith_feedback,
                        dst,
                        lhs,
                        rhs,
                    });
                }
            } else {
                let arith_feedback = generator.next_arith_feedback();
                generator.put(op::RightShift {
                    arith_feedback,
                    dst,
                    lhs,
                    rhs,
                });
            }
        }
        BinaryOp::UnsignedRightShift => {
            let arith_feedback = generator.next_arith_feedback();
            generator.put(op::UnsignedRightShift {
                arith_feedback,
                dst,
                lhs,
                rhs,
            });
        }
        BinaryOp::In => generator.put(op::In { dst, lhs, rhs }),
        BinaryOp::InstanceOf => generator.put(op::InstanceOf { dst, lhs, rhs }),
    }
}

/// Convert a property name to the primitive that ToPropertyKey turns into a property key without observable effects,
/// in a register of its own unless it is a constant, which is a primitive already.
pub(super) fn emit_to_primitive_property_key(generator: &mut Generator<'_>, property_name: Operand) -> Operand {
    if property_name.is_constant() {
        return property_name;
    }
    // NB: The name can be the operand of a variable or of `this`, which converting it in place would change.
    let property_key = generator.held_register();
    generator.put(op::ToPrimitiveWithStringHint {
        dst: property_key,
        value: property_name,
    });
    property_key
}
