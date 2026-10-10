/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::dominators::Dominators;
use super::verify::verify;
use crate::code::Repr;
use crate::ir::Block;
use crate::ir::BlockId;
use crate::ir::BranchCondition;
use crate::ir::Graph;
use crate::ir::Node;
use crate::ir::NodeId;
use crate::ir::Op;

fn node(graph: &mut Graph, op: Op, inputs: Vec<NodeId>, repr: Option<Repr>) -> NodeId {
    graph.add_node(Node {
        op,
        inputs,
        repr,
        frame_state: None,
        pc: 0,
    })
}

/// A diamond: b0 branches to b1 and b2, which join in b3 with a phi of the
/// values they load.
fn diamond() -> (Graph, NodeId) {
    let mut graph = Graph::default();
    for predecessors in [vec![], vec![BlockId(0)], vec![BlockId(0)], vec![BlockId(1), BlockId(2)]] {
        graph.add_block(Block {
            predecessors,
            ..Block::default()
        });
    }
    let condition = node(&mut graph, Op::LoadSlot { slot: 0 }, vec![], Some(Repr::Tagged));
    let branch = node(
        &mut graph,
        Op::Branch {
            condition: BranchCondition::Undefined,
            if_true: BlockId(1),
            if_false: BlockId(2),
        },
        vec![condition],
        None,
    );
    graph.blocks[0].body.push(condition);
    graph.blocks[0].control = Some(branch);
    let mut loads = Vec::new();
    for block in 1..3 {
        let load = node(&mut graph, Op::LoadSlot { slot: block }, vec![], Some(Repr::Tagged));
        let jump = node(&mut graph, Op::Jump { target: BlockId(3) }, vec![], None);
        graph.blocks[block as usize].body.push(load);
        graph.blocks[block as usize].control = Some(jump);
        loads.push(load);
    }
    let phi = node(&mut graph, Op::Phi, loads, Some(Repr::Tagged));
    let ret = node(&mut graph, Op::Return, vec![phi], None);
    graph.blocks[3].phis.push(phi);
    graph.blocks[3].control = Some(ret);
    (graph, condition)
}

#[test]
fn dominators_of_a_diamond() {
    let (graph, _) = diamond();
    let dominators = Dominators::compute(&graph);
    assert_eq!(dominators.idom(BlockId(0)), None);
    assert_eq!(dominators.idom(BlockId(1)), Some(BlockId(0)));
    assert_eq!(dominators.idom(BlockId(3)), Some(BlockId(0)));
    assert!(dominators.dominates(BlockId(0), BlockId(3)));
    assert!(!dominators.dominates(BlockId(1), BlockId(3)));
    assert!(dominators.dominates(BlockId(3), BlockId(3)));
}

#[test]
fn verifier_accepts_a_valid_graph() {
    let (graph, _) = diamond();
    verify(&graph).unwrap();
}

#[test]
fn verifier_rejects_uses_of_values_from_blocks_that_do_not_dominate() {
    let (mut graph, _) = diamond();
    // The return uses b1's load, which b2 does not see.
    let load = graph.blocks[1].body[0];
    let ret = graph.blocks[3].control.unwrap();
    graph.nodes[ret.index()].inputs = vec![load];
    let errors = verify(&graph).unwrap_err();
    assert!(errors.contains("not defined before it"), "{errors}");
}

#[test]
fn verifier_rejects_refinements_away_from_the_edge_they_refine() {
    let (mut graph, _) = diamond();
    // The join has two predecessors, so no one edge refines anything there.
    let load = graph.blocks[0].body[0];
    let repr = graph.nodes[load.index()].repr;
    let refinement = node(
        &mut graph,
        Op::Refine {
            condition: BranchCondition::Object,
            holds: true,
        },
        vec![load],
        repr,
    );
    graph.blocks[3].body.insert(0, refinement);
    let errors = verify(&graph).unwrap_err();
    assert!(errors.contains("is not at the head of"), "{errors}");
}

#[test]
fn verifier_rejects_inconsistent_edges_and_phis() {
    let (mut graph, _) = diamond();
    graph.blocks[3].predecessors.pop();
    let errors = verify(&graph).unwrap_err();
    assert!(errors.contains("phi"), "{errors}");
}

