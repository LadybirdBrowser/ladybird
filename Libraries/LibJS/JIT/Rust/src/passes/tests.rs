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
        legacy_arguments: 0..0,
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

#[test]
fn simplification_removes_number_checks_of_numeric_constants() {
    use crate::ir::value;
    for (bits, expected) in [
        (0.5_f64.to_bits(), Some(0.5_f64)),
        (value::int32(-3), Some(-3.0)),
        (value::UNDEFINED, None),
    ] {
        let mut graph = Graph::default();
        graph.add_block(Block::default());
        let constant = node(&mut graph, Op::Constant(bits), vec![], Some(Repr::Tagged));
        let check = node(&mut graph, Op::CheckNumber, vec![constant], Some(Repr::Float64));
        graph.nodes[check.index()].frame_state = Some(graph.add_frame_state(crate::ir::FrameState {
            executable: 0,
            pc: 0,
            mode: crate::code::ResumeMode::ResumeAt,
            values: Vec::new(),
            in_frame: Vec::new(),
            parent: None,
            passed_argument_count: None,
            legacy_arguments: 0..0,
        }));
        let boxed = node(&mut graph, Op::BoxFloat64, vec![check], Some(Repr::Tagged));
        let ret = node(&mut graph, Op::Return, vec![boxed], None);
        graph.blocks[0].body = vec![check, boxed];
        graph.blocks[0].control = Some(ret);
        verify(&graph).unwrap();

        super::simplify::run(&mut graph);
        verify(&graph).unwrap();
        assert_eq!(!graph.blocks[0].body.contains(&check), expected.is_some());
        let input = graph.node(boxed).inputs[0];
        assert_eq!(
            graph.constant_value(input).map(f64::from_bits),
            expected,
            "{}",
            value::describe(bits)
        );
    }
}

