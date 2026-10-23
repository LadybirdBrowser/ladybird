/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use crate::builder::tests::build;
use crate::builder::tests::build_with_layout;
use crate::bytecode::FrameLayout;
use crate::bytecode::Instruction;
use crate::bytecode::Operand;
use crate::bytecode::test_support::*;
use crate::code::ExitKind;
use crate::code::FrameState as CodeFrameState;
use crate::code::Site;
use crate::code::SiteKind;
use crate::code::ValueLocation;
use crate::ir::value;

#[track_caller]
fn allocate_and_verify(graph: &Graph) -> Allocation {
    let registers = RegisterSet::test_configuration();
    let allocation = allocate(graph, &registers);
    if let Err(errors) = verify(graph, &registers, &allocation) {
        panic!("{errors}\n{}", dump_allocation(graph, &allocation));
    }
    allocation
}

fn find_node(graph: &Graph, predicate: impl Fn(&crate::ir::Node) -> bool) -> NodeId {
    let index = graph.nodes.iter().position(predicate).expect("no such node");
    NodeId(u32::try_from(index).unwrap())
}

#[track_caller]
fn assert_text(actual: &str, expected: &str) {
    let expected = expected
        .lines()
        .map(str::trim_end)
        .skip_while(|line| line.is_empty())
        .map(|line| line.strip_prefix("        ").unwrap_or(line))
        .collect::<Vec<_>>()
        .join("\n");
    assert_eq!(actual.trim_end(), expected.trim_end(), "\nactual:\n{actual}");
}

