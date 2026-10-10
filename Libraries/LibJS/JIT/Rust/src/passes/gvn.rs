/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Global value numbering of pure nodes: a node that computes the same
//! operation on the same inputs as a node dominating it is replaced by that
//! node. Nodes that read memory are left to check elimination, which knows
//! when memory changes.
//!
//! Nodes that may exit qualify too: the dominating node made the same check
//! on the same values, so this one cannot fail.

use super::dominators::Dominators;
use super::edit;
use super::edit::Replacements;
use crate::bitset::BitSet;
use crate::fast_hash::HashMap;
use crate::ir::BlockId;
use crate::ir::Graph;
use crate::ir::NodeId;
use crate::ir::Op;
use std::mem::Discriminant;
use std::mem::discriminant;

fn is_numbered(op: &Op) -> bool {
    let properties = op.properties();
    properties.reads.is_empty()
        && properties.writes.is_empty()
        && !properties.allocates
        && !properties.runs_code
        && !properties.can_lazy_exit
        && !properties.is_call
        && !matches!(op, Op::Constant(_) | Op::Phi)
        && !op.is_control()
}

pub fn run(graph: &mut Graph) {
    let dominators = Dominators::compute(graph);
    let children = dominators.children();
    let mut replacements = Replacements::new(graph);
    let mut removed = BitSet::new(graph.nodes.len());
    // The available nodes by (kind of op, inputs), in dominator tree scopes.
    let mut available: HashMap<(Discriminant<Op>, Vec<NodeId>), Vec<NodeId>> = HashMap::default();

    // Iterative preorder walk of the dominator tree; leaving a block drops
    // what its nodes made available.
    let roots = (0..graph.blocks.len())
        .map(BlockId::from_index)
        .filter(|block| dominators.is_reachable(*block) && dominators.idom(*block).is_none())
        .collect::<Vec<_>>();
    enum Step {
        Enter(BlockId),
        Leave(Vec<(Discriminant<Op>, Vec<NodeId>)>),
    }
    let mut steps = roots.into_iter().rev().map(Step::Enter).collect::<Vec<_>>();
    while let Some(step) = steps.pop() {
        let block = match step {
            Step::Enter(block) => block,
            Step::Leave(keys) => {
                for key in keys {
                    let nodes = available.get_mut(&key).expect("keys are added before they are dropped");
                    nodes.pop();
                }
                continue;
            }
        };
        let mut added = Vec::new();
        for node_id in graph.block(block).body.clone() {
            let node = graph.node(node_id);
            if !is_numbered(&node.op) {
                continue;
            }
            let inputs = node
                .inputs
                .iter()
                .map(|input| replacements.resolve(*input))
                .collect::<Vec<_>>();
            let key = (discriminant(&node.op), inputs);
            let candidates = available.entry(key.clone()).or_default();
            if let Some(existing) = candidates
                .iter()
                .rev()
                .find(|candidate| graph.node(**candidate).op == node.op)
            {
                if node.repr.is_some() {
                    replacements.replace(node_id, *existing);
                }
                removed.insert(node_id.index());
                continue;
            }
            candidates.push(node_id);
            added.push(key);
        }
        steps.push(Step::Leave(added));
        for child in children[block.index()].iter().rev() {
            steps.push(Step::Enter(*child));
        }
    }
    replacements.apply(graph);
    edit::remove_nodes(graph, &removed);
}
