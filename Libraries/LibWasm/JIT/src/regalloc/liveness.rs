/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Liveness as one interval per value in linear order, from its definition to its last use, extended over every
//! loop that uses it but does not define it.
//!
//! Linear order keeps every loop contiguous, and every definition dominates the positions between it and its uses,
//! so an interval is a sound approximation of where the value is live: it only adds the arms of `if`s and the
//! like that come between a definition and a use without leading to it (the "holes" of an interval). Loops are
//! entered only at their header, so a value used in a loop and defined before it is live in all of it.
//!
//! Positions: a body node `n` is at `4n + 2`, which is where it reads its inputs and writes its value. The phis of a
//! block are defined at `4 * start`, before its first node. The moves into a successor's phis happen at
//! `4 * end - 1`, after the block's terminator, so that the phis' inputs are still where the terminator left them.
//! Parameters, the instance and the memory base are defined at position 0.

use crate::convention::{self, Place, Register};
use crate::ir::{BlockId, Func, NONE, Op, Type, Value};

pub fn node_position(value: Value) -> u32 {
    4 * value + 2
}

pub fn block_start_position(func: &Func, block: BlockId) -> u32 {
    4 * func.blocks[block as usize].start
}

pub fn edge_position(func: &Func, block: BlockId) -> u32 {
    4 * func.blocks[block as usize].end - 1
}

#[derive(Default)]
pub struct Liveness {
    /// Where each value is defined, or NONE for values that need no location (constants, dead nodes).
    pub definition: Vec<u32>,
    /// The last position each value is live at.
    pub end: Vec<u32>,
    /// The block each value is defined in.
    pub block: Vec<BlockId>,
    /// The use positions of value `v`, in order: `positions[starts[v]..starts[v + 1]]`.
    pub starts: Vec<u32>,
    pub positions: Vec<u32>,
    /// The register a value's first fixed use wants, or NONE as u8.
    pub hint: Vec<u8>,
    /// The last position of each loop, with its back edges' moves.
    pub loop_ends: Vec<u32>,
    cursor: Vec<u32>,
    places: Vec<Place>,
}

pub const NO_HINT: u8 = u8::MAX;

/// Whether a node is a value the register allocator gives a location.
#[inline]
pub fn is_allocated(func: &Func, value: Value) -> bool {
    let node = func.node(value);
    node.ty != Type::None && !node.op.is_constant()
}

impl Liveness {
    pub fn uses(&self, value: Value) -> &[u32] {
        let start = self.starts[value as usize] as usize;
        let end = self.starts[value as usize + 1] as usize;
        &self.positions[start..end]
    }

    /// Whether `value` is live at `position`, after its definition.
    #[inline]
    pub fn is_live_at(&self, value: Value, position: u32) -> bool {
        let definition = self.definition[value as usize];
        definition != NONE && definition < position && self.end[value as usize] >= position
    }

    /// Whether `value` is still needed after `position`.
    #[inline]
    pub fn is_live_after(&self, value: Value, position: u32) -> bool {
        self.end[value as usize] > position
    }

    /// The first use of `value` after `position`.
    pub fn next_use(&self, value: Value, position: u32) -> Option<u32> {
        let uses = self.uses(value);
        let index = uses.partition_point(|use_position| *use_position <= position);
        uses.get(index).copied()
    }