#[test]
fn straight_line_code() {
    let program = assemble(|_| {
        vec![
            Instruction::Enter,
            Instruction::Mov { dst: r(5), src: c(1) },
            Instruction::Mov { dst: r(6), src: a(0) },
            Instruction::Exp {
                arith_feedback: 0,
                dst: r(7),
                lhs: r(5),
                rhs: r(6),
            },
            Instruction::Return { value: r(7) },
        ]
    });
    let graph = build(&program);
    let allocation = allocate_and_verify(&graph);
    assert_text(
        &dump_allocation(&graph, &allocation),
        "
        b0:
          v0 Jump
        b1:
          v5 LoadSlot -> r0
          v9 InitializeFrame temps r1, r2
          v6 CallSlowPath (#10, r0) -> r0
          v7 EmptyToUndefined (r0) -> r1
          moves r1 -> r0
          v8 Return (r0)
        spill slots: 0
        ",
    );
}

#[test]
fn values_live_across_calls_are_spilled_at_their_definition() {
    let program = assemble(|_| {
        vec![
            Instruction::Mov { dst: r(6), src: a(0) },
            Instruction::Call {
                value_feedback: 0,
                call_feedback: 0,
                dst: r(7),
                callee: r(5),
                this_value: r(5),
                argument_count: 1,
                expression_string: None,
                arguments: vec![r(6)],
            },
            Instruction::Return { value: r(6) },
        ]
    });
    let graph = build(&program);
    let allocation = allocate_and_verify(&graph);
    let argument = find_node(&graph, |node| node.op == Op::LoadSlot { slot: a(0).raw() });
    assert!(allocation.node(argument).spill_slot.is_some());
    assert_text(
        &dump_allocation(&graph, &allocation),
        "
        b0:
          v0 Jump
        b1:
          v1 LoadSlot -> r0 spill s0
          v2 LoadSlot -> r1
          v3 CallSlowPath (r1, r1, r0) -> r0 exit {6=s0}
          moves s0 -> r0
          v4 Return (r0)
        spill slots: 1
        ",
    );
}

#[test]
fn many_simultaneously_live_values_are_spilled() {
    let layout = FrameLayout {
        number_of_registers: 16,
        registers_and_locals_count: 16,
        number_of_constants: 2,
        number_of_arguments: 1,
    };
    let registers = || (5..15).map(Operand::from_raw);
    let program = assemble(|_| {
        let mut instructions = registers()
            .map(|dst| Instruction::GetArgumentCount { dst })
            .collect::<Vec<_>>();
        instructions.extend(registers().map(|environment| Instruction::SetLexicalEnvironment { environment }));
        instructions.push(Instruction::Return {
            value: Operand::from_raw(5),
        });
        instructions
    });
    let graph = build_with_layout(&program, layout, &[]);
    let allocation = allocate_and_verify(&graph);
    // Ten values are live at once, but there are only six registers.
    assert!(allocation.spill_slot_count >= 4);
    let reloads = allocation
        .nodes
        .iter()
        .flat_map(|node| &node.moves_before)
        .filter(|m| matches!(m.from, Location::Stack(_)))
        .count();
    assert!(reloads >= 4);
}

#[test]
fn if_else_phis_and_truthiness_fallbacks() {
    let program = assemble(|label| {
        vec![
            Instruction::Mov { dst: l(1), src: a(0) },
            Instruction::JumpIf {
                condition: r(5),
                true_target: label(2),
                false_target: label(4),
            },
            Instruction::Mov { dst: l(0), src: c(0) },
            Instruction::Jump { target: label(5) },
            Instruction::Mov { dst: l(0), src: l(1) },
            Instruction::Not { dst: r(6), src: l(0) },
            Instruction::Return { value: r(6) },
        ]
    });
    let graph = build(&program);
    allocate_and_verify(&graph);
}

#[test]
fn nested_loops_with_generic_nodes() {
    let program = assemble(|label| {
        vec![
            Instruction::Enter,
            Instruction::Mov { dst: r(5), src: c(0) },
            Instruction::Mov { dst: l(1), src: a(0) },
            Instruction::JumpLessThan {
                arith_feedback: 0,
                lhs: r(5),
                rhs: l(1),
                true_target: label(4),
                false_target: label(11),
            },
            Instruction::Mov { dst: r(6), src: c(0) },
            Instruction::JumpLessThan {
                arith_feedback: 0,
                lhs: r(6),
                rhs: l(1),
                true_target: label(6),
                false_target: label(9),
            },
            Instruction::Increment {
                arith_feedback: 0,
                dst: r(6),
            },
            Instruction::Mov { dst: l(0), src: r(6) },
            Instruction::Jump { target: label(5) },
            Instruction::Increment {
                arith_feedback: 0,
                dst: r(5),
            },
            Instruction::Jump { target: label(3) },
            Instruction::Add {
                arith_feedback: 0,
                dst: r(7),
                lhs: r(5),
                rhs: l(1),
            },
            Instruction::Return { value: r(7) },
        ]
    });
    let graph = build(&program);
    allocate_and_verify(&graph);
}

#[test]
fn swapping_loop_phis_go_through_a_temp_slot() {
    let program = assemble(|label| {
        vec![
            Instruction::Enter,
            Instruction::Mov { dst: l(0), src: c(0) },
            Instruction::Mov { dst: l(1), src: c(1) },
            Instruction::JumpIf {
                condition: l(0),
                true_target: label(4),
                false_target: label(8),
            },
            Instruction::Mov { dst: r(5), src: l(0) },
            Instruction::Mov { dst: l(0), src: l(1) },
            Instruction::Mov { dst: l(1), src: r(5) },
            Instruction::Jump { target: label(3) },
            Instruction::Return { value: l(1) },
        ]
    });
    let graph = build(&program);
    let allocation = allocate_and_verify(&graph);
    let header = graph.blocks.iter().position(|block| block.is_loop_header).unwrap();
    let back_edge = graph.blocks[header].predecessors[1];
    // NB: The back edge's block starts with the phis in their registers,
    //     like the merge it is, so the swap goes through a temp slot.
    let moves = &allocation.block_end_moves[back_edge.index()];
    assert_eq!(moves.len(), 3, "{moves:?}");
    assert!(moves.iter().any(|m| matches!(m.to, Location::Stack(_))), "{moves:?}");
    // The loop header starts with the registers of the block entering the
    // loop: the condition, a phi, stays in r0, with no reload in the loop.
    assert_text(
        &dump_allocation(&graph, &allocation),
        "
        b0:
          v0 Jump
        b1:
          moves #0 -> r0, #10 -> r1
          v6 Jump
        b2:
          v7 Phi -> r0
          v8 Phi -> r1
          v9 BranchTruthy (r0)
        b3:
          v10 Jump
        b4:
          v11 Jump
        b5:
          moves r0 -> r1
          v12 ToBoolean (r1) -> r0
          v13 Branch (r0)
        b6:
          moves s0 -> r0, s1 -> r1
          v14 Jump
        b7:
          moves s1 -> r1
          v15 Jump
        b8:
          moves r1 -> r0
          v16 Return (r0)
        b9:
          moves r0 -> s2, r1 -> r0, s2 -> r1
          v17 Jump
        spill slots: 3
        ",
    );
}

#[test]
fn loop_phis_share_spill_slots_with_their_inputs() {
    // Values carried around a loop with calls in it need stack slots. Each
    // phi shares its slot with the value flowing into it over the back edge,
    // so that nothing moves between slots where both are in theirs.
    let program = assemble(|label| {
        vec![
            Instruction::Enter,
            Instruction::Mov { dst: l(0), src: a(0) },
            Instruction::Increment {
                arith_feedback: 0,
                dst: l(0),
            },
            Instruction::Mov { dst: l(1), src: l(0) },
            Instruction::Increment {
                arith_feedback: 0,
                dst: l(1),
            },
            Instruction::Mov { dst: r(6), src: l(1) },
            Instruction::Increment {
                arith_feedback: 0,
                dst: r(6),
            },
            Instruction::JumpIf {
                condition: l(0),
                true_target: label(8),
                false_target: label(12),
            },
            Instruction::Increment {
                arith_feedback: 0,
                dst: l(0),
            },
            Instruction::Increment {
                arith_feedback: 0,
                dst: l(1),
            },
            Instruction::Increment {
                arith_feedback: 0,
                dst: r(6),
            },
            Instruction::Jump { target: label(7) },
            Instruction::Add {
                arith_feedback: 0,
                dst: r(5),
                lhs: l(0),
                rhs: l(1),
            },
            Instruction::Add {
                arith_feedback: 0,
                dst: r(5),
                lhs: r(5),
                rhs: r(6),
            },
            Instruction::Return { value: r(5) },
        ]
    });
    let graph = build(&program);
    let allocation = allocate_and_verify(&graph);
    let header = graph.blocks.iter().position(|block| block.is_loop_header).unwrap();
    let back_edge = graph.blocks[header]
        .predecessors
        .iter()
        .position(|predecessor| predecessor.index() > header)
        .unwrap();
    let (mut shared, mut filled) = (0, 0);
    for phi in &graph.blocks[header].phis {
        let input = graph.node(*phi).inputs[back_edge];
        let (phi_slot, input_slot) = (
            allocation.nodes[phi.index()].spill_slot,
            allocation.nodes[input.index()].spill_slot,
        );
        if let (Some(phi_slot), Some(input_slot)) = (phi_slot, input_slot) {
            assert_eq!(phi_slot, input_slot, "{}", dump_allocation(&graph, &allocation));
            shared += 1;
        }
        // NB: Where all inputs fill the slot, the phi needs no store to it.
        if graph
            .node(*phi)
            .inputs
            .iter()
            .all(|input| allocation.nodes[input.index()].spill_slot == phi_slot)
        {
            assert_eq!(phi_spill_store(&graph, &allocation, *phi), None);
            filled += 1;
        }
    }
    assert_eq!(shared, 2, "{}", dump_allocation(&graph, &allocation));
    assert!(filled > 0, "{}", dump_allocation(&graph, &allocation));
}

#[test]
fn loops_leave_the_values_they_only_carry_through_in_their_stack_slots() {
    // r(5) to r(9) are live across the call, back in registers before the loop and used only after it. With them in
    // registers, the six registers would not hold the counter, the bound and `last` as well. The loop leaves them in
    // their stack slots, so that its back edge neither reloads them nor stores the counter. r(11) is live across the
    // call too, and the loop only uses it as the input of `last`'s phi: it keeps its register.
    let layout = FrameLayout {
        number_of_registers: 16,
        registers_and_locals_count: 18,
        number_of_constants: 0,
        number_of_arguments: 1,
    };
    let r = Operand::from_raw;
    let (counter, last, argument) = (Operand::from_raw(16), Operand::from_raw(17), Operand::from_raw(18));
    let xor = |arith_feedback, dst, lhs, rhs| Instruction::BitwiseXor {
        arith_feedback,
        dst,
        lhs,
        rhs,
    };
    let program = assemble(|label| {
        vec![
            Instruction::Enter,
            xor(0, r(5), argument, argument),
            xor(1, r(6), r(5), argument),
            xor(2, r(7), r(6), argument),
            xor(3, r(8), r(7), argument),
            xor(4, r(9), r(8), argument),
            xor(14, r(11), r(9), argument),
            Instruction::Call {
                value_feedback: 0,
                call_feedback: 0,
                dst: r(14),
                callee: r(15),
                this_value: r(15),
                argument_count: 0,
                expression_string: None,
                arguments: vec![],
            },
            xor(5, r(10), r(5), r(6)),
            xor(6, r(12), r(7), r(11)),
            Instruction::Mov {
                dst: counter,
                src: r(10),
            },
            Instruction::Mov { dst: last, src: r(5) },
            Instruction::JumpLessThan {
                arith_feedback: 8,
                lhs: counter,
                rhs: r(12),
                true_target: label(13),
                false_target: label(16),
            },
            Instruction::Increment {
                arith_feedback: 9,
                dst: counter,
            },
            Instruction::Mov { dst: last, src: r(11) },
            Instruction::Jump { target: label(12) },
            xor(10, r(13), r(5), r(6)),
            xor(11, r(13), r(13), r(7)),
            xor(12, r(13), r(13), r(8)),
            xor(13, r(13), r(13), r(9)),
            xor(15, r(13), r(13), last),
            Instruction::Return { value: r(13) },
        ]
    });
    let mut snapshot = crate::builder::tests::snapshot_for(&program, layout);
    snapshot.executables[0].feedback.arith = vec![crate::snapshot::arith_feedback::INT32; 16];
    let graph = crate::builder::tests::optimized_graph(&snapshot);
    let allocation = allocate_and_verify(&graph);
    let header = graph.blocks.iter().position(|block| block.is_loop_header).unwrap();
    assert_eq!(
        graph.blocks[header].phis.len(),
        2,
        "{}",
        dump_allocation(&graph, &allocation)
    );
    let back_edge = *graph.blocks[header]
        .predecessors
        .iter()
        .find(|predecessor| predecessor.index() > header)
        .unwrap();
    assert!(
        allocation.block_end_moves[back_edge.index()]
            .iter()
            .all(|m| !matches!(m.from, Location::Stack(_)) && !matches!(m.to, Location::Stack(_))),
        "{}",
        dump_allocation(&graph, &allocation)
    );
    for phi in &graph.blocks[header].phis {
        assert!(
            matches!(allocation.node(*phi).output, Some(Location::Register(_))),
            "{}",
            dump_allocation(&graph, &allocation)
        );
    }
}

#[test]
fn int32_operations_write_over_their_first_input_where_it_dies() {
    // The first XOR's value dies at the second, whose output then takes its
    // register: two-address instructions need no copy of it.
    let program = assemble(|_| {
        vec![
            Instruction::Enter,
            Instruction::BitwiseXor {
                arith_feedback: 0,
                dst: r(5),
                lhs: a(0),
                rhs: a(0),
            },
            Instruction::BitwiseXor {
                arith_feedback: 1,
                dst: r(6),
                lhs: r(5),
                rhs: a(0),
            },
            Instruction::Return { value: r(6) },
        ]
    });
    let mut snapshot = crate::builder::tests::snapshot_for(&program, crate::bytecode::test_support::test_layout());
    snapshot.executables[0].feedback.arith = vec![crate::snapshot::arith_feedback::INT32; 2];
    let graph = crate::builder::tests::optimized_graph(&snapshot);
    let allocation = allocate_and_verify(&graph);
    let xors = graph
        .nodes
        .iter()
        .enumerate()
        .filter(|(_, node)| {
            node.op
                == Op::Int32Binary {
                    op: crate::ir::BinaryOp::BitwiseXor,
                }
        })
        .map(|(index, _)| index)
        .collect::<Vec<_>>();
    assert_eq!(xors.len(), 2, "{}", dump_allocation(&graph, &allocation));
    let second = &allocation.nodes[xors[1]];
    assert_eq!(
        second.output,
        Some(second.inputs[0]),
        "{}",
        dump_allocation(&graph, &allocation)
    );
}

#[test]
fn exits_find_constants_registers_and_spilled_values() {
    let program = assemble(|_| {
        vec![
            Instruction::Mov { dst: r(5), src: c(1) },
            Instruction::Mov { dst: r(6), src: a(0) },
            Instruction::Add {
                arith_feedback: 0,
                dst: r(7),
                lhs: r(5),
                rhs: r(6),
            },
            Instruction::Return { value: r(7) },
        ]
    });
    let graph = build_with_layout(&program, test_layout(), &[2]);
    let allocation = allocate_and_verify(&graph);
    let exit = find_node(&graph, |node| matches!(node.op, Op::Exit { .. }));
    let descriptor = crate::codegen::site(
        &graph,
        &allocation,
        exit,
        SiteKind::Exit(ExitKind::NoFeedback),
        &|slot| -8 * (i32::try_from(slot).unwrap() + 1),
    );
    let argument = find_node(&graph, |node| node.op == Op::LoadSlot { slot: a(0).raw() });
    let Some(Location::Register(argument_register)) = allocation.node(argument).output else {
        panic!("the argument is in a register");
    };
    assert_eq!(
        descriptor,
        Site {
            kind: SiteKind::Exit(ExitKind::NoFeedback),
            frames: vec![CodeFrameState {
                executable: 0,
                pc: program.offsets[2],
                mode: crate::code::ResumeMode::ResumeAt,
                values: vec![
                    (r(5).raw(), ValueLocation::Constant(value::int32(10))),
                    (r(6).raw(), ValueLocation::Register(argument_register, Repr::Tagged)),
                ],
                in_frame: Vec::new(),
                passed_argument_count: None,
                legacy_arguments: 0..0,
            }],
            objects: Vec::new(),
            inlined_frame_bytes: 0,
        }
    );
}

#[test]
fn the_verifier_finds_values_in_the_wrong_place() {
    let program = assemble(|_| {
        vec![
            Instruction::Mov { dst: r(6), src: a(0) },
            Instruction::Call {
                value_feedback: 0,
                call_feedback: 0,
                dst: r(7),
                callee: r(5),
                this_value: r(5),
                argument_count: 1,
                expression_string: None,
                arguments: vec![r(6)],
            },
            Instruction::Return { value: r(6) },
        ]
    });
    let graph = build(&program);
    let registers = RegisterSet::test_configuration();
    let allocation = allocate(&graph, &registers);
    verify(&graph, &registers, &allocation).unwrap();

    // Forgetting to spill a value that is live across a call loses it.
    let argument = find_node(&graph, |node| node.op == Op::LoadSlot { slot: a(0).raw() });
    let mut broken = allocation.clone();
    broken.nodes[argument.index()].spill_slot = None;
    assert!(verify(&graph, &registers, &broken).is_err());

    // Reading an input from a register that holds something else.
    let call = find_node(&graph, |node| matches!(node.op, Op::CallSlowPath { .. }));
    let mut broken = allocation;
    let Location::Register(register) = broken.nodes[call.index()].inputs[2] else {
        panic!("the argument is in a register");
    };
    broken.nodes[call.index()].inputs[2] = Location::Register((register + 1) % 6);
    assert!(verify(&graph, &registers, &broken).is_err());
}

/// A tiny deterministic random number generator (xorshift64).
pub(crate) struct Random(pub(crate) u64);

impl Random {
    pub(crate) fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }

    pub(crate) fn below(&mut self, bound: usize) -> usize {
        (self.next() % bound as u64) as usize
    }

    pub(crate) fn chance(&mut self, percent: usize) -> bool {
        self.below(100) < percent
    }
}

