/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Dead code elimination: removes nodes whose values nobody uses, as long
//! as running them has no effect beyond producing their value. Frame states
//! count as uses, since exits need their values. Int32 arithmetic only
//! exits to produce a value that is no int32, so it goes too. Branches both
//! of whose edges lead to the same place, doing nothing, become jumps.

use super::edit;
use crate::bitset::BitSet;
use crate::ir::Graph;
use crate::ir::Op;

/// Whether a node can be dropped when its value is unused.
fn is_removable(op: &Op) -> bool {
    if matches!(op, Op::Phi | Op::Int32Binary { .. }) {
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
    let mut live = BitSet::new(count);
    let mut live_frames = BitSet::new(graph.frame_states.len());
    let mut pending = Vec::new();
    for block in &graph.blocks {
        pending.extend(block.nodes().filter(|node| {
            let data = graph.node(*node);
            data.repr.is_none() || !is_removable(&data.op)
        }));
    }
    while let Some(node) = pending.pop() {
        if live.contains(node.index()) {
            continue;
        }
        live.insert(node.index());
        let data = graph.node(node);
        pending.extend(data.inputs.iter().copied());
        let mut frame = data.frame_state;
        while let Some(id) = frame {
            if live_frames.contains(id.index()) {
                break;
            }
            live_frames.insert(id.index());
            let state = graph.frame_state(id);
            pending.extend(state.values.iter().map(|(_, value)| *value));
            frame = state.parent;
        }
    }
    let mut removed = BitSet::new(count);
    for block in &graph.blocks {
        for node in block.nodes() {
            if !live.contains(node.index()) {
                removed.insert(node.index());
            }
        }
    }
    edit::remove_nodes(graph, &removed);
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::code::{Repr, ResumeMode};
    use crate::ir::{BinaryOp, Block, FrameState};
    use crate::snapshot::CellId;

    #[test]
    fn unused_cycles_of_pure_values_are_removed() {
        let mut graph = Graph::default();
        graph.add_block(Block::default());
        let constant = graph.add_node(crate::ir::Node::new(Op::Constant(1), vec![], Some(Repr::Int32), 0));
        let phi = graph.add_node(crate::ir::Node::new(Op::Phi, vec![], Some(Repr::Int32), 0));
        let add = graph.add_node(crate::ir::Node::new(
            Op::Int32Binary {
                op: BinaryOp::BitwiseOr,
            },
            vec![phi, constant],
            Some(Repr::Int32),
            0,
        ));
        graph.nodes[phi.index()].inputs = vec![phi, add];
        graph.blocks[0].phis.push(phi);
        graph.blocks[0].body.push(add);
        let returned = graph.add_node(crate::ir::Node::new(Op::Return, vec![constant], None, 0));
        graph.blocks[0].control = Some(returned);
        remove_dead_nodes(&mut graph);
        assert!(graph.blocks[0].phis.is_empty());
        assert!(graph.blocks[0].body.is_empty());
        assert_eq!(graph.blocks[0].control, Some(returned));
    }

    #[test]
    fn shared_frame_states_keep_nested_virtual_object_properties_alive() {
        let mut graph = Graph::default();
        graph.add_block(Block::default());
        let constant = graph.add_node(crate::ir::Node::new(Op::Constant(1), vec![], Some(Repr::Int32), 0));
        let boxed = graph.add_node(crate::ir::Node::new(
            Op::BoxInt32,
            vec![constant],
            Some(Repr::Tagged),
            0,
        ));
        let object = graph.add_node(crate::ir::Node::new(
            Op::VirtualObject { shape: CellId(1) },
            vec![boxed],
            Some(Repr::Tagged),
            0,
        ));
        let frame = graph.add_frame_state(FrameState {
            executable: 0,
            pc: 0,
            mode: ResumeMode::ResumeAt,
            values: vec![(0, object)],
            in_frame: vec![],
            parent: None,
            passed_argument_count: None,
        });
        graph.blocks[0].body.push(boxed);
        for _ in 0..2 {
            let check = graph.add_node(crate::ir::Node::new(
                Op::CheckObject,
                vec![boxed],
                Some(Repr::Tagged),
                0,
            ));
            graph.nodes[check.index()].frame_state = Some(frame);
            graph.blocks[0].body.push(check);
        }
        let returned = graph.add_node(crate::ir::Node::new(Op::Return, vec![constant], None, 0));
        graph.blocks[0].control = Some(returned);
        remove_dead_nodes(&mut graph);
        assert!(graph.blocks[0].body.contains(&boxed));
        assert_eq!(graph.blocks[0].body.len(), 3);
    }
}
