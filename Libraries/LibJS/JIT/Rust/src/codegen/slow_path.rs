/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Which interpreter slow path each generic opcode calls, and how.
//!
//! Generic nodes call the same `asm_slow_path_*` functions the interpreter's
//! handlers call, with the same C calling convention. `RuntimeInfo::slow_paths`
//! holds their addresses, indexed by opcode; `slow_path_symbol()` names the
//! function the runtime must put there for each opcode.

use crate::builder::handling;
use crate::bytecode::Instruction;
use crate::bytecode::OpCode;
use crate::bytecode::Operand;

/// The calling convention of an opcode's slow path.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SlowPathCall {
    /// The function declared by `JS_DECLARE_SLOW_PATH_<Op>`, taking the
    /// instruction's operands as described by `slow_path_layout()`.
    Record,
    /// `i64 f(VM*, u32 pc, Value& dst, Value lhs, Value rhs)`.
    BinaryValues,
    /// `i64 f(VM*, u32 pc, Value lhs, Value rhs, u32 true_target, u32 false_target)`,
    /// returning the target to continue at.
    JumpValues,
    /// `i64 libjs_jit_call(VM*, ExecutionContext*, u32 pc)`.
    JitCall,
}

/// Where a binary slow path gets one of its operands.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ValueSource {
    Slot(Operand),
    /// An int32 immediate of a specialized instruction.
    Int32(i32),
}

/// The calling convention of `opcode`'s slow path, or `None` if the opcode is
/// not generic.
pub fn slow_path_call(opcode: OpCode) -> Option<SlowPathCall> {
    let info = handling(opcode).generic()?;
    if info.is_js_call {
        return Some(SlowPathCall::JitCall);
    }
    let name = opcode.name();
    if name.starts_with("Jump") {
        return Some(SlowPathCall::JumpValues);
    }
    if is_binary_values_opcode(opcode) {
        return Some(SlowPathCall::BinaryValues);
    }
    Some(SlowPathCall::Record)
}

fn is_binary_values_opcode(opcode: OpCode) -> bool {
    use OpCode as O;
    matches!(
        opcode,
        O::Add
            | O::Sub
            | O::Mul
            | O::Exp
            | O::Div
            | O::Mod
            | O::BitwiseXor
            | O::BitwiseAnd
            | O::BitwiseOr
            | O::LeftShift
            | O::RightShift
            | O::UnsignedRightShift
            | O::LessThan
            | O::LessThanEquals
            | O::GreaterThan
            | O::GreaterThanEquals
            | O::StrictlyEquals
            | O::StrictlyInequals
            | O::LooselyEquals
            | O::LooselyInequals
            | O::AddLhsInt32
            | O::AddRhsInt32
            | O::SubRhsInt32
            | O::MulRhsInt32
            | O::ExpRhsInt32
            | O::DivRhsInt32
            | O::ModRhsInt32
            | O::BitwiseXorRhsInt32
            | O::BitwiseAndRhsInt32
            | O::BitwiseOrRhsInt32
            | O::LeftShiftRhsInt32
            | O::RightShiftRhsInt32
            | O::UnsignedRightShiftRhsInt32
            | O::LessThanRhsInt32
            | O::LessThanEqualsRhsInt32
            | O::GreaterThanRhsInt32
            | O::GreaterThanEqualsRhsInt32
            | O::StrictlyEqualsRhsInt32
            | O::StrictlyInequalsRhsInt32
            | O::LooselyEqualsRhsInt32
            | O::LooselyInequalsRhsInt32
    )
}

fn snake_case(name: &str) -> String {
    let mut result = String::new();
    for (index, character) in name.chars().enumerate() {
        if character.is_ascii_uppercase() && index != 0 {
            result.push('_');
        }
        result.push(character.to_ascii_lowercase());
    }
    result
}

/// The C function the runtime must store in `RuntimeInfo::slow_paths` for
/// `opcode`, or `None` if JIT code never calls one for it.
pub fn slow_path_symbol(opcode: OpCode) -> Option<String> {
    let name = opcode.name();
    let base = name
        .strip_suffix("RhsInt32")
        .or_else(|| name.strip_suffix("LhsInt32"))
        .unwrap_or(name);
    // Loop back edges share the slow path of the jump they replace.
    let base = base.strip_suffix("Loop").unwrap_or(base);
    Some(match slow_path_call(opcode)? {
        SlowPathCall::JitCall => return None,
        SlowPathCall::JumpValues => format!("asm_slow_path_{}_values", snake_case(base)),
        SlowPathCall::BinaryValues => format!("asm_slow_path_{}_values", snake_case(base)),
        SlowPathCall::Record => match opcode {
            OpCode::ThrowIfTDZ => "asm_slow_path_throw_if_tdz".to_string(),
            OpCode::GetCalleeAndThisFromEnvironment => "asm_slow_path_get_callee_and_this".to_string(),
            OpCode::DynamicGetCalleeAndThisFromEnvironment => "asm_slow_path_dynamic_get_callee_and_this".to_string(),
            OpCode::NewRegExp => "asm_slow_path_new_regexp".to_string(),
            OpCode::GetById => "asm_slow_path_get_by_id_from_jit".to_string(),
            OpCode::GetByValue => "asm_slow_path_get_by_value_from_jit".to_string(),
            OpCode::PutByValue => "asm_slow_path_put_by_value_from_jit".to_string(),
            _ => format!("asm_slow_path_{}", snake_case(name)).replace("_reg_exp_", "_regexp_"),
        },
    })
}