/// An instruction whose jump targets are instruction indices.
#[derive(Clone)]
enum Item {
    Plain(Instruction),
    Jump(usize),
    JumpIf(Operand, usize, usize),
    JumpTrue(Operand, usize),
    JumpFalse(Operand, usize),
    JumpNullish(Operand, usize, usize),
    JumpLessThan(Operand, Operand, usize, usize),
}

/// Generates structured random programs: nested ifs and loops around moves,
/// native and generic instructions, calls and returns.
pub(crate) struct ProgramGenerator {
    random: Random,
    layout: FrameLayout,
    items: Vec<Item>,
    /// Whether to emit GetById (with property caches 0 and 1) and PutById
    /// (with property caches 2 and 3).
    property_access: bool,
}

impl ProgramGenerator {
    fn writable(&mut self) -> Operand {
        let layout = self.layout;
        let count = layout.registers_and_locals_count - 5 + layout.number_of_arguments;
        let index = 5 + u32::try_from(self.random.below(count as usize)).unwrap();
        if index < layout.registers_and_locals_count {
            Operand::from_raw(index)
        } else {
            Operand::from_raw(layout.arguments_base() + index - layout.registers_and_locals_count)
        }
    }

    fn readable(&mut self) -> Operand {
        match self.random.below(10) {
            0 => Operand::from_raw(u32::try_from(self.random.below(5)).unwrap()),
            1 | 2 => Operand::from_raw(
                self.layout.constants_base()
                    + u32::try_from(self.random.below(self.layout.number_of_constants as usize)).unwrap(),
            ),
            _ => self.writable(),
        }
    }

