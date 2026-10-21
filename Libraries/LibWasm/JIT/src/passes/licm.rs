/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Loop invariant code motion: finds the nodes of loops that compute the same value in every iteration, and the
//! outermost loop each one is invariant in. `schedule` then moves them to the end of that loop's preheader: the
//! block entering the loop, which always ends in a jump to its header.
//!
//! A pure node is invariant in a loop when none of its inputs is defined in the loop (or each was hoisted out of
//! it). Pure nodes never trap, so computing them where the loop might not have needs no proof that it would have.
//! A global's value is invariant in a loop that does not set it. Memory loads stay where they are, since moving one
//! out of a loop that would not have run it could trap.
//!
//! OPTIMIZATION: Nothing leaves a loop that calls: calls clobber every register, so a hoisted value would be reloaded
//! from its stack slot in every iteration, which costs about what computing it again does. In a loop that calls a
//! leaf 10 million times, hoisting the callee's `FunctionHeader` made it 16% slower.

use crate::ir::{BlockId, Effect, FLAG_IMMUTABLE, Func, NONE, Op, Value};
use crate::passes::cleanup::Liveliness;

#[derive(Default)]
pub struct Licm {
    /// The block each value moves to, or NONE.
    pub home: Vec<BlockId>,
    /// The block of each body node.
    block_of: Vec<BlockId>,
    /// Per loop: whether it calls, and the globals it sets.
    calls: Vec<bool>,
    globals_set: Vec<Vec<u32>>,
    /// How many nodes moved.
    pub hoisted: u32,
}

impl Licm {
    pub fn run(&mut self, func: &Func, liveliness: &Liveliness) {
        let count = func.nodes.len();
        self.home.clear();
        self.home.resize(count, NONE);
        self.hoisted = 0;
        if func.loops.is_empty() {
            return;
        }
        self.block_of.clear();
        self.block_of.resize(count, 0);
        self.calls.clear();
        self.calls.resize(func.loops.len(), false);
        for set in &mut self.globals_set {
            set.clear();
        }
        self.globals_set.resize_with(func.loops.len(), Vec::new);
        for (index, block) in func.blocks.iter().enumerate() {
            for value in block.start..block.end {
                self.block_of[value as usize] = index as BlockId;
                let node = func.node(value);
                if liveliness.is_dead(value) || block.loop_index == NONE {
                    continue;
                }
                let global = match node.op {
                    Op::Call | Op::Helper => None,
                    Op::GlobalSet => Some(node.args[2]),
                    _ => continue,
                };
                let mut loop_index = block.loop_index;
                while loop_index != NONE {
                    match global {
                        None => self.calls[loop_index as usize] = true,
                        Some(global) => self.globals_set[loop_index as usize].push(global),
                    }
                    loop_index = func.loops[loop_index as usize].parent;
                }
            }
        }
        for phi_block in 0..func.blocks.len() as BlockId {
            for phi in func.phis(phi_block) {
                self.block_of[phi as usize] = phi_block;
            }
        }
        for block in &func.blocks {
            if block.loop_index == NONE {
                continue;
            }
            for value in block.start..block.end - 1 {
                let node = func.node(value);
                if liveliness.is_dead(value) || node.op.is_entry_value() || matches!(node.op, Op::Phi | Op::Forward) {
                    continue;
                }
                let global = match node.effect() {
                    Effect::Pure if node.op != Op::CallResult => None,
                    Effect::Read if node.op == Op::GlobalGet && node.flags & FLAG_IMMUTABLE == 0 => Some(node.args[1]),
                    _ => continue,
                };
                let mut target = NONE;
                let mut loop_index = block.loop_index;
                while loop_index != NONE {
                    let loop_ = &func.loops[loop_index as usize];
                    let outside = |input: &Value| {
                        let input_node = func.node(*input);
                        if input_node.op.is_entry_value() {
                            return true;
                        }
                        let home = self.home[*input as usize];
                        let block = if home == NONE {
                            self.block_of[*input as usize]
                        } else {
                            home
                        };
                        block < loop_.header || block > loop_.last
                    };
                    let unwritten =
                        global.is_none_or(|global| !self.globals_set[loop_index as usize].contains(&global));
                    if self.calls[loop_index as usize] || !unwritten || !func.inputs(value).iter().all(outside) {
                        break;
                    }
                    target = loop_index;
                    loop_index = loop_.parent;
                }
                if target != NONE {
                    let header = func.loops[target as usize].header;
                    self.home[value as usize] = func.predecessors(header)[0];
                    self.hoisted += 1;
                }
            }
        }
    }
}
