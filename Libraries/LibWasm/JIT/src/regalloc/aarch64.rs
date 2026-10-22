/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The constraints of nodes on AArch64. Instructions have three operands and no fixed registers, and code generation
//! puts any constant that does not fit an immediate into a scratch register, so constants may be inputs anywhere a
//! lowering takes one.

use super::{Constraints, Input, Output, Temp};
use crate::ir::{Func, Op, Type, Value};

/// Whether constant `value` can be an input of its own: any scalar constant can.
pub fn fits_immediate(func: &Func, value: Value) -> bool {
    func.node(value).constant().is_some()
}

pub(super) fn constraints(func: &Func, value: Value) -> Constraints {
    let node = func.node(value);
    let output = if node.ty == Type::None {
        Output::None
    } else {
        Output::Register
    };
    let mut constraints = Constraints {
        inputs: [Input::Register; 3],
        output,
        temps: [Temp::None; 2],
        clobbers: 0,
        reuse_first: false,
        slot_inputs: 0,
    };
    match node.op {
        Op::Add
        | Op::Sub
        | Op::And
        | Op::Or
        | Op::Xor
        | Op::Mul
        | Op::Shl
        | Op::ShrS
        | Op::ShrU
        | Op::Rotl
        | Op::Rotr
        | Op::Compare
        | Op::BranchCompare
        | Op::Load
        | Op::VectorLoad
        | Op::GlobalSet
        | Op::WriteField
        | Op::RefTest => constraints.inputs[1] = Input::RegisterOrConstant,
        Op::Store => {
            constraints.inputs[1] = Input::RegisterOrConstant;
            constraints.inputs[2] = Input::RegisterOrConstant;
        }
        Op::CheckFunction => constraints.temps = [Temp::Gpr, Temp::Gpr],
        Op::CheckedAddress => {
            let size = 1u64 << (node.flags & 7);
            let offset = func.node(node.args[2]).constant();
            // NB: A memory64's offset can be close enough to 2^64 that adding the size wraps around.
            if offset
                .and_then(|offset| offset.checked_add(size))
                .is_some_and(|end| end <= i32::MAX as u64)
            {
                constraints.inputs[2] = Input::RegisterOrConstant;
            }
        }
        _ => {}
    }
    if node.op == Op::RefTest {
        constraints.temps = match crate::ir::HeapTest::from_flags(node.flags) {
            crate::ir::HeapTest::Struct | crate::ir::HeapTest::Array | crate::ir::HeapTest::ConcreteObject => {
                [Temp::Gpr, Temp::None]
            }
            crate::ir::HeapTest::ConcreteFunction => [Temp::Gpr, Temp::Gpr],
            _ => [Temp::None, Temp::None],
        };
    }
    if matches!(
        node.op,
        Op::Shuffle | Op::VectorUnary | Op::VectorBinary | Op::VectorTernary
    ) {
        let temps = crate::codegen::aarch64::vector_temps(func, value);
        for temp in constraints.temps.iter_mut().take(temps) {
            *temp = Temp::Fpr;
        }
        if node.op == Op::VectorBinary && crate::codegen::aarch64::is_vector_shift(node.args[2]) {
            constraints.inputs[1] = Input::RegisterOrConstant;
        }
    }
    constraints
}