    /// Usually an argument, where the property access tests pass objects.
    fn object_operand(&mut self) -> Operand {
        if self.random.chance(80) {
            let argument = u32::try_from(self.random.below(self.layout.number_of_arguments as usize)).unwrap();
            Operand::from_raw(self.layout.arguments_base() + argument)
        } else {
            self.readable()
        }
    }

    fn plain(&mut self) -> Instruction {
        if self.property_access && self.random.chance(30) {
            let cache = u32::try_from(self.random.below(2)).unwrap();
            return if self.random.chance(60) {
                Instruction::GetById {
                    value_feedback: 0,
                    dst: self.writable(),
                    base: self.object_operand(),
                    property: crate::bytecode::PropertyKeyTableIndex(0),
                    base_identifier: None,
                    cache,
                }
            } else {
                Instruction::PutById {
                    base: self.object_operand(),
                    property: crate::bytecode::PropertyKeyTableIndex(0),
                    src: self.readable(),
                    kind: 0,
                    cache: 2 + cache,
                    base_identifier: None,
                }
            };
        }
        match self.random.below(13) {
            0..=2 => Instruction::Mov {
                dst: self.writable(),
                src: self.readable(),
            },
            3 => Instruction::Mov2 {
                c0_dst: self.writable(),
                c0_src: self.readable(),
                c1_dst: self.writable(),
                c1_src: self.readable(),
            },
            4 => Instruction::MovSrcUndefined { dst: self.writable() },
            5 => Instruction::Add {
                arith_feedback: 0,
                dst: self.writable(),
                lhs: self.readable(),
                rhs: self.readable(),
            },
            6 => Instruction::Increment {
                arith_feedback: 0,
                dst: self.writable(),
            },
            7 => Instruction::Not {
                dst: self.writable(),
                src: self.readable(),
            },
            8 => Instruction::GetLexicalEnvironment { dst: self.writable() },
            9 => Instruction::IsCallable {
                dst: self.writable(),
                value: self.readable(),
            },
            10 => Instruction::SetLexicalEnvironment {
                environment: self.readable(),
            },
            11 => {
                let argument_count = self.random.below(3);
                Instruction::Call {
                    value_feedback: 0,
                    call_feedback: 0,
                    dst: self.writable(),
                    callee: self.readable(),
                    this_value: self.readable(),
                    argument_count: u32::try_from(argument_count).unwrap(),
                    expression_string: None,
                    arguments: (0..argument_count).map(|_| self.readable()).collect(),
                }
            }
            _ => Instruction::ToBoolean {
                dst: self.writable(),
                value: self.readable(),
            },
        }
    }