/// The destination and operands of a binary slow path call.
pub fn binary_operands(instruction: &Instruction) -> Option<(Operand, ValueSource, ValueSource)> {
    use Instruction as I;
    use ValueSource::Int32;
    use ValueSource::Slot;
    match instruction {
        I::Add { dst, lhs, rhs, .. }
        | I::Sub { dst, lhs, rhs, .. }
        | I::Mul { dst, lhs, rhs, .. }
        | I::Exp { dst, lhs, rhs, .. }
        | I::Div { dst, lhs, rhs, .. }
        | I::Mod { dst, lhs, rhs, .. }
        | I::BitwiseXor { dst, lhs, rhs, .. }
        | I::BitwiseAnd { dst, lhs, rhs, .. }
        | I::BitwiseOr { dst, lhs, rhs, .. }
        | I::LeftShift { dst, lhs, rhs, .. }
        | I::RightShift { dst, lhs, rhs, .. }
        | I::UnsignedRightShift { dst, lhs, rhs, .. }
        | I::LessThan { dst, lhs, rhs, .. }
        | I::LessThanEquals { dst, lhs, rhs, .. }
        | I::GreaterThan { dst, lhs, rhs, .. }
        | I::GreaterThanEquals { dst, lhs, rhs, .. }
        | I::StrictlyEquals { dst, lhs, rhs, .. }
        | I::StrictlyInequals { dst, lhs, rhs, .. }
        | I::LooselyEquals { dst, lhs, rhs, .. }
        | I::LooselyInequals { dst, lhs, rhs, .. } => Some((*dst, Slot(*lhs), Slot(*rhs))),
        I::AddRhsInt32 { dst, lhs, rhs, .. }
        | I::SubRhsInt32 { dst, lhs, rhs, .. }
        | I::MulRhsInt32 { dst, lhs, rhs, .. }
        | I::ExpRhsInt32 { dst, lhs, rhs, .. }
        | I::DivRhsInt32 { dst, lhs, rhs, .. }
        | I::ModRhsInt32 { dst, lhs, rhs, .. }
        | I::BitwiseXorRhsInt32 { dst, lhs, rhs, .. }
        | I::BitwiseAndRhsInt32 { dst, lhs, rhs, .. }
        | I::BitwiseOrRhsInt32 { dst, lhs, rhs, .. }
        | I::LeftShiftRhsInt32 { dst, lhs, rhs, .. }
        | I::RightShiftRhsInt32 { dst, lhs, rhs, .. }
        | I::UnsignedRightShiftRhsInt32 { dst, lhs, rhs, .. }
        | I::LessThanRhsInt32 { dst, lhs, rhs, .. }
        | I::LessThanEqualsRhsInt32 { dst, lhs, rhs, .. }
        | I::GreaterThanRhsInt32 { dst, lhs, rhs, .. }
        | I::GreaterThanEqualsRhsInt32 { dst, lhs, rhs, .. }
        | I::StrictlyEqualsRhsInt32 { dst, lhs, rhs, .. }
        | I::StrictlyInequalsRhsInt32 { dst, lhs, rhs, .. }
        | I::LooselyEqualsRhsInt32 { dst, lhs, rhs, .. }
        | I::LooselyInequalsRhsInt32 { dst, lhs, rhs, .. } => Some((*dst, Slot(*lhs), Int32(*rhs))),
        I::AddLhsInt32 { dst, lhs, rhs, .. } => Some((*dst, Int32(*lhs), Slot(*rhs))),
        _ => None,
    }
}