#[test]
fn verifier_rejects_edges_missing_from_predecessor_lists() {
    let (mut graph, _) = diamond();
    let jump = graph.blocks[2].control.unwrap();
    graph.nodes[jump.index()].op = Op::Jump { target: BlockId(1) };
    let errors = verify(&graph).unwrap_err();
    assert!(errors.contains("edges to b1"), "{errors}");
}

#[test]
fn verifier_rejects_inputs_of_the_wrong_representation() {
    let (mut graph, _) = diamond();
    let branch = graph.blocks[0].control.unwrap();
    if let Op::Branch { condition, .. } = &mut graph.nodes[branch.index()].op {
        *condition = BranchCondition::Bool;
    }
    let errors = verify(&graph).unwrap_err();
    assert!(errors.contains("takes"), "{errors}");
}

#[test]
fn verifier_rejects_blocks_no_entry_reaches() {
    let (mut graph, _) = diamond();
    // b4 is a loop of its own that nothing enters.
    let block = graph.add_block(Block {
        predecessors: vec![BlockId(4)],
        is_loop_header: true,
        ..Block::default()
    });
    let jump = node(&mut graph, Op::Jump { target: block }, vec![], None);
    graph.blocks[block.index()].control = Some(jump);
    let errors = verify(&graph).unwrap_err();
    assert!(errors.contains("b4 is reachable from no entry"), "{errors}");
}

#[test]
fn verifier_rejects_slow_path_outputs_away_from_their_call() {
    let (mut graph, condition) = diamond();
    let call = node(
        &mut graph,
        Op::CallSlowPath {
            opcode: crate::bytecode::OpCode::GetIterator,
            executable: 0,
            pc: 0,
            saves_registers: false,
        },
        vec![condition],
        Some(Repr::Tagged),
    );
    let frame_state = graph.add_frame_state(crate::ir::FrameState {
        executable: 0,
        pc: 0,
        mode: crate::code::ResumeMode::ResumeAt,
        values: Vec::new(),
        in_frame: Vec::new(),
        parent: None,
        passed_argument_count: None,
    });
    graph.nodes[call.index()].frame_state = Some(frame_state);
    let output = node(
        &mut graph,
        Op::SlowPathOutput { index: 1 },
        vec![call],
        Some(Repr::Tagged),
    );
    graph.blocks[0].body.extend([call, output]);
    verify(&graph).unwrap();
    // Anything between the call and the output may reuse the record.
    let load = node(&mut graph, Op::LoadSlot { slot: 5 }, vec![], Some(Repr::Tagged));
    graph.blocks[0].body.insert(2, load);
    let errors = verify(&graph).unwrap_err();
    assert!(errors.contains("does not follow its slow path call"), "{errors}");
}

/// A loop that stores elements in their bounds, and loads how many elements
/// an object it does not change has: the stores change no element count, so
/// the load moves out of the loop.
#[test]
fn element_counts_move_out_of_loops_that_only_store_elements() {
    use crate::ir::Comparison;
    use crate::ir::ElementsKind;
    let mut graph = Graph::default();
    for predecessors in [vec![], vec![BlockId(0), BlockId(1)], vec![BlockId(1)]] {
        graph.add_block(Block {
            predecessors,
            ..Block::default()
        });
    }
    let object = node(&mut graph, Op::LoadSlot { slot: 0 }, vec![], Some(Repr::Tagged));
    let address = node(&mut graph, Op::CellAddress, vec![object], Some(Repr::Pointer));
    let zero = node(&mut graph, Op::Constant(0), vec![], Some(Repr::Int32));
    let enter = node(&mut graph, Op::Jump { target: BlockId(1) }, vec![], None);
    graph.blocks[0].body.extend([object, address]);
    graph.blocks[0].control = Some(enter);
    let length = node(&mut graph, Op::LoadElementsLength, vec![address], Some(Repr::Int32));
    let store = node(
        &mut graph,
        Op::StoreElementAt {
            kind: ElementsKind::Packed,
        },
        vec![address, zero, object],
        None,
    );
    let more = node(
        &mut graph,
        Op::Branch {
            condition: BranchCondition::Int32(Comparison::LessThan),
            if_true: BlockId(1),
            if_false: BlockId(2),
        },
        vec![zero, length],
        None,
    );
    graph.blocks[1].is_loop_header = true;
    graph.blocks[1].body.extend([length, store]);
    graph.blocks[1].control = Some(more);
    let ret = node(&mut graph, Op::Return, vec![object], None);
    graph.blocks[2].control = Some(ret);
    verify(&graph).unwrap();

    super::licm::run(&mut graph);
    assert!(graph.blocks[0].body.contains(&length), "{:?}", graph.blocks);
    assert_eq!(graph.blocks[1].body, vec![store]);
}