    fn statements(&mut self, depth: usize) {
        for _ in 0..1 + self.random.below(5) {
            match self.random.below(10) {
                0 | 1 if depth < 4 => self.if_else(depth + 1),
                2 if depth < 4 => self.while_loop(depth + 1),
                3 if self.random.chance(15) => {
                    let value = self.readable();
                    self.items.push(Item::Plain(Instruction::Return { value }));
                }
                _ => {
                    let instruction = self.plain();
                    self.items.push(Item::Plain(instruction));
                }
            }
        }
    }

    fn if_else(&mut self, depth: usize) {
        let condition = self.readable();
        let branch = self.items.len();
        self.items.push(Item::Jump(0));
        let then_start = self.items.len();
        self.statements(depth);
        let jump_to_end = self.items.len();
        self.items.push(Item::Jump(0));
        let else_start = self.items.len();
        self.statements(depth);
        let end = self.items.len();
        self.items[jump_to_end] = Item::Jump(end);
        self.items[branch] = match self.random.below(5) {
            0 => Item::JumpIf(condition, then_start, else_start),
            1 => Item::JumpFalse(condition, else_start),
            2 => Item::JumpTrue(condition, else_start),
            3 => Item::JumpNullish(condition, then_start, else_start),
            _ => Item::JumpLessThan(condition, self.readable(), then_start, else_start),
        };
    }