/// The operands and targets of a comparison jump slow path call.
pub fn jump_operands(instruction: &Instruction) -> Option<(ValueSource, ValueSource, u32, u32)> {
    use Instruction as I;
    use ValueSource::Int32;
    use ValueSource::Slot;
    match instruction {
        I::JumpLessThan {
            lhs,
            rhs,
            true_target,
            false_target,
            ..
        }
        | I::JumpGreaterThan {
            lhs,
            rhs,
            true_target,
            false_target,
            ..
        }
        | I::JumpLessThanEquals {
            lhs,
            rhs,
            true_target,
            false_target,
            ..
        }
        | I::JumpGreaterThanEquals {
            lhs,
            rhs,
            true_target,
            false_target,
            ..
        }
        | I::JumpLooselyEquals {
            lhs,
            rhs,
            true_target,
            false_target,
            ..
        }
        | I::JumpLooselyInequals {
            lhs,
            rhs,
            true_target,
            false_target,
            ..
        }
        | I::JumpStrictlyEquals {
            lhs,
            rhs,
            true_target,
            false_target,
            ..
        }
        | I::JumpStrictlyInequals {
            lhs,
            rhs,
            true_target,
            false_target,
            ..
        }
        | I::JumpLessThanLoop {
            lhs,
            rhs,
            true_target,
            false_target,
            ..
        }
        | I::JumpGreaterThanLoop {
            lhs,
            rhs,
            true_target,
            false_target,
            ..
        }
        | I::JumpLessThanEqualsLoop {
            lhs,
            rhs,
            true_target,
            false_target,
            ..
        }
        | I::JumpGreaterThanEqualsLoop {
            lhs,
            rhs,
            true_target,
            false_target,
            ..
        }
        | I::JumpLooselyEqualsLoop {
            lhs,
            rhs,
            true_target,
            false_target,
            ..
        }
        | I::JumpLooselyInequalsLoop {
            lhs,
            rhs,
            true_target,
            false_target,
            ..
        }
        | I::JumpStrictlyEqualsLoop {
            lhs,
            rhs,
            true_target,
            false_target,
            ..
        }
        | I::JumpStrictlyInequalsLoop {
            lhs,
            rhs,
            true_target,
            false_target,
            ..
        } => Some((Slot(*lhs), Slot(*rhs), true_target.0, false_target.0)),
        I::JumpLessThanRhsInt32 {
            lhs,
            rhs,
            true_target,
            false_target,
            ..
        }
        | I::JumpLessThanEqualsRhsInt32 {
            lhs,
            rhs,
            true_target,
            false_target,
            ..
        }
        | I::JumpGreaterThanRhsInt32 {
            lhs,
            rhs,
            true_target,
            false_target,
            ..
        }
        | I::JumpGreaterThanEqualsRhsInt32 {
            lhs,
            rhs,
            true_target,
            false_target,
            ..
        }
        | I::JumpStrictlyEqualsRhsInt32 {
            lhs,
            rhs,
            true_target,
            false_target,
            ..
        }
        | I::JumpStrictlyInequalsRhsInt32 {
            lhs,
            rhs,
            true_target,
            false_target,
            ..
        }
        | I::JumpLooselyEqualsRhsInt32 {
            lhs,
            rhs,
            true_target,
            false_target,
            ..
        }
        | I::JumpLooselyInequalsRhsInt32 {
            lhs,
            rhs,
            true_target,
            false_target,
            ..
        }
        | I::JumpLessThanLoopRhsInt32 {
            lhs,
            rhs,
            true_target,
            false_target,
            ..
        }
        | I::JumpLessThanEqualsLoopRhsInt32 {
            lhs,
            rhs,
            true_target,
            false_target,
            ..
        }
        | I::JumpGreaterThanLoopRhsInt32 {
            lhs,
            rhs,
            true_target,
            false_target,
            ..
        }
        | I::JumpGreaterThanEqualsLoopRhsInt32 {
            lhs,
            rhs,
            true_target,
            false_target,
            ..
        }
        | I::JumpStrictlyEqualsLoopRhsInt32 {
            lhs,
            rhs,
            true_target,
            false_target,
            ..
        }
        | I::JumpStrictlyInequalsLoopRhsInt32 {
            lhs,
            rhs,
            true_target,
            false_target,
            ..
        }
        | I::JumpLooselyEqualsLoopRhsInt32 {
            lhs,
            rhs,
            true_target,
            false_target,
            ..
        }
        | I::JumpLooselyInequalsLoopRhsInt32 {
            lhs,
            rhs,
            true_target,
            false_target,
            ..
        } => Some((Slot(*lhs), Int32(*rhs), true_target.0, false_target.0)),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bytecode::NUM_OPCODES;

    #[test]
    fn every_generic_opcode_has_a_slow_path() {
        for value in 0..NUM_OPCODES {
            let opcode = OpCode::from_u8(u8::try_from(value).unwrap()).unwrap();
            let Some(call) = slow_path_call(opcode) else {
                continue;
            };
            let symbol = slow_path_symbol(opcode);
            assert_eq!(symbol.is_none(), call == SlowPathCall::JitCall, "{}", opcode.name());
        }
        assert_eq!(
            slow_path_symbol(OpCode::JumpLessThanEqualsRhsInt32).unwrap(),
            "asm_slow_path_jump_less_than_equals_values"
        );
        assert_eq!(
            slow_path_symbol(OpCode::JumpLessThanEqualsLoopRhsInt32).unwrap(),
            "asm_slow_path_jump_less_than_equals_values"
        );
        assert_eq!(
            slow_path_symbol(OpCode::AddLhsInt32).unwrap(),
            "asm_slow_path_add_values"
        );
        assert_eq!(
            slow_path_symbol(OpCode::GetById).unwrap(),
            "asm_slow_path_get_by_id_from_jit"
        );
        assert_eq!(
            slow_path_symbol(OpCode::ThrowIfTDZ).unwrap(),
            "asm_slow_path_throw_if_tdz"
        );
    }
}