#[test]
fn simplification_removes_int32_checks_that_number_checks_convert() {
    let mut graph = Graph::default();
    for predecessors in [vec![], vec![BlockId(0)], vec![BlockId(0)]] {
        graph.add_block(Block {
            predecessors,
            ..Block::default()
        });
    }
    let frame_state = graph.add_frame_state(crate::ir::FrameState {
        executable: 0,
        pc: 0,
        mode: crate::code::ResumeMode::ResumeAt,
        values: Vec::new(),
        in_frame: Vec::new(),
        parent: None,
        passed_argument_count: None,
        legacy_arguments: 0..0,
    });
    let double = node(&mut graph, Op::Constant(2.0_f64.to_bits()), vec![], Some(Repr::Float64));
    let boxed_double = node(&mut graph, Op::BoxFloat64, vec![double], Some(Repr::Tagged));
    let check = node(&mut graph, Op::CheckInt32, vec![boxed_double], Some(Repr::Int32));
    graph.nodes[check.index()].frame_state = Some(frame_state);
    let boxed = node(&mut graph, Op::BoxInt32, vec![check], Some(Repr::Tagged));
    let branch = node(
        &mut graph,
        Op::Branch {
            condition: BranchCondition::Int32Value,
            if_true: BlockId(1),
            if_false: BlockId(2),
        },
        vec![boxed],
        None,
    );
    graph.blocks[0].body = vec![boxed_double, check, boxed];
    graph.blocks[0].control = Some(branch);
    // The number check sees the boxing through a refinement, which
    // simplification does not replace.
    let refined = node(
        &mut graph,
        Op::Refine {
            condition: BranchCondition::Int32Value,
            holds: true,
        },
        vec![boxed],
        Some(Repr::Tagged),
    );
    let number = node(&mut graph, Op::CheckNumber, vec![refined], Some(Repr::Float64));
    graph.nodes[number.index()].frame_state = Some(frame_state);
    let result = node(&mut graph, Op::BoxFloat64, vec![number], Some(Repr::Tagged));
    let ret = node(&mut graph, Op::Return, vec![result], None);
    graph.blocks[1].body = vec![refined, number, result];
    graph.blocks[1].control = Some(ret);
    let other_ret = node(&mut graph, Op::Return, vec![boxed], None);
    graph.blocks[2].control = Some(other_ret);
    verify(&graph).unwrap();

    super::simplify::run(&mut graph);
    verify(&graph).unwrap();
    assert!(!graph.blocks[0].body.contains(&check));
    assert_eq!(graph.node(result).inputs, vec![double]);
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

#[test]
fn typed_array_storage_offsets_are_reused_across_stores_but_not_calls() {
    use crate::ir::{ElementsKind, TypedArrayElement};
    let mut graph = Graph::default();
    graph.add_block(Block::default());
    let object = node(&mut graph, Op::LoadSlot { slot: 0 }, vec![], Some(Repr::Tagged));
    let address = node(&mut graph, Op::CellAddress, vec![object], Some(Repr::Pointer));
    let first = node(
        &mut graph,
        Op::LoadTypedArrayDataOffset,
        vec![address],
        Some(Repr::Pointer),
    );
    let first_data = node(&mut graph, Op::TypedArrayDataAddress, vec![first], Some(Repr::Pointer));
    let zero = node(&mut graph, Op::Constant(0), vec![], Some(Repr::Int32));
    let store = node(
        &mut graph,
        Op::StoreElementAt {
            kind: ElementsKind::TypedArray(TypedArrayElement::Int32),
        },
        vec![first_data, zero, zero],
        None,
    );
    let second = node(
        &mut graph,
        Op::LoadTypedArrayDataOffset,
        vec![address],
        Some(Repr::Pointer),
    );
    let second_data = node(&mut graph, Op::TypedArrayDataAddress, vec![second], Some(Repr::Pointer));
    let load = node(
        &mut graph,
        Op::LoadElementAt {
            kind: ElementsKind::TypedArray(TypedArrayElement::Int32),
        },
        vec![second_data, zero],
        Some(Repr::Int32),
    );
    let call = node(
        &mut graph,
        Op::Generic {
            opcode: crate::bytecode::OpCode::Call,
            executable: 0,
            pc: 0,
        },
        vec![],
        None,
    );
    let third = node(
        &mut graph,
        Op::LoadTypedArrayDataOffset,
        vec![address],
        Some(Repr::Pointer),
    );
    graph.nodes[call.index()].frame_state = Some(graph.add_frame_state(crate::ir::FrameState {
        executable: 0,
        pc: 0,
        mode: crate::code::ResumeMode::ResumeAt,
        values: Vec::new(),
        in_frame: Vec::new(),
        parent: None,
        passed_argument_count: None,
        legacy_arguments: 0..0,
    }));
    graph.blocks[0].body.extend([
        object,
        address,
        first,
        first_data,
        store,
        second,
        second_data,
        load,
        call,
        third,
    ]);
    let ret = node(&mut graph, Op::Return, vec![object], None);
    graph.blocks[0].control = Some(ret);
    verify(&graph).unwrap();
    super::check_elimination::run(&mut graph);
    assert!(!graph.blocks[0].body.contains(&second));
    assert_eq!(graph.node(second_data).inputs[0], first);
    assert!(graph.blocks[0].body.contains(&third));
    verify(&graph).unwrap();
}

#[test]
fn typed_array_storage_metadata_moves_out_of_loops_that_only_store_elements() {
    use crate::ir::{Comparison, ElementsKind, TypedArrayElement};
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
    let length = node(&mut graph, Op::LoadTypedArrayLength, vec![address], Some(Repr::Int32));
    let offset = node(
        &mut graph,
        Op::LoadTypedArrayDataOffset,
        vec![address],
        Some(Repr::Pointer),
    );
    let data = node(&mut graph, Op::TypedArrayDataAddress, vec![offset], Some(Repr::Pointer));
    let store = node(
        &mut graph,
        Op::StoreElementAt {
            kind: ElementsKind::TypedArray(TypedArrayElement::Int32),
        },
        vec![data, zero, zero],
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
    graph.blocks[1].body.extend([length, offset, data, store]);
    graph.blocks[1].control = Some(more);
    let ret = node(&mut graph, Op::Return, vec![object], None);
    graph.blocks[2].control = Some(ret);
    verify(&graph).unwrap();
    super::licm::run(&mut graph);
    for metadata in [length, offset, data] {
        assert!(graph.blocks[0].body.contains(&metadata));
    }
    assert_eq!(graph.blocks[1].body, vec![store]);
    verify(&graph).unwrap();
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

/// A value that passed an int32 check is no empty value, so a branch on
/// whether it is one (like the initialization check of a binding store)
/// takes the edge where it is not.
#[test]
fn branches_on_whether_values_of_known_kinds_are_empty_fold() {
    let mut graph = Graph::default();
    for predecessors in [vec![], vec![BlockId(0)], vec![BlockId(0)]] {
        graph.add_block(Block {
            predecessors,
            ..Block::default()
        });
    }
    let frame_state = graph.add_frame_state(crate::ir::FrameState {
        executable: 0,
        pc: 0,
        mode: crate::code::ResumeMode::ResumeAt,
        values: Vec::new(),
        in_frame: Vec::new(),
        parent: None,
        passed_argument_count: None,
        legacy_arguments: 0..0,
    });
    let value = node(&mut graph, Op::LoadSlot { slot: 0 }, vec![], Some(Repr::Tagged));
    let check = node(&mut graph, Op::CheckInt32, vec![value], Some(Repr::Int32));
    graph.nodes[check.index()].frame_state = Some(frame_state);
    let empty = node(
        &mut graph,
        Op::Constant(crate::ir::value::EMPTY),
        vec![],
        Some(Repr::Tagged),
    );
    let branch = node(
        &mut graph,
        Op::Branch {
            condition: BranchCondition::TaggedEquals { equal: false },
            if_true: BlockId(1),
            if_false: BlockId(2),
        },
        vec![value, empty],
        None,
    );
    graph.blocks[0].body = vec![value, check];
    graph.blocks[0].control = Some(branch);
    for block in [1, 2] {
        let ret = node(&mut graph, Op::Return, vec![value], None);
        graph.blocks[block].control = Some(ret);
    }
    verify(&graph).unwrap();

    super::check_elimination::run(&mut graph);
    let control = graph.blocks[0].control.unwrap();
    assert!(
        matches!(graph.node(control).op, Op::Jump { target: BlockId(1) }),
        "{:?}",
        graph.node(control)
    );
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
        legacy_arguments: 0..0,
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

#[test]
fn simplification_removes_integer_double_round_trips() {
    for conversion in [Op::Int32ToFloat64, Op::Uint32ToFloat64] {
        let mut graph = Graph::default();
        graph.add_block(Block::default());
        let bits = node(&mut graph, Op::Constant(0xffff_ffff), vec![], Some(Repr::Int32));
        let double = node(&mut graph, conversion, vec![bits], Some(Repr::Float64));
        let integer = node(&mut graph, Op::Float64ToInt32, vec![double], Some(Repr::Int32));
        let boxed = node(&mut graph, Op::BoxInt32, vec![integer], Some(Repr::Tagged));
        let ret = node(&mut graph, Op::Return, vec![boxed], None);
        graph.blocks[0].body = vec![double, integer, boxed];
        graph.blocks[0].control = Some(ret);
        verify(&graph).unwrap();

        super::simplify::run(&mut graph);
        verify(&graph).unwrap();
        assert!(!graph.blocks[0].body.contains(&integer));
        assert_eq!(
            graph.constant_value(graph.node(ret).inputs[0]),
            Some(crate::ir::value::int32(-1))
        );
    }
}

#[test]
fn simplification_tests_the_truthiness_of_boxed_int32_values_unboxed() {
    // b0 branches on the truthiness of a boxed int32 to b1 and b2, with b3
    // as the fallback for other values.
    let mut graph = Graph::default();
    graph.add_block(Block::default());
    for _ in 1..4 {
        graph.add_block(Block {
            predecessors: vec![BlockId(0)],
            ..Block::default()
        });
    }
    let array = node(&mut graph, Op::LoadSlot { slot: 0 }, vec![], Some(Repr::Tagged));
    let address = node(&mut graph, Op::CellAddress, vec![array], Some(Repr::Pointer));
    let unboxed = node(&mut graph, Op::LoadElementsLength, vec![address], Some(Repr::Int32));
    let boxed = node(&mut graph, Op::BoxInt32, vec![unboxed], Some(Repr::Tagged));
    let branch = node(
        &mut graph,
        Op::BranchTruthy {
            if_true: BlockId(1),
            if_false: BlockId(2),
            fallback: BlockId(3),
        },
        vec![boxed],
        None,
    );
    graph.blocks[0].body = vec![array, address, unboxed, boxed];
    graph.blocks[0].control = Some(branch);
    for block in 1..4 {
        let ret = node(&mut graph, Op::Return, vec![array], None);
        graph.blocks[block].control = Some(ret);
    }
    verify(&graph).unwrap();

    super::simplify::run(&mut graph);
    verify(&graph).unwrap();
    let branch = graph.blocks[0].control.unwrap();
    assert_eq!(
        graph.node(branch).op,
        Op::Branch {
            condition: BranchCondition::Int32(crate::ir::Comparison::StrictlyInequals),
            if_true: BlockId(1),
            if_false: BlockId(2),
        }
    );
    assert_eq!(graph.node(branch).inputs[0], unboxed);
    assert_eq!(graph.constant_value(graph.node(branch).inputs[1]), Some(0));
}

#[test]
fn simplification_keeps_double_integer_round_trips() {
    let mut graph = Graph::default();
    graph.add_block(Block::default());
    let load = node(&mut graph, Op::LoadSlot { slot: 5 }, vec![], Some(Repr::Tagged));
    let double = node(&mut graph, Op::UnboxDouble, vec![load], Some(Repr::Float64));
    let integer = node(&mut graph, Op::Float64ToInt32, vec![double], Some(Repr::Int32));
    let converted = node(&mut graph, Op::Int32ToFloat64, vec![integer], Some(Repr::Float64));
    let boxed = node(&mut graph, Op::BoxFloat64, vec![converted], Some(Repr::Tagged));
    let ret = node(&mut graph, Op::Return, vec![boxed], None);
    graph.blocks[0].body = vec![load, double, integer, converted, boxed];
    graph.blocks[0].control = Some(ret);
    verify(&graph).unwrap();

    super::simplify::run(&mut graph);
    verify(&graph).unwrap();
    assert_eq!(graph.node(converted).inputs, vec![integer]);
    assert!(graph.blocks[0].body.contains(&integer));
}

#[test]
fn simplification_converts_constant_doubles_to_integers() {
    for (op, number, expected) in [
        (Op::Float64ToInt32, 1.5, 1),
        (Op::Float64ToInt32, -1.5, -1),
        (Op::Float64ToInt32, -0.0, 0),
        (Op::Float64ToInt32, f64::NAN, 0),
        (Op::Float64ToInt32, f64::INFINITY, 0),
        (Op::Float64ToInt32, 4_294_967_301.0, 5),
        (Op::Float64ToInt32, 2_147_483_648.0, i32::MIN),
        (Op::Float64ToInt32, -2_147_483_649.0, i32::MAX),
        (Op::Float64ToUint8Clamped, 2.5, 2),
        (Op::Float64ToUint8Clamped, 3.5, 4),
        (Op::Float64ToUint8Clamped, 254.9, 255),
        (Op::Float64ToUint8Clamped, 300.0, 255),
        (Op::Float64ToUint8Clamped, -1.0, 0),
        (Op::Float64ToUint8Clamped, f64::NAN, 0),
    ] {
        let mut graph = Graph::default();
        graph.add_block(Block::default());
        let double = node(&mut graph, Op::Constant(number.to_bits()), vec![], Some(Repr::Float64));
        let integer = node(&mut graph, op.clone(), vec![double], Some(Repr::Int32));
        let boxed = node(&mut graph, Op::BoxInt32, vec![integer], Some(Repr::Tagged));
        let ret = node(&mut graph, Op::Return, vec![boxed], None);
        graph.blocks[0].body = vec![integer, boxed];
        graph.blocks[0].control = Some(ret);
        verify(&graph).unwrap();

        super::simplify::run(&mut graph);
        verify(&graph).unwrap();
        assert_eq!(
            graph.constant_value(graph.node(ret).inputs[0]),
            Some(crate::ir::value::int32(expected)),
            "{op:?} of {number}"
        );
    }
}

#[test]
fn constant_bounds_checks_only_fold_when_the_unsigned_index_is_in_bounds() {
    for (index, count, folds) in [
        (0, 1, true),
        (7, 8, true),
        (8, 8, false),
        (-1, 8, false),
        (0, 0, false),
        (i32::MAX, i32::MIN, true),
    ] {
        let mut graph = Graph::default();
        graph.add_block(Block::default());
        let index = node(
            &mut graph,
            Op::Constant(u64::from(index as u32)),
            vec![],
            Some(Repr::Int32),
        );
        let count = node(
            &mut graph,
            Op::Constant(u64::from(count as u32)),
            vec![],
            Some(Repr::Int32),
        );
        let check = node(&mut graph, Op::CheckBounds, vec![index, count], Some(Repr::Int32));
        graph.nodes[check.index()].frame_state = Some(graph.add_frame_state(crate::ir::FrameState {
            executable: 0,
            pc: 0,
            mode: crate::code::ResumeMode::ResumeAt,
            values: Vec::new(),
            in_frame: Vec::new(),
            parent: None,
            passed_argument_count: None,
            legacy_arguments: 0..0,
        }));
        let boxed = node(&mut graph, Op::BoxInt32, vec![check], Some(Repr::Tagged));
        let ret = node(&mut graph, Op::Return, vec![boxed], None);
        graph.blocks[0].body.extend([check, boxed]);
        graph.blocks[0].control = Some(ret);
        verify(&graph).unwrap();
        super::simplify::run(&mut graph);
        assert_eq!(graph.blocks[0].body.contains(&check), !folds);
        verify(&graph).unwrap();
    }
}

/// A register-saving slow path initializes the frame if it runs, so a frame
/// write after it initializes the frame only if it still is not, instead of
/// emptying the frame the slow path may have initialized and written.
#[test]
fn frame_writes_after_slow_paths_that_may_initialize_the_frame_check_first() {
    use crate::bytecode::FrameLayout;
    use crate::bytecode::RESERVED_REGISTER_COUNT;
    let layout = FrameLayout {
        number_of_registers: RESERVED_REGISTER_COUNT + 1,
        registers_and_locals_count: RESERVED_REGISTER_COUNT + 1,
        number_of_constants: 0,
        number_of_arguments: 0,
    };
    let mut graph = Graph::default();
    graph.add_block(Block::default());
    let initialize = node(&mut graph, Op::InitializeFrame { clobbers_floats: false }, vec![], None);
    let call = node(
        &mut graph,
        Op::CallSlowPath {
            opcode: crate::bytecode::OpCode::GetIterator,
            executable: 0,
            pc: 0,
            saves_registers: true,
        },
        vec![],
        Some(Repr::Tagged),
    );
    let store = node(
        &mut graph,
        Op::StoreSlot {
            slot: RESERVED_REGISTER_COUNT,
        },
        vec![call],
        None,
    );
    let ret = node(&mut graph, Op::Return, vec![call], None);
    graph.blocks[0].body = vec![initialize, call, store];
    graph.blocks[0].control = Some(ret);

    super::frame_initialization::run(&mut graph, &layout);
    assert_eq!(graph.slow_paths_initializing_frame, vec![call]);
    let ops: Vec<&Op> = graph.blocks[0].body.iter().map(|node| &graph.node(*node).op).collect();
    assert!(matches!(
        ops.as_slice(),
        [
            Op::CallSlowPath { .. },
            Op::EnsureFrameInitialized { .. },
            Op::StoreSlot { .. }
        ]
    ));
}

#[test]
fn hoisted_checks_gate_exits_by_the_loop_headers_executable() {
    use crate::code::{ExitKind, ResumeMode};
    use crate::ir::FrameState;
    for failed_executable in [0, 1] {
        let mut graph = Graph::default();
        for predecessors in [vec![], vec![BlockId(0), BlockId(1)], vec![BlockId(1)]] {
            graph.add_block(Block {
                predecessors,
                ..Block::default()
            });
        }
        let object = node(&mut graph, Op::LoadSlot { slot: 0 }, vec![], Some(Repr::Tagged));
        let condition = node(&mut graph, Op::Constant(1), vec![], Some(Repr::Bool));
        let enter = node(&mut graph, Op::Jump { target: BlockId(1) }, vec![], None);
        graph.blocks[0].body.push(object);
        graph.blocks[0].control = Some(enter);
        let parent = graph.add_frame_state(FrameState {
            executable: 0,
            pc: 10,
            mode: ResumeMode::ResumeAfter { dst: 5 },
            values: vec![(0, object)],
            in_frame: vec![],
            parent: None,
            passed_argument_count: None,
            legacy_arguments: 0..0,
        });
        let state = graph.add_frame_state(FrameState {
            executable: 1,
            pc: 77,
            mode: ResumeMode::ResumeAt,
            values: vec![(0, object)],
            in_frame: vec![],
            parent: Some(parent),
            passed_argument_count: None,
            legacy_arguments: 0..0,
        });
        let check = node(&mut graph, Op::CheckObject, vec![object], Some(Repr::Tagged));
        graph.nodes[check.index()].frame_state = Some(state);
        graph.blocks[1].bytecode_start = Some(77);
        graph.blocks[1].start_frame_state = Some(state);
        graph.blocks[1].is_loop_header = true;
        graph.blocks[1].body.push(check);
        let more = node(
            &mut graph,
            Op::Branch {
                condition: BranchCondition::Bool,
                if_true: BlockId(1),
                if_false: BlockId(2),
            },
            vec![condition],
            None,
        );
        graph.blocks[1].control = Some(more);
        let ret = node(&mut graph, Op::Return, vec![object], None);
        graph.blocks[2].control = Some(ret);
        graph.exit_sites.push((failed_executable, 77, ExitKind::NotObject));
        verify(&graph).unwrap();
        super::licm::run(&mut graph);
        verify(&graph).unwrap();
        assert_eq!(graph.blocks[0].body.contains(&check), failed_executable == 0);
        let recovery = graph.frame_state(graph.node(check).frame_state.unwrap());
        assert_eq!(recovery.executable, 1);
        assert_eq!(recovery.parent, Some(parent));
    }
}

#[test]
fn control_flow_cleanup_preserves_duplicate_edges_and_phi_input_order() {
    let mut graph = Graph::default();
    graph.add_block(Block::default());
    graph.add_block(Block {
        predecessors: vec![BlockId(0); 3],
        ..Block::default()
    });
    let values = (0..3)
        .map(|bits| node(&mut graph, Op::Constant(bits), vec![], Some(Repr::Bool)))
        .collect::<Vec<_>>();
    let branch = node(
        &mut graph,
        Op::Branch {
            condition: BranchCondition::Bool,
            if_true: BlockId(1),
            if_false: BlockId(1),
        },
        vec![values[0]],
        None,
    );
    graph.blocks[0].control = Some(branch);
    let phi = node(&mut graph, Op::Phi, values.clone(), Some(Repr::Bool));
    graph.blocks[1].phis.push(phi);
    let returned = node(&mut graph, Op::Return, vec![phi], None);
    graph.blocks[1].control = Some(returned);
    super::edit::clean_up_control_flow(&mut graph);
    assert_eq!(graph.blocks[1].predecessors, [BlockId(0), BlockId(0)]);
    assert_eq!(graph.node(phi).inputs, values[..2]);
}

/// An addition whose operand a bounds check keeps below a count, where the
/// count is a loop counter that an on-stack replacement entry brings, relies
/// on the counter being below the loop's bound where it enters: it stops
/// checking for overflow only together with the check of the counter there.
#[test]
fn overflow_checks_removed_by_bounds_checks_check_the_counters_of_their_counts() {
    use crate::code::ExitKind;
    use crate::code::ResumeMode;
    use crate::ir::BinaryOp;
    use crate::ir::Comparison;
    use crate::ir::FrameState;
    // b0 and the on-stack replacement entry b1 start the counter in b2, the
    // loop's preheader. The loop b3 (with the back edge from b4) adds one to
    // an index that a bounds check against the counter checks (not to the
    // value the check refines), and to the counter until it is 100.
    for failed_before in [false, true] {
        let mut graph = Graph::default();
        for predecessors in [
            vec![],
            vec![],
            vec![BlockId(0), BlockId(1)],
            vec![BlockId(2), BlockId(4)],
            vec![BlockId(3)],
            vec![BlockId(3)],
        ] {
            graph.add_block(Block {
                predecessors,
                ..Block::default()
            });
        }
        graph.blocks[3].is_loop_header = true;
        graph.osr_entries.push((7, BlockId(1)));
        let state = graph.add_frame_state(FrameState {
            executable: 0,
            pc: 7,
            mode: ResumeMode::ResumeAt,
            values: Vec::new(),
            in_frame: Vec::new(),
            parent: None,
            passed_argument_count: None,
            legacy_arguments: 0..0,
        });
        if failed_before {
            graph.exit_sites.push((0, 7, ExitKind::OutOfBounds));
        }
        let int32 = |graph: &mut Graph, value: i32| {
            node(graph, Op::Constant(u64::from(value as u32)), vec![], Some(Repr::Int32))
        };
        let checked = |graph: &mut Graph, op: Op, inputs: Vec<NodeId>| {
            let id = node(graph, op, inputs, Some(Repr::Int32));
            graph.nodes[id.index()].frame_state = Some(state);
            id
        };
        let jump = |graph: &mut Graph, block: usize, target: u32| {
            let jump = node(
                graph,
                Op::Jump {
                    target: BlockId(target),
                },
                vec![],
                None,
            );
            graph.blocks[block].control = Some(jump);
        };

        let zero = int32(&mut graph, 0);
        jump(&mut graph, 0, 2);

        let slot = node(&mut graph, Op::LoadSlot { slot: 0 }, vec![], Some(Repr::Tagged));
        let counter = checked(&mut graph, Op::CheckInt32, vec![slot]);
        graph.blocks[1].body.extend([slot, counter]);
        jump(&mut graph, 1, 2);

        let start = node(&mut graph, Op::Phi, vec![zero, counter], Some(Repr::Int32));
        graph.blocks[2].phis.push(start);
        let index_slot = node(&mut graph, Op::LoadSlot { slot: 1 }, vec![], Some(Repr::Tagged));
        let index = checked(&mut graph, Op::CheckInt32, vec![index_slot]);
        graph.blocks[2].body.extend([index_slot, index]);
        jump(&mut graph, 2, 3);

        let one = int32(&mut graph, 1);
        let bound = int32(&mut graph, 100);
        let phi = node(&mut graph, Op::Phi, vec![start], Some(Repr::Int32));
        graph.blocks[3].phis.push(phi);
        let check = checked(&mut graph, Op::CheckBounds, vec![index, phi]);
        let sum = checked(&mut graph, Op::Int32Binary { op: BinaryOp::Add }, vec![index, one]);
        let next = node(
            &mut graph,
            Op::Int32WrappingBinary { op: BinaryOp::Add },
            vec![phi, one],
            Some(Repr::Int32),
        );
        graph.nodes[phi.index()].inputs.push(next);
        let compare = node(
            &mut graph,
            Op::Int32Compare {
                comparison: Comparison::LessThan,
            },
            vec![next, bound],
            Some(Repr::Bool),
        );
        let branch = node(
            &mut graph,
            Op::Branch {
                condition: BranchCondition::Bool,
                if_true: BlockId(4),
                if_false: BlockId(5),
            },
            vec![compare],
            None,
        );
        graph.blocks[3].body.extend([check, sum, next, compare]);
        graph.blocks[3].control = Some(branch);
        jump(&mut graph, 4, 3);

        let boxed = node(&mut graph, Op::BoxInt32, vec![sum], Some(Repr::Tagged));
        let ret = node(&mut graph, Op::Return, vec![boxed], None);
        graph.blocks[5].body.push(boxed);
        graph.blocks[5].control = Some(ret);

        verify(&graph).unwrap();
        super::bounds_check_elimination::run(&mut graph);
        verify(&graph).unwrap();

        let guarded = graph.blocks[1].body.iter().any(|node| {
            let node = graph.node(*node);
            node.op == Op::CheckBounds && node.inputs[0] == counter && graph.int32_constant(node.inputs[1]) == Some(100)
        });
        let wrapping = matches!(graph.node(sum).op, Op::Int32WrappingBinary { .. });
        assert_eq!(guarded, !failed_before);
        assert_eq!(wrapping, !failed_before);
    }
}