    fn while_loop(&mut self, depth: usize) {
        let header = self.items.len();
        let condition = self.readable();
        self.items.push(Item::Jump(0));
        let body = self.items.len();
        self.statements(depth);
        self.items.push(Item::Jump(header));
        let end = self.items.len();
        self.items[header] = match self.random.below(3) {
            0 => Item::JumpFalse(condition, end),
            1 => Item::JumpIf(condition, body, end),
            _ => Item::JumpLessThan(condition, self.readable(), body, end),
        };
    }

    pub(crate) fn generate(seed: u64, layout: FrameLayout) -> Program {
        Self::generate_with(seed, layout, false)
    }

    pub(crate) fn generate_with(seed: u64, layout: FrameLayout, property_access: bool) -> Program {
        let mut generator = Self {
            random: Random(seed),
            layout,
            items: vec![Item::Plain(Instruction::Enter)],
            property_access,
        };
        generator.statements(0);
        let value = generator.readable();
        generator.items.push(Item::Plain(Instruction::Return { value }));
        let items = generator.items;
        assemble(|label| {
            items
                .iter()
                .map(|item| match item.clone() {
                    Item::Plain(instruction) => instruction,
                    Item::Jump(target) => Instruction::Jump { target: label(target) },
                    Item::JumpIf(condition, if_true, if_false) => Instruction::JumpIf {
                        condition,
                        true_target: label(if_true),
                        false_target: label(if_false),
                    },
                    Item::JumpTrue(condition, target) => Instruction::JumpTrue {
                        condition,
                        target: label(target),
                    },
                    Item::JumpFalse(condition, target) => Instruction::JumpFalse {
                        condition,
                        target: label(target),
                    },
                    Item::JumpNullish(condition, if_true, if_false) => Instruction::JumpNullish {
                        condition,
                        true_target: label(if_true),
                        false_target: label(if_false),
                    },
                    Item::JumpLessThan(lhs, rhs, if_true, if_false) => Instruction::JumpLessThan {
                        arith_feedback: 0,
                        lhs,
                        rhs,
                        true_target: label(if_true),
                        false_target: label(if_false),
                    },
                })
                .collect()
        })
    }
}