    pub fn compute(&mut self, func: &Func) {
        let count = func.nodes.len();
        self.definition.clear();
        self.definition.resize(count, NONE);
        self.end.clear();
        self.end.resize(count, 0);
        self.block.clear();
        self.block.resize(count, 0);
        self.hint.clear();
        self.hint.resize(count, NO_HINT);
        self.starts.clear();
        self.starts.resize(count + 1, 0);

        self.loop_ends.clear();
        for loop_ in &func.loops {
            self.loop_ends.push(edge_position(func, loop_.last));
        }

        // Definitions, and the number of uses of each value.
        let counts = &mut self.starts;
        for block in 0..func.blocks.len() as BlockId {
            let data = &func.blocks[block as usize];
            for phi in func.phis(block) {
                self.definition[phi as usize] = block_start_position(func, block);
                self.block[phi as usize] = block;
            }
            for value in data.start..data.end {
                let node = func.node(value);
                if node.op.is_entry_value() {
                    if !node.op.is_constant() && node.ty != Type::None {
                        self.definition[value as usize] = 0;
                        self.block[value as usize] = 0;
                    }
                    continue;
                }
                if node.ty != Type::None {
                    self.definition[value as usize] = node_position(value);
                    self.block[value as usize] = block;
                }
                for &input in func.inputs(value) {
                    counts[input as usize] += 1;
                }
            }
            for &successor in func.successors(block) {
                let index = predecessor_index(func, successor, block);
                for phi in func.phis(successor) {
                    counts[func.inputs(phi)[index] as usize] += 1;
                }
            }
        }
        // Entry values outside block 0's range (cleanup's constants) were skipped above; they need no location.
        let mut total = 0;
        for value in 0..=count {
            let uses = self.starts[value];
            self.starts[value] = total;
            total += uses;
        }
        self.positions.clear();
        self.positions.resize(total as usize, 0);
        self.cursor.clear();
        self.cursor.extend_from_slice(&self.starts[..count]);

        // The uses, in order, and the ends of the intervals.
        for block in 0..func.blocks.len() as BlockId {
            let data = &func.blocks[block as usize];
            for value in data.start..data.end {
                let node = func.node(value);
                if node.op.is_entry_value() {
                    continue;
                }
                let position = node_position(value);
                for &input in func.inputs(value) {
                    self.add_use(func, input, position, block);
                }
                match node.op {
                    Op::Call | Op::TailCall => self.hint_call_arguments(func, value),
                    Op::Return => self.hint_results(func, value),
                    _ => {}
                }
            }
            let position = edge_position(func, block);
            for &successor in func.successors(block) {
                let index = predecessor_index(func, successor, block);
                for phi in func.phis(successor) {
                    let input = func.inputs(phi)[index];
                    self.add_use(func, input, position, block);
                }
            }
        }
    }

    fn add_use(&mut self, func: &Func, value: Value, position: u32, block: BlockId) {
        let slot = &mut self.cursor[value as usize];
        self.positions[*slot as usize] = position;
        *slot += 1;
        let mut end = position;
        let definition_block = self.block[value as usize];
        let use_loop = func.blocks[block as usize].loop_index;
        if use_loop != NONE && use_loop != func.blocks[definition_block as usize].loop_index {
            // The outermost loop around the use that does not contain the definition keeps the value live.
            let mut loop_index = use_loop;
            while loop_index != NONE {
                let loop_ = &func.loops[loop_index as usize];
                if loop_.header <= definition_block && definition_block <= loop_.last {
                    break;
                }
                end = end.max(self.loop_ends[loop_index as usize]);
                loop_index = loop_.parent;
            }
        }
        let current = &mut self.end[value as usize];
        *current = (*current).max(end);
    }

    fn hint_call_arguments(&mut self, func: &Func, call: Value) {
        let inputs = func.inputs(call);
        let callee = &inputs[3..];
        self.places.clear();
        let (mut gprs, mut fprs) = (0, 0);
        for &argument in callee {
            let register = if func.node(argument).ty.uses_xmm() {
                fprs += 1;
                convention::FPR_ARGUMENTS.get(fprs - 1)
            } else {
                gprs += 1;
                convention::GPR_ARGUMENTS.get(gprs - 1)
            };
            if let Some(&register) = register
                && self.hint[argument as usize] == NO_HINT
            {
                self.hint[argument as usize] = register;
            }
        }
    }

    fn hint_results(&mut self, func: &Func, ret: Value) {
        let (mut gprs, mut fprs) = (0, 0);
        for &result in func.inputs(ret) {
            let register: Option<&Register> = if func.node(result).ty.uses_xmm() {
                fprs += 1;
                convention::FPR_RESULTS.get(fprs - 1)
            } else {
                gprs += 1;
                convention::GPR_RESULTS.get(gprs - 1)
            };
            if let Some(&register) = register
                && self.hint[result as usize] == NO_HINT
            {
                self.hint[result as usize] = register;
            }
        }
    }
}

/// The index of `predecessor` among the predecessors of `block`, which is the index of its phis' inputs from it.
pub fn predecessor_index(func: &Func, block: BlockId, predecessor: BlockId) -> usize {
    func.predecessors(block)
        .iter()
        .position(|candidate| *candidate == predecessor)
        .expect("successors list their predecessors")
}
