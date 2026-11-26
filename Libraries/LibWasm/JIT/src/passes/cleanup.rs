/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Cleanup after building: removes the phis that turned out trivial, to a fixed point, rewrites every use of a
//! forwarded phi, removes pure nodes nobody uses, and fuses branches and integer selects with the comparisons only
//! they use.

use crate::ir::{Effect, Func, IntCondition, NONE, Node, Op, SELECT_TEST, Shape, Type, Value};

/// What cleanup leaves for the later phases: which nodes are gone, and how often each value is used.
#[derive(Default)]
pub struct Liveliness {
    pub dead: Vec<bool>,
    pub uses: Vec<u32>,
    worklist: Vec<Value>,
}

impl Liveliness {
    pub fn is_dead(&self, value: Value) -> bool {
        self.dead[value as usize]
    }
}

pub fn cleanup(func: &mut Func, state: &mut Liveliness) {
    // Trivial phis, to a fixed point over the phis only.
    loop {
        let mut changed = false;
        for block in 0..func.blocks.len() as u32 {
            let mut phi = func.blocks[block as usize].first_phi;
            while phi != NONE {
                let next = func.nodes[phi as usize].args[2];
                if func.nodes[phi as usize].op == Op::Phi
                    && let Some(value) = func.trivial_phi_input(phi)
                {
                    func.forward(phi, value);
                    changed = true;
                }
                phi = next;
            }
        }
        if !changed {
            break;
        }
    }
    // Every use of a forwarded phi becomes a use of its value, and every use is counted.
    let count = func.nodes.len();
    state.dead.clear();
    state.dead.resize(count, false);
    state.uses.clear();
    state.uses.resize(count, 0);
    for index in 0..count {
        let node = func.nodes[index];
        if node.op == Op::Forward {
            state.dead[index] = true;
            continue;
        }
        match node.op.shape() {
            Shape::Fixed(inputs) => {
                for slot in 0..inputs as usize {
                    let input = func.resolve(node.args[slot]);
                    func.nodes[index].args[slot] = input;
                    state.uses[input as usize] += 1;
                }
            }
            Shape::Variadic => {
                let start = node.args[0] as usize;
                for slot in start..start + node.args[1] as usize {
                    let input = func.resolve(func.operands[slot]);
                    func.operands[slot] = input;
                    state.uses[input as usize] += 1;
                }
            }
        }
    }

    // Dead code: pure nodes without uses.
    state.worklist.clear();
    for index in 0..count {
        // NB: Unused results of calls stay, since they tell how the call returns its results.
        let node = &func.nodes[index];
        if state.uses[index] == 0 && !state.dead[index] && node.effect() == Effect::Pure && node.op != Op::CallResult {
            state.worklist.push(index as Value);
        }
    }
    while let Some(value) = state.worklist.pop() {
        if state.dead[value as usize] {
            continue;
        }
        state.dead[value as usize] = true;
        for input_index in 0..func.inputs(value).len() {
            let input = func.inputs(value)[input_index];
            let uses = &mut state.uses[input as usize];
            *uses -= 1;
            let input_node = &func.nodes[input as usize];
            if *uses == 0 && input_node.effect() == Effect::Pure && input_node.op != Op::CallResult {
                state.worklist.push(input);
            }
        }
    }

    // Forwarded and dead phis leave their blocks.
    for block in 0..func.blocks.len() {
        let mut previous = NONE;
        let mut phi = func.blocks[block].first_phi;
        while phi != NONE {
            let next = func.nodes[phi as usize].args[2];
            if state.dead[phi as usize] {
                if previous == NONE {
                    func.blocks[block].first_phi = next;
                } else {
                    func.nodes[previous as usize].args[2] = next;
                }
            } else {
                previous = phi;
            }
            phi = next;
        }
    }

    fuse_branches(func, state);
    fuse_selects(func, state);
}

/// A select of integers on a comparison, an `eqz` or an `and` that only selects use, in the same block, compares (or
/// tests) itself, so that it becomes a compare and a conditional move.
fn fuse_selects(func: &mut Func, state: &mut Liveliness) {
    let fusable = |func: &Func, state: &Liveliness, block_start: u32, value: usize| {
        let node = func.nodes[value];
        let condition = node.args[0];
        node.op == Op::Select
            && !state.dead[value]
            && !node.ty.uses_xmm()
            && (block_start..value as Value).contains(&condition)
            && matches!(func.nodes[condition as usize].op, Op::Compare | Op::And | Op::Eqz)
    };
    // NB: `worklist` counts the selects of each condition here.
    let count = func.nodes.len();
    state.worklist.clear();
    state.worklist.resize(count, 0);
    for block in &func.blocks {
        for value in block.start as usize..block.end as usize {
            if fusable(func, state, block.start, value) {
                state.worklist[func.nodes[value].args[0] as usize] += 1;
            }
        }
    }
    for block in 0..func.blocks.len() {
        let block_start = func.blocks[block].start;
        for value in block_start as usize..func.blocks[block].end as usize {
            if fusable(func, state, block_start, value) {
                fuse_select(func, state, block_start, value);
            }
        }
    }
    state.worklist.clear();
}