/// Marks `blocks` cold and lays the graph out like the passes do.
fn make_cold(graph: &mut Graph, blocks: &[BlockId]) {
    for block in blocks {
        graph.blocks[block.index()].is_cold = true;
    }
    crate::passes::edit::move_cold_blocks_last(graph);
    if let Err(errors) = crate::passes::verify::verify(graph) {
        panic!("{errors}\n{}", crate::ir::dump(graph, &test_layout()));
    }
}

#[test]
fn cold_blocks_cost_the_code_around_them_no_spills() {
    // The then branch is cold: the value in l(1), live across it, and the
    // phi of l(0) at the join stay in registers.
    let program = assemble(|label| {
        vec![
            Instruction::Mov { dst: l(1), src: a(0) },
            Instruction::JumpNullish {
                condition: r(5),
                true_target: label(2),
                false_target: label(4),
            },
            Instruction::Mov { dst: l(0), src: c(0) },
            Instruction::Jump { target: label(5) },
            Instruction::Mov { dst: l(0), src: r(5) },
            Instruction::Add {
                arith_feedback: 0,
                dst: r(6),
                lhs: l(0),
                rhs: l(1),
            },
            Instruction::Return { value: r(6) },
        ]
    });
    let mut graph = build(&program);
    let then_block = (0..graph.blocks.len())
        .map(|index| BlockId(u32::try_from(index).unwrap()))
        .find(|block| graph.block(*block).bytecode_start == Some(program.offsets[2]))
        .expect("the then branch has a block");
    make_cold(&mut graph, &[then_block]);
    let join = (0..graph.blocks.len())
        .map(|index| BlockId(u32::try_from(index).unwrap()))
        .find(|block| liveness::state_predecessor(&graph, *block).is_some())
        .expect("the join has a state predecessor");
    let allocation = allocate_and_verify(&graph);
    let kept = find_node(&graph, |node| node.op == Op::LoadSlot { slot: a(0).raw() });
    assert_eq!(allocation.node(kept).spill_slot, None);
    let phi = graph.block(join).phis[0];
    assert!(matches!(allocation.node(phi).output, Some(Location::Register(_))));
    assert_eq!(allocation.node(phi).spill_slot, None);
    assert_eq!(
        allocation.spill_slot_count, 1,
        "only the cycle temp of the cold block's moves"
    );
}

