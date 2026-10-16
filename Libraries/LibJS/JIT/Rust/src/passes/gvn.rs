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
use crate::fast_hash::FastHasher;
use crate::fast_hash::HashMap;
use crate::ir::BlockId;
use crate::ir::Graph;
use crate::ir::Node;
use crate::ir::NodeId;
use crate::ir::Op;
use std::hash::Hash;
use std::hash::Hasher;
use std::mem::discriminant;

fn is_numbered(op: &Op) -> bool {
    let properties = op.properties();
    properties.reads.is_empty()
        && properties.writes.is_empty()
        && !properties.allocates
        && !properties.runs_code
        && !properties.can_lazy_exit
        && !matches!(op, Op::Constant(_) | Op::Phi)
        && !op.is_control()
}

pub fn run(graph: &mut Graph) {
    run_with_hash(graph, |node, replacements| {
        let mut hash = FastHasher::default();
        discriminant(&node.op).hash(&mut hash);
        node.inputs.len().hash(&mut hash);
        for input in &node.inputs {
            replacements.resolve(*input).hash(&mut hash);
        }
        hash.finish()
    });
}

fn run_with_hash(graph: &mut Graph, hash: impl Fn(&Node, &Replacements) -> u64) {
    if graph
        .blocks
        .iter()
        .flat_map(|block| &block.body)
        .filter(|node| is_numbered(&graph.node(**node).op))
        .take(2)
        .count()
        < 2
    {
        return;
    }
    let dominators = Dominators::compute(graph);
    let children = dominators.children();
    let mut replacements = Replacements::new(graph);
    let mut removed = BitSet::new(graph.nodes.len());
    // NB: Hash borrowed inputs instead of allocating keys. Candidates with
    //     the same hash form a chain through `previous`; equality is checked
    //     before reusing one, including when hashes collide.
    let mut available: HashMap<u64, NodeId> = HashMap::default();
    let mut previous = vec![None; graph.nodes.len()];
    let mut added = Vec::new();

    // Iterative preorder walk of the dominator tree; leaving a block drops
    // what its nodes made available.
    let roots = (0..graph.blocks.len())
        .map(BlockId::from_index)
        .filter(|block| dominators.is_reachable(*block) && dominators.idom(*block).is_none())
        .collect::<Vec<_>>();
    enum Step {
        Enter(BlockId),
        Leave(usize),
    }
    let mut steps = roots.into_iter().rev().map(Step::Enter).collect::<Vec<_>>();
    while let Some(step) = steps.pop() {
        let block = match step {
            Step::Enter(block) => block,
            Step::Leave(checkpoint) => {
                while added.len() > checkpoint {
                    let (key, previous) = added.pop().expect("the scope added a node");
                    if let Some(previous) = previous {
                        available.insert(key, previous);
                    } else {
                        available.remove(&key);
                    }
                }
                continue;
            }
        };
        let checkpoint = added.len();
        for node_id in graph.block(block).body.iter().copied() {
            let node = graph.node(node_id);
            if !is_numbered(&node.op) {
                continue;
            }
            let key = hash(node, &replacements);
            let mut candidate = available.get(&key).copied();
            let mut existing = None;
            while let Some(id) = candidate {
                let other = graph.node(id);
                if other.op == node.op
                    && other.inputs.len() == node.inputs.len()
                    && other
                        .inputs
                        .iter()
                        .zip(&node.inputs)
                        .all(|(a, b)| replacements.resolve(*a) == replacements.resolve(*b))
                {
                    existing = Some(id);
                    break;
                }
                candidate = previous[id.index()];
            }
            if let Some(existing) = existing {
                if node.repr.is_some() {
                    replacements.replace(node_id, existing);
                }
                removed.insert(node_id.index());
                continue;
            }
            let old = available.insert(key, node_id);
            previous[node_id.index()] = old;
            added.push((key, old));
        }
        steps.push(Step::Leave(checkpoint));
        for child in children[block.index()].iter().rev() {
            steps.push(Step::Enter(*child));
        }
    }
    replacements.apply(graph);
    edit::remove_nodes(graph, &removed);
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::code::Repr;
    use crate::ir::Block;

    #[test]
    fn hash_collisions_do_not_make_different_nodes_equivalent() {
        let mut graph = Graph::default();
        graph.add_block(Block::default());
        let a = graph.add_node(Node::new(Op::Constant(1), vec![], Some(Repr::Int32), 0));
        let b = graph.add_node(Node::new(Op::Constant(2), vec![], Some(Repr::Int32), 0));
        let first = graph.add_node(Node::new(Op::BoxInt32, vec![a], Some(Repr::Tagged), 0));
        let different_input = graph.add_node(Node::new(Op::BoxInt32, vec![b], Some(Repr::Tagged), 0));
        let different_operation = graph.add_node(Node::new(Op::Int32ToFloat64, vec![a], Some(Repr::Float64), 0));
        let duplicate = graph.add_node(Node::new(Op::BoxInt32, vec![a], Some(Repr::Tagged), 0));
        let returned = graph.add_node(Node::new(Op::Return, vec![duplicate], None, 0));
        graph.blocks[0].body = vec![first, different_input, different_operation, duplicate];
        graph.blocks[0].control = Some(returned);
        run_with_hash(&mut graph, |_, _| 0);
        assert_eq!(graph.blocks[0].body, [first, different_input, different_operation]);
        assert_eq!(graph.node(returned).inputs, [first]);
    }
}
