/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The IR verifier: checks the invariants every phase relies on. It runs under `verify-ir` and in tests.

use super::{Effect, Func, NONE, Op, Shape, Type, Value};

/// The immediate dominator of each block, with the entry its own. Linear order is a topological order of the
/// forward edges, so one pass over the blocks in it finds them; loops add back edges only.
pub fn immediate_dominators(func: &Func) -> Vec<u32> {
    let mut idom = vec![NONE; func.blocks.len()];
    if func.blocks.is_empty() {
        return idom;
    }
    idom[0] = 0;
    for block in 1..func.blocks.len() as u32 {
        let mut dominator = NONE;
        for &predecessor in func.predecessors(block) {
            if predecessor >= block || idom[predecessor as usize] == NONE {
                continue;
            }
            dominator = if dominator == NONE {
                predecessor
            } else {
                intersect(&idom, dominator, predecessor)
            };
        }
        idom[block as usize] = dominator;
    }
    idom
}

fn intersect(idom: &[u32], mut a: u32, mut b: u32) -> u32 {
    while a != b {
        while a > b {
            a = idom[a as usize];
        }
        while b > a {
            b = idom[b as usize];
        }
    }
    a
}

/// Whether block `a` dominates block `b`.
pub fn dominates(idom: &[u32], a: u32, mut b: u32) -> bool {
    loop {
        if a == b {
            return true;
        }
        if b == 0 || idom[b as usize] == NONE || b < a {
            return false;
        }
        b = idom[b as usize];
    }
}

/// Checks the function. `after_cleanup` requires that no forwarded phi is used.
pub fn verify(func: &Func, after_cleanup: bool) -> Result<(), String> {
    let mut errors = Vec::new();
    let mut error = |message: String| {
        if errors.len() < 20 {
            errors.push(message);
        }
    };
    let idom = immediate_dominators(func);
    // The block of every body node and phi, to check dominance.
    let mut block_of = vec![NONE; func.nodes.len()];
    for (index, block) in func.blocks.iter().enumerate() {
        let block_id = index as u32;
        if block.start >= block.end {
            error(format!("b{index} has no terminator"));
            continue;
        }
        if index > 0 && idom[index] == NONE {
            error(format!("b{index} is unreachable"));
        }
        for value in block.start..block.end {
            let op = func.node(value).op;
            if matches!(op, Op::Phi | Op::Forward) || op.is_entry_value() {
                continue;
            }
            block_of[value as usize] = block_id;
            if op.is_terminator() != (value == block.end - 1) {
                error(format!("v{value} in b{index}: terminators end blocks and only they do"));
            }
        }
        for phi in func.phis(block_id) {
            block_of[phi as usize] = block_id;
            let node = func.node(phi);
            if node.op == Op::Phi && node.args[1] as usize != func.predecessors(block_id).len() {
                error(format!(
                    "phi v{phi} of b{index} has {} inputs for {} predecessors",
                    node.args[1],
                    func.predecessors(block_id).len()
                ));
            }
        }
        for &successor in func.successors(block_id) {
            if successor as usize >= func.blocks.len() {
                error(format!("b{index} has an unknown successor b{successor}"));
                continue;
            }
            let count = func.successors(block_id).iter().filter(|s| **s == successor).count();
            let back = func.predecessors(successor).iter().filter(|p| **p == block_id).count();
            if count != 1 || back != 1 {
                error(format!(
                    "the edge b{index} -> b{successor} is not listed once on each side"
                ));
            }
        }
        for &predecessor in func.predecessors(block_id) {
            if !func.successors(predecessor).contains(&block_id) {
                error(format!(
                    "b{index} lists b{predecessor} as a predecessor, which does not branch to it"
                ));
            }
        }
        let terminator = func.terminator(block_id);
        let expected_successors = match func.node(terminator).op {
            Op::Jump => Some(1),
            Op::Branch | Op::BranchCompare | Op::BranchFloatCompare => Some(2),
            Op::Return | Op::Trap | Op::TailCall | Op::Throw => Some(0),
            _ => None,
        };
        if let Some(expected) = expected_successors
            && func.successors(block_id).len() != expected
        {
            error(format!("b{index} has {} successors", func.successors(block_id).len()));
        }
    }

    let defined_before = |input: Value, block: u32, position: Value| -> bool {
        let node = func.node(input);
        if node.op.is_entry_value() {
            return true;
        }
        let definition = block_of[input as usize];
        if definition == NONE {
            return false;
        }
        if definition == block {
            return node.op == Op::Phi || input < position;
        }
        dominates(&idom, definition, block)
    };
    for (index, block) in func.blocks.iter().enumerate() {
        let block_id = index as u32;
        for value in (block.start..block.end).chain(func.phis(block_id)) {
            let node = func.node(value);
            if node.op.is_entry_value() || block_of[value as usize] != block_id {
                continue;
            }
            if after_cleanup && node.op == Op::Forward {
                continue;
            }
            for (input_index, &input) in func.inputs(value).iter().enumerate() {
                if input as usize >= func.nodes.len() {
                    error(format!("v{value} uses an unknown value {input}"));
                    continue;
                }
                let mut input = input;
                while !after_cleanup && func.node(input).op == Op::Forward {
                    input = func.node(input).args[0];
                }
                let input_node = func.node(input);
                if input_node.ty == Type::None {
                    error(format!("v{value} uses v{input}, which has no value"));
                }
                if after_cleanup && input_node.op == Op::Forward {
                    error(format!("v{value} uses the forwarded phi v{input}"));
                }
                let (use_block, position) = if node.op == Op::Phi {
                    let predecessor = func.predecessors(block_id)[input_index];
                    (predecessor, func.blocks[predecessor as usize].end)
                } else {
                    (block_id, value)
                };
                if !defined_before(input, use_block, position) {
                    error(format!(
                        "v{value} in b{index} uses v{input}, which does not dominate it"
                    ));
                }
            }
            check_types(func, value, &mut error);
        }
    }
    if errors.is_empty() {
        Ok(())
    } else {
        Err(errors.join("\n"))
    }
}