fn fuse_select(func: &mut Func, state: &mut Liveliness, block_start: Value, value: usize) {
    {
        let node = func.nodes[value];
        let [condition, if_true, if_false] = node.args;
        if state.worklist[condition as usize] != state.uses[condition as usize] {
            return;
        }
        let condition_node = func.nodes[condition as usize];
        let mut tested_and = NONE;
        let (flags, lhs, rhs) = match condition_node.op {
            Op::Compare => (condition_node.flags, condition_node.args[0], condition_node.args[1]),
            Op::And => (
                SELECT_TEST | IntCondition::Ne as u8,
                condition_node.args[0],
                condition_node.args[1],
            ),
            _ => {
                let operand = condition_node.args[0];
                let operand_node = func.nodes[operand as usize];
                // NB: An `and` from another block stays, so that only its result lives up to the select.
                if operand_node.op == Op::And
                    && state.uses[operand as usize] == 1
                    && (block_start..condition).contains(&operand)
                {
                    tested_and = operand;
                    (
                        SELECT_TEST | IntCondition::Eq as u8,
                        operand_node.args[0],
                        operand_node.args[1],
                    )
                } else {
                    let zero = if operand_node.ty.is_64() {
                        Node::new(Op::Const64, operand_node.ty, 0, [0, 0, 0])
                    } else {
                        Node::new(Op::Const32, Type::I32, 0, [0, 0, 0])
                    };
                    func.nodes.push(zero);
                    state.dead.push(false);
                    state.uses.push(0);
                    (IntCondition::Eq as u8, operand, func.nodes.len() as Value - 1)
                }
            }
        };
        let start = func.operands.len() as Value;
        func.operands.extend_from_slice(&[if_false, lhs, rhs, if_true]);
        func.nodes[value] = Node::new(Op::SelectCompare, node.ty, flags, [start, 4, 0]);
        state.uses[lhs as usize] += 1;
        state.uses[rhs as usize] += 1;
        // The condition goes once its last select reads its inputs instead.
        state.uses[condition as usize] -= 1;
        state.worklist[condition as usize] -= 1;
        if state.uses[condition as usize] == 0 {
            state.dead[condition as usize] = true;
            let dying = if tested_and != NONE {
                state.dead[tested_and as usize] = true;
                state.uses[tested_and as usize] = 0;
                tested_and
            } else {
                condition
            };
            for index in 0..func.inputs(dying).len() {
                let input = func.inputs(dying)[index];
                state.uses[input as usize] -= 1;
            }
        }
    }
}

/// A branch on a comparison that only it uses, in the same block, compares itself.
fn fuse_branches(func: &mut Func, state: &mut Liveliness) {
    for block in 0..func.blocks.len() {
        let terminator = func.blocks[block].end - 1;
        let node = func.nodes[terminator as usize];
        if node.op != Op::Branch {
            continue;
        }
        let condition = node.args[0];
        let condition_node = func.nodes[condition as usize];
        let in_block = condition >= func.blocks[block].start && condition < terminator;
        if !in_block || state.uses[condition as usize] != 1 {
            continue;
        }
        let fused = match condition_node.op {
            Op::Compare => Node::new(
                Op::BranchCompare,
                Type::None,
                condition_node.flags,
                [condition_node.args[0], condition_node.args[1], 0],
            ),
            Op::FloatCompare => Node::new(
                Op::BranchFloatCompare,
                Type::None,
                condition_node.flags,
                [condition_node.args[0], condition_node.args[1], 0],
            ),
            Op::Eqz => {
                let operand = condition_node.args[0];
                let ty = func.nodes[operand as usize].ty;
                let zero = if ty.is_64() {
                    Node::new(Op::Const64, ty, 0, [0, 0, 0])
                } else {
                    Node::new(Op::Const32, Type::I32, 0, [0, 0, 0])
                };
                func.nodes.push(zero);
                state.dead.push(false);
                state.uses.push(1);
                let zero = func.nodes.len() as Value - 1;
                Node::new(
                    Op::BranchCompare,
                    Type::None,
                    IntCondition::Eq as u8,
                    [operand, zero, 0],
                )
            }
            _ => continue,
        };
        func.nodes[terminator as usize] = fused;
        // The comparison's inputs are now the branch's.
        state.dead[condition as usize] = true;
        state.uses[condition as usize] = 0;
    }
}