/// A branch on the kind of a value tells its edges which kind the value is,
/// so a later branch on the same value there takes the edge it decides.
#[test]
fn branches_on_known_kinds_fold() {
    let mut graph = Graph::default();
    for predecessors in [
        vec![],
        vec![BlockId(0)],
        vec![BlockId(0)],
        vec![BlockId(1)],
        vec![BlockId(1)],
    ] {
        graph.add_block(Block {
            predecessors,
            ..Block::default()
        });
    }
    let value = node(&mut graph, Op::LoadSlot { slot: 0 }, vec![], Some(Repr::Tagged));
    let branch = |graph: &mut Graph, if_true: u32, if_false: u32| {
        node(
            graph,
            Op::Branch {
                condition: BranchCondition::String,
                if_true: BlockId(if_true),
                if_false: BlockId(if_false),
            },
            vec![value],
            None,
        )
    };
    let first = branch(&mut graph, 1, 2);
    graph.blocks[0].body.push(value);
    graph.blocks[0].control = Some(first);
    let second = branch(&mut graph, 3, 4);
    graph.blocks[1].control = Some(second);
    for block in [2, 3, 4] {
        let ret = node(&mut graph, Op::Return, vec![value], None);
        graph.blocks[block].control = Some(ret);
    }
    verify(&graph).unwrap();

    super::check_elimination::run(&mut graph);
    let branches = graph
        .blocks
        .iter()
        .filter_map(|block| block.control)
        .filter(|control| matches!(graph.node(*control).op, Op::Branch { .. }))
        .count();
    assert_eq!(branches, 1, "{:?}", graph.blocks);
}

/// b4 is reached from the entry b0, which checks a constant, and from b2,
/// which only the on-stack replacement entry b1 reaches through b3 (a block
/// later in the order than b2). b2 is skipped until b3 has been visited, so
/// what b4 knows must not stay what b0 checked.
#[test]
fn check_elimination_keeps_checks_that_a_later_entry_reaches_blocks_before_it() {
    let mut graph = Graph::default();
    for predecessors in [
        vec![],
        vec![],
        vec![BlockId(3)],
        vec![BlockId(1)],
        vec![BlockId(0), BlockId(2)],
    ] {
        graph.add_block(Block {
            predecessors,
            ..Block::default()
        });
    }
    graph.blocks[2].is_loop_header = true;
    graph.osr_entries.push((0, BlockId(1)));
    let constant = node(&mut graph, Op::Constant(0x1234), vec![], Some(Repr::Tagged));
    let frame_state = graph.add_frame_state(crate::ir::FrameState {
        executable: 0,
        pc: 0,
        mode: crate::code::ResumeMode::ResumeAt,
        values: Vec::new(),
        in_frame: Vec::new(),
        parent: None,
        passed_argument_count: None,
    });
    let first = node(&mut graph, Op::CheckObject, vec![constant], Some(Repr::Tagged));
    graph.nodes[first.index()].frame_state = Some(frame_state);
    graph.blocks[0].body.push(first);
    for (block, target) in [(0, 4), (1, 3), (2, 4), (3, 2)] {
        let jump = node(
            &mut graph,
            Op::Jump {
                target: BlockId(target),
            },
            vec![],
            None,
        );
        graph.blocks[block].control = Some(jump);
    }
    let second = node(&mut graph, Op::CheckObject, vec![constant], Some(Repr::Tagged));
    graph.nodes[second.index()].frame_state = Some(frame_state);
    let ret = node(&mut graph, Op::Return, vec![second], None);
    graph.blocks[4].body.push(second);
    graph.blocks[4].control = Some(ret);
    verify(&graph).unwrap();

    super::check_elimination::run(&mut graph);
    verify(&graph).unwrap();
    assert_eq!(graph.blocks[4].body, vec![second]);
}
