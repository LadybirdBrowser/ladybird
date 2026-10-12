/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Exit unboxing: frame states take unboxed values instead of their boxings.
//!
//! Unboxed operations box their results for the interpreter's slots, but
//! where only exits read a boxing, it is computed (and kept in a register or
//! spilled) on the fast path for nothing: exits box int32, double and
//! boolean values themselves. Frame states then take the unboxed value,
//! which dominates its boxing and thus the exit, and dead code elimination
//! removes the boxing.

use crate::bitset::BitSet;
use crate::ir::Graph;
use crate::ir::Op;

fn is_boxing(op: &Op) -> bool {
    matches!(op, Op::BoxInt32 | Op::BoxFloat64 | Op::BoxBool)
}

pub fn run(graph: &mut Graph) {
    // NB: Boxings that other nodes use stay in frame states, where they
    //     cost nothing more than the unboxed value would.
    let mut used = BitSet::new(graph.nodes.len());
    for block in &graph.blocks {
        for node in block.nodes() {
            for input in &graph.node(node).inputs {
                used.insert(input.index());
            }
        }
    }
    let nodes = &graph.nodes;
    for frame_state in &mut graph.frame_states {
        for (_, value) in &mut frame_state.values {
            let node = &nodes[value.index()];
            if is_boxing(&node.op) && !used.contains(value.index()) {
                *value = node.inputs[0];
            }
        }
    }
}
