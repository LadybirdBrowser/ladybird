/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Dead code elimination: removes nodes whose values nobody uses, as long
//! as running them has no effect beyond producing their value. Frame states
//! count as uses, since exits need their values. Branches both of whose
//! edges lead to the same place, doing nothing, become jumps.

use super::edit;
use crate::bitset::BitSet;
use crate::ir::Graph;
use crate::ir::NodeId;
use crate::ir::Op;

/// Whether a node can be dropped when its value is unused.
fn is_removable(op: &Op) -> bool {
    if *op == Op::Phi {
        return true;
    }
    let properties = op.properties();
    !op.is_control()
        && !op.can_eager_exit()
        && !properties.can_lazy_exit
        && !properties.is_call
        && !properties.allocates
        && properties.writes.is_empty()
        && !properties.runs_code
}

pub fn run(graph: &mut Graph) {
    remove_dead_nodes(graph);
    // NB: Branches whose edges did the same are left jumping where they go,
    //     which leaves what only they used dead.
    if edit::fold_branches_to_one_target(graph) {
        remove_dead_nodes(graph);
    }
}

fn remove_dead_nodes(graph: &mut Graph) {
    let count = graph.nodes.len();
    let mut uses = vec![0u32; count];
    let mut placed = BitSet::new(count);
    for block in &graph.blocks {
        for node in block.nodes() {
            placed.insert(node.index());
        }
    }
    let count_uses = |node: usize, uses: &mut Vec<u32>, delta: i32| {
        for value in graph.used_values(NodeId::from_index(node)) {
            // NB: A phi using itself does not keep itself alive.
            if value.index() != node {
                uses[value.index()] = uses[value.index()]
                    .checked_add_signed(delta)
                    .expect("use counts stay positive");
            }
        }
    };
    for node in placed.iter() {
        count_uses(node, &mut uses, 1);
    }

    let mut removed = BitSet::new(count);
    let mut pending = placed
        .iter()
        .filter(|node| uses[*node] == 0 && graph.nodes[*node].repr.is_some())
        .collect::<Vec<_>>();
    while let Some(node) = pending.pop() {
        if removed.contains(node) || uses[node] != 0 || !is_removable(&graph.nodes[node].op) {
            continue;
        }
        removed.insert(node);
        count_uses(node, &mut uses, -1);
        for input in &graph.nodes[node].inputs {
            if uses[input.index()] == 0 && placed.contains(input.index()) {
                pending.push(input.index());
            }
        }
    }
    edit::remove_nodes(graph, &removed);
}