#[test]
fn random_structured_programs_with_cold_blocks_allocate_correctly() {
    let layout = FrameLayout {
        number_of_registers: 12,
        registers_and_locals_count: 16,
        number_of_constants: 2,
        number_of_arguments: 2,
    };
    for seed in 1..=1000u64 {
        let program = ProgramGenerator::generate(seed.wrapping_mul(0x9E37_79B9_7F4A_7C15), layout);
        let mut graph = build_with_layout(&program, layout, &[]);
        let mut random = Random(seed);
        let osr_blocks = graph.osr_entries.iter().map(|(_, block)| *block).collect::<Vec<_>>();
        let cold = (1..graph.blocks.len())
            .map(|index| BlockId(u32::try_from(index).unwrap()))
            .filter(|block| !graph.block(*block).is_loop_header && !osr_blocks.contains(block) && random.chance(25))
            .collect::<Vec<_>>();
        for block in &cold {
            graph.blocks[block.index()].is_cold = true;
        }
        crate::passes::edit::move_cold_blocks_last(&mut graph);
        if let Err(errors) = crate::passes::verify::verify(&graph) {
            panic!("seed {seed}: {errors}\n{}", crate::ir::dump(&graph, &layout));
        }
        let registers = RegisterSet::test_configuration();
        let allocation = allocate(&graph, &registers);
        if let Err(errors) = verify(&graph, &registers, &allocation) {
            panic!(
                "seed {seed}: {errors}\n{}\n{}",
                crate::ir::dump(&graph, &layout),
                dump_allocation(&graph, &allocation)
            );
        }
    }
}

#[test]
fn random_structured_programs_allocate_correctly() {
    let layout = FrameLayout {
        number_of_registers: 12,
        registers_and_locals_count: 16,
        number_of_constants: 2,
        number_of_arguments: 2,
    };
    for seed in 1..=2000u64 {
        let program = ProgramGenerator::generate(seed.wrapping_mul(0x9E37_79B9_7F4A_7C15), layout);
        let mut random = Random(seed);
        let never_ran = (0..program.offsets.len())
            .filter(|_| random.chance(3))
            .collect::<Vec<_>>();
        let graph = build_with_layout(&program, layout, &never_ran);
        let registers = RegisterSet::test_configuration();
        let allocation = allocate(&graph, &registers);
        if let Err(errors) = verify(&graph, &registers, &allocation) {
            panic!(
                "seed {seed}: {errors}\n{}\n{}",
                crate::ir::dump(&graph, &layout),
                dump_allocation(&graph, &allocation)
            );
        }
    }
}

#[test]
fn float64_values_get_floating_point_registers() {
    let program = assemble(|_| {
        vec![
            Instruction::Enter,
            Instruction::Call {
                value_feedback: 0,
                call_feedback: 0,
                dst: r(5),
                callee: r(6),
                this_value: r(7),
                argument_count: 1,
                expression_string: None,
                arguments: vec![a(0)],
            },
            Instruction::Return { value: r(5) },
        ]
    });
    let mut snapshot = crate::builder::tests::snapshot_for(&program, test_layout());
    snapshot.runtime.layout.builtin_math_sqrt = 13;
    snapshot.executables[0].feedback.call = vec![crate::snapshot::CallFeedbackSnapshot {
        target_builtin: Some(13),
        ..Default::default()
    }];
    let mut graph = crate::builder::build_graph_with_feedback(&snapshot, &|_| true).unwrap();
    crate::passes::optimize(&mut graph, &snapshot.executables[0].layout, true);
    let allocation = allocate_and_verify(&graph);
    let mut floats = 0;
    for (index, node) in graph.nodes.iter().enumerate() {
        let output = allocation.nodes[index].output;
        match (node.repr, output) {
            (Some(Repr::Float64), Some(Location::Register(register))) => {
                assert!(register >= FPR_BASE, "v{index} is a float64 in r{register}");
                floats += 1;
            }
            (_, Some(Location::Register(register))) => {
                assert!(
                    register < FPR_BASE,
                    "v{index} is in the floating point register r{register}"
                );
            }
            _ => {}
        }
    }
    assert!(floats >= 2, "{}", dump_allocation(&graph, &allocation));
}