fn check_types(func: &Func, value: Value, error: &mut impl FnMut(String)) {
    let node = func.node(value);
    let inputs = func.inputs(value);
    let input_type = |index: usize| func.node(inputs[index]).ty;
    let same = |ty: Type| inputs.iter().all(|input| func.node(*input).ty == ty);
    let ok = match node.op {
        Op::Add
        | Op::Sub
        | Op::Mul
        | Op::And
        | Op::Or
        | Op::Xor
        | Op::Shl
        | Op::ShrS
        | Op::ShrU
        | Op::Rotl
        | Op::Rotr
        | Op::DivS
        | Op::DivU
        | Op::RemS
        | Op::RemU
        | Op::Clz
        | Op::Ctz
        | Op::Popcnt
        | Op::Extend8S
        | Op::Extend16S
        | Op::Extend32S => matches!(node.ty, Type::I32 | Type::I64) && same(node.ty),
        Op::Eqz => node.ty == Type::I32 && matches!(input_type(0), Type::I32 | Type::I64 | Type::Ref),
        Op::Compare | Op::BranchCompare => {
            matches!(input_type(0), Type::I32 | Type::I64 | Type::Ref | Type::Pointer) && input_type(0) == input_type(1)
        }
        Op::FAdd
        | Op::FSub
        | Op::FMul
        | Op::FDiv
        | Op::FMin
        | Op::FMax
        | Op::FCopysign
        | Op::FAbs
        | Op::FNeg
        | Op::FSqrt
        | Op::FCeil
        | Op::FFloor
        | Op::FTrunc
        | Op::FNearest => node.ty.is_float() && same(node.ty),
        Op::FloatCompare | Op::BranchFloatCompare => input_type(0).is_float() && input_type(0) == input_type(1),
        Op::ExtendI32S | Op::ExtendI32U => node.ty == Type::I64 && input_type(0) == Type::I32,
        Op::Wrap => node.ty == Type::I32 && input_type(0) == Type::I64,
        Op::Select => input_type(0) == Type::I32 && input_type(1) == node.ty && input_type(2) == node.ty,
        Op::Phi => same(node.ty),
        // NB: GC objects' fields are accessed at the object's reference.
        Op::Load | Op::Store => matches!(input_type(0), Type::Pointer | Type::Ref) && input_type(1) == Type::I32,
        Op::Branch => input_type(0) == Type::I32,
        Op::BrTable => input_type(0) == Type::I32,
        Op::InstanceField | Op::LoadPointer | Op::GlobalGet | Op::GlobalSet | Op::Offset | Op::FrameLoad => {
            input_type(0) == Type::Pointer
        }
        // NB: References to runtime objects, like exceptions and GC objects, point at them.
        Op::ReadField | Op::WriteField => matches!(input_type(0), Type::Pointer | Type::Ref),
        Op::CheckedAddress => {
            input_type(0) == Type::Pointer
                && matches!(input_type(1), Type::I32 | Type::I64)
                && input_type(2) == Type::I64
                && node.ty == Type::Pointer
        }
        Op::AtomicRmw | Op::AtomicCompareExchange => {
            input_type(0) == Type::Pointer
                && matches!(node.ty, Type::I32 | Type::I64)
                && inputs[1..].iter().all(|input| func.node(*input).ty == node.ty)
        }
        Op::Rtt => input_type(0) == Type::Pointer && node.ty == Type::Pointer,
        Op::I31New => input_type(0) == Type::I32 && node.ty == Type::Ref,
        Op::I31Get => input_type(0) == Type::Ref && node.ty == Type::I32,
        Op::ElementAddress => input_type(0) == Type::Ref && input_type(1) == Type::I32 && node.ty == Type::Pointer,
        Op::RefTest => input_type(0) == Type::Ref && input_type(1) == Type::I32,
        Op::TableElement => input_type(0) == Type::Pointer && node.ty == Type::Ref,
        Op::TableStore => input_type(0) == Type::Pointer && input_type(2) == Type::Ref,
        Op::CheckFunction => input_type(0) == Type::Ref && input_type(1) == Type::I32,
        Op::CheckNull => input_type(0) == Type::Ref,
        Op::CalleeInstance => matches!(input_type(0), Type::Ref | Type::Pointer) && input_type(1) == Type::Pointer,
        Op::Call | Op::TailCall => {
            matches!(input_type(0), Type::Ref | Type::Pointer)
                && input_type(1) == Type::Pointer
                && input_type(2) == Type::Pointer
        }
        Op::Helper => input_type(0) == Type::Pointer && inputs.len() <= 7,
        _ => true,
    };
    if !ok {
        error(format!("v{value} ({}) has inputs of the wrong types", node.op.name()));
    }
    if node.op.shape() == Shape::Variadic && node.op != Op::Phi && node.effect() == Effect::Pure {
        error(format!("v{value} is a pure variadic node"));
    }
}
