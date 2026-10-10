/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use crate::bytecode::test_support::*;
use crate::ir::dump;
use crate::snapshot::ExecutableSnapshot;

/// The constants of `test_layout()`: c0 is 0 and c1 is 10.
pub(crate) fn test_constants() -> Vec<u64> {
    vec![value::int32(0), value::int32(10)]
}

/// A snapshot of `program`. Its `bytecode_address` points at the snapshot's
/// own copy of the bytecode.
pub(crate) fn snapshot_for(program: &Program, layout: FrameLayout) -> Snapshot {
    let mut snapshot = Snapshot {
        executables: vec![ExecutableSnapshot {
            bytecode: program.bytes.clone(),
            bytecode_address: 0,
            layout,
            constants: test_constants(),
            exception_handlers: program.handlers.clone(),
            ..ExecutableSnapshot::default()
        }],
        ..Snapshot::default()
    };
    snapshot.executables[0].bytecode_address = snapshot.executables[0].bytecode.as_ptr() as u64;
    snapshot
}

/// Builds the graph of `program` with `test_layout()`. The instructions with
/// the indices in `never_ran` are treated as never having run.
pub(crate) fn build_with_layout(program: &Program, layout: FrameLayout, never_ran: &[usize]) -> Graph {
    let snapshot = snapshot_for(program, layout);
    let never_ran_pcs = never_ran
        .iter()
        .map(|index| program.offsets[*index])
        .collect::<Vec<_>>();
    let mut graph = build_graph_with_feedback(&snapshot, &|pc| !never_ran_pcs.contains(&pc)).unwrap();
    crate::passes::optimize(&mut graph, &layout, true);
    graph
}

/// Builds and optimizes the graph of `snapshot`, like compilation does.
pub(crate) fn optimized_graph(snapshot: &Snapshot) -> Graph {
    let mut graph = build_graph(snapshot).unwrap();
    crate::passes::optimize(&mut graph, &snapshot.executables[0].layout, true);
    graph
}

pub(crate) fn build(program: &Program) -> Graph {
    build_with_layout(program, test_layout(), &[])
}

fn dump_of(program: &Program) -> String {
    dump(&build(program), &test_layout())
}

/// Whether `line` matches `pattern`, where `[[NAME]]` stands for a value,
/// block or frame state name (`v3`, `b2`, `fs0`): the one `NAME` was bound
/// to by an earlier match, or any, which binds `NAME` to it.
fn matches_line(pattern: &str, line: &str, bindings: &mut std::collections::HashMap<String, String>) -> bool {
    let mut new_bindings = bindings.clone();
    let (mut pattern, mut line) = (pattern, line);
    while !pattern.is_empty() {
        if let Some(rest) = pattern.strip_prefix("[[")
            && rest.starts_with(|c: char| c.is_ascii_uppercase())
        {
            let (name, rest) = rest.split_once("]]").expect("names are closed");
            let letters = line.len() - line.trim_start_matches(|c: char| c.is_ascii_lowercase()).len();
            let digits = line[letters..].len() - line[letters..].trim_start_matches(|c: char| c.is_ascii_digit()).len();
            if letters == 0 || digits == 0 {
                return false;
            }
            let (token, after) = line.split_at(letters + digits);
            match new_bindings.get(name) {
                Some(bound) if bound != token => return false,
                Some(_) => {}
                None if new_bindings.values().any(|bound| bound == token) => return false,
                None => {
                    new_bindings.insert(name.to_string(), token.to_string());
                }
            }
            (pattern, line) = (rest, after);
            continue;
        }
        let mut characters = pattern.chars();
        let character = characters.next().expect("the pattern is not empty");
        let Some(after) = line.strip_prefix(character) else {
            return false;
        };
        (pattern, line) = (characters.as_str(), after);
    }
    if !line.is_empty() {
        return false;
    }
    *bindings = new_bindings;
    true
}

/// Checks that the lines of `checks` match lines of `actual` in order (see
/// `matches_line()`), ignoring indentation, so that the dumps tests expect do
/// not depend on how nodes, blocks and frame states are numbered.
#[track_caller]
fn assert_dump(actual: &str, checks: &str) {
    let lines = actual.lines().map(str::trim).collect::<Vec<_>>();
    let mut bindings = std::collections::HashMap::new();
    let mut next = 0;
    for check in checks.lines().map(str::trim).filter(|check| !check.is_empty()) {
        let Some(found) = (next..lines.len()).find(|index| matches_line(check, lines[*index], &mut bindings)) else {
            panic!("no line after line {next} matches '{check}'\nactual:\n{actual}");
        };
        next = found + 1;
    }
}

#[test]
fn dump_checks_bind_each_name_to_one_value() {
    let mut bindings = std::collections::HashMap::new();
    assert!(matches_line(
        "[[A]] = Phi ([[A]], [[B]])",
        "v1 = Phi (v1, v2)",
        &mut bindings
    ));
    assert!(!matches_line("[[B]] = Phi ([[A]])", "v3 = Phi (v1)", &mut bindings));
    assert!(!matches_line("[[C]] = Phi ([[A]])", "v2 = Phi (v1)", &mut bindings));
    assert!(matches_line(
        "Return ([[C]]) [[[F]]]",
        "Return (v3) [fs0]",
        &mut bindings
    ));
}

/// Whether the graph's only `Return` returns the `undefined` of an empty
/// value.
fn returns_undefined_for_empty(graph: &Graph) -> bool {
    let returned = graph
        .blocks
        .iter()
        .filter_map(|block| block.control)
        .filter(|control| graph.node(*control).op == Op::Return)
        .map(|control| graph.node(control).inputs[0])
        .collect::<Vec<_>>();
    assert_eq!(returned.len(), 1);
    graph.node(returned[0]).op == Op::EmptyToUndefined
}

#[test]
fn returns_of_arguments_need_no_check_for_the_empty_value() {
    let returns = |value| assemble(|_| vec![Instruction::Enter, Instruction::Return { value }]);
    assert!(!returns_undefined_for_empty(&build(&returns(a(0)))));
    assert!(returns_undefined_for_empty(&build(&returns(l(0)))));
}

#[test]
fn straight_line_code_renames_moves_and_passes_values_to_slow_paths() {
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
            Instruction::Mov { dst: l(0), src: r(7) },
            Instruction::Return { value: l(0) },
        ]
    });
    assert_dump(
        &dump_of(&program),
        "
        [[B0]]:
          Jump [[B1]]
        [[B1]] (pc 0) <- [[B0]]:
          [[V5]] = LoadSlot a0
          InitializeFrame
          [[V6]] = CallSlowPath Exp @40 (#10, [[V5]]) [[[FS0]]]
          [[V7]] = EmptyToUndefined ([[V6]])
          Return ([[V7]])
        frame states:
          [[FS0]]: pc 40 after -> r7 {} frame [r7]
        ",
    );
}

#[test]
fn if_else_merges_with_phis() {
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
            Instruction::Mov { dst: l(0), src: c(1) },
            Instruction::Exp {
                arith_feedback: 0,
                dst: r(6),
                lhs: l(0),
                rhs: l(1),
            },
            Instruction::Return { value: r(6) },
        ]
    });
    assert_dump(
        &dump_of(&program),
        "
        [[B0]]:
          Jump [[B1]]
        [[B1]] (pc 0) <- [[B0]]:
          [[V1]] = LoadSlot a0
          [[V2]] = LoadSlot r5
          BranchTruthy ([[V2]]) -> [[B2]], [[B3]], fallback [[B4]]
        [[B2]] <- [[B1]]:
          Jump [[B8]]
        [[B3]] <- [[B1]]:
          Jump [[B7]]
        [[B4]] <- [[B1]]:
          [[V6]]:bool = ToBoolean ([[V2]])
          BranchBool ([[V6]]) -> [[B5]], [[B6]]
        [[B5]] <- [[B4]]:
          Jump [[B8]]
        [[B6]] <- [[B4]]:
          Jump [[B7]]
        [[B7]] (pc 56) <- [[B3]], [[B6]]:
          Jump [[B9]]
        [[B8]] (pc 32) <- [[B2]], [[B5]]:
          Jump [[B9]]
        [[B9]] (pc 72) <- [[B7]], [[B8]]:
          [[V14]] = Phi (#10, #0)
          [[V15]] = CallSlowPath Exp @72 ([[V14]], [[V1]]) [[[FS0]]]
          [[V16]] = EmptyToUndefined ([[V15]])
          Return ([[V16]])
        frame states:
          [[FS0]]: pc 72 after -> r6 {} frame [r6]
        ",
    );
}

#[test]
fn writes_to_constant_operands_go_to_the_frame() {
    // The interpreter writes the frame's copy of a constant; later reads see it.
    let program = assemble(|_| {
        vec![
            Instruction::Mov { dst: c(0), src: a(0) },
            Instruction::ToString { dst: c(1), value: c(0) },
            Instruction::Mov { dst: r(5), src: c(1) },
            Instruction::Return { value: r(5) },
        ]
    });
    assert_dump(
        &dump_of(&program),
        "
        [[B0]]:
          Jump [[B1]]
        [[B1]] (pc 0) <- [[B0]]:
          [[V1]] = LoadSlot a0
          StoreSlot c0 ([[V1]])
          [[V3]] = LoadSlot c0
          CallSlowPath ToString @16 ([[V3]]) [[[FS0]]]
          [[V5]] = LoadSlot c1
          [[V6]] = EmptyToUndefined ([[V5]])
          Return ([[V6]])
        frame states:
          [[FS0]]: pc 16 after -> c1 {}
        ",
    );
}

#[test]
fn loop_headers_inside_a_loop_body_add_their_phis_to_it() {
    use crate::bitset::BitSet;
    use crate::bytecode::cfg::Cfg;
    use crate::bytecode::cfg::Loop;

    // Loop 1's header lies in loop 0's body, but loop 1 extends outside it,
    // as `finally` dispatch can make happen. Loop 1 assigns slot 7.
    let assigned = |slots: &[usize]| {
        let mut set = BitSet::new(10);
        slots.iter().for_each(|slot| set.insert(*slot));
        set
    };
    let cfg = Cfg {
        blocks: Vec::new(),
        reverse_post_order: Vec::new(),
        loops: vec![
            Loop {
                header: 1,
                back_edge_sources: vec![3],
                blocks: vec![1, 2, 3],
                assigned_slots: assigned(&[5]),
            },
            Loop {
                header: 2,
                back_edge_sources: vec![4],
                blocks: vec![0, 2, 4],
                assigned_slots: assigned(&[7]),
            },
        ],
    };
    let slots = loop_phi_slots(&cfg);
    assert_eq!(slots[0].iter().collect::<Vec<_>>(), [5, 7]);
    assert_eq!(slots[1].iter().collect::<Vec<_>>(), [7]);
}

#[test]
fn instructions_whose_feedback_is_empty_never_ran() {
    let program = assemble(|_| {
        vec![
            Instruction::Mov { dst: r(5), src: a(0) },
            Instruction::Add {
                arith_feedback: 1,
                dst: r(6),
                lhs: r(5),
                rhs: r(5),
            },
            Instruction::Return { value: r(6) },
        ]
    });
    let compile = |arith: Vec<u8>, exit_sites| {
        let mut snapshot = snapshot_for(&program, test_layout());
        snapshot.executables[0].feedback.arith = arith;
        snapshot.executables[0].exit_sites = exit_sites;
        dump(&optimized_graph(&snapshot), &test_layout())
    };
    let add_pc = program.offsets[1];
    assert!(compile(vec![0, 0], Vec::new()).contains("Exit NoFeedback"));
    assert!(!compile(vec![0, 1], Vec::new()).contains("Exit"));
    // Without feedback to go by, instructions count as having run.
    assert!(!compile(Vec::new(), Vec::new()).contains("Exit"));
    // An instruction that already exited because it looked unexecuted is compiled.
    assert!(!compile(vec![0, 0], vec![(add_pc, crate::code::ExitKind::NoFeedback)]).contains("Exit"));
}

#[test]
fn unsupported_instructions_fail_cleanly() {
    let program = assemble(|label| {
        vec![
            Instruction::Yield {
                continuation_label: Some(label(1)),
                value: r(5),
            },
            Instruction::Return { value: r(5) },
        ]
    });
    let snapshot = snapshot_for(&program, test_layout());
    let failure = build_graph(&snapshot).unwrap_err();
    assert!(matches!(
        failure,
        CompileFailure::UnsupportedInstruction {
            pc: 0,
            opcode: crate::bytecode::OpCode::Yield,
            ..
        }
    ));
}

#[test]
fn nested_loops_get_phis_for_slots_slow_paths_assign() {
    let program = assemble(|label| {
        vec![
            Instruction::Enter,
            Instruction::Mov { dst: r(5), src: c(0) },
            Instruction::JumpLessThan {
                arith_feedback: 0,
                lhs: r(5),
                rhs: a(0),
                true_target: label(3),
                false_target: label(9),
            },
            Instruction::Mov { dst: r(6), src: c(0) },
            Instruction::JumpLessThan {
                arith_feedback: 0,
                lhs: r(6),
                rhs: a(0),
                true_target: label(5),
                false_target: label(7),
            },
            Instruction::Exp {
                arith_feedback: 0,
                dst: r(6),
                lhs: r(6),
                rhs: c(1),
            },
            Instruction::Jump { target: label(4) },
            Instruction::Exp {
                arith_feedback: 0,
                dst: r(5),
                lhs: r(5),
                rhs: c(1),
            },
            Instruction::Jump { target: label(2) },
            Instruction::Return { value: r(5) },
        ]
    });
    assert_dump(
        &dump_of(&program),
        "
        [[B0]]:
          Jump [[B1]]
        [[B1]] (pc 0) <- [[B0]]:
          [[V7]] = LoadSlot a0
          Jump [[B2]]
        [[B2]] (pc 24) <- [[B1]], [[B10]] loop:
          [[V6]] = Phi (#0, [[V20]])
          EnsureFrameInitialized
          [[V8]]:int32 = CallSlowPath JumpLessThan @24 ([[V6]], [[V7]]) [[[FS0]]]
          BranchOnPc @48 ([[V8]]) -> [[B3]], [[B4]]
        [[B3]] <- [[B2]]:
          Jump [[B6]]
        [[B4]] <- [[B2]]:
          Jump [[B5]]
        [[B5]] (pc 136) <- [[B4]]:
          Return ([[V6]])
        [[B6]] (pc 48) <- [[B3]]:
          Jump [[B7]]
        [[B7]] (pc 64) <- [[B6]], [[B11]] loop:
          [[V14]] = Phi (#0, [[V22]])
          [[V15]]:int32 = CallSlowPath JumpLessThan @64 ([[V14]], [[V7]]) [[[FS1]]]
          BranchOnPc @88 ([[V15]]) -> [[B8]], [[B9]]
        [[B8]] <- [[B7]]:
          Jump [[B11]]
        [[B9]] <- [[B7]]:
          Jump [[B10]]
        [[B10]] (pc 112) <- [[B9]]:
          [[V20]] = CallSlowPath Exp @112 ([[V6]], #10) [[[FS2]]]
          Jump [[B2]]
        [[B11]] (pc 88) <- [[B8]]:
          [[V22]] = CallSlowPath Exp @88 ([[V14]], #10) [[[FS3]]]
          Jump [[B7]]
        frame states:
          [[FS0]]: pc 24 after {r5=[[V6]]} frame [a0]
          [[FS1]]: pc 64 after {r5=[[V6]], r6=[[V14]]} frame [a0]
          [[FS2]]: pc 112 after -> r5 {r5=[[V6]]} frame [a0]
          [[FS3]]: pc 88 after -> r6 {r5=[[V6]], r6=[[V14]]} frame [a0]
        ",
    );
}

#[test]
fn truthiness_values_and_tag_branches() {
    let program = assemble(|label| {
        vec![
            Instruction::Not { dst: r(5), src: a(0) },
            Instruction::JumpNullish {
                condition: r(5),
                true_target: label(2),
                false_target: label(3),
            },
            Instruction::Return { value: r(5) },
            Instruction::End { value: a(0) },
        ]
    });
    assert_dump(
        &dump_of(&program),
        "
        [[B0]]:
          Jump [[B1]]
        [[B1]] (pc 0) <- [[B0]]:
          [[V1]] = LoadSlot a0
          BranchTruthy ([[V1]]) -> [[B2]], [[B3]], fallback [[B4]]
        [[B2]] <- [[B1]]:
          Jump [[B7]]
        [[B3]] <- [[B1]]:
          Jump [[B7]]
        [[B4]] <- [[B1]]:
          [[V3]]:bool = ToBoolean ([[V1]])
          BranchBool ([[V3]]) -> [[B5]], [[B6]]
        [[B5]] <- [[B4]]:
          Jump [[B7]]
        [[B6]] <- [[B4]]:
          Jump [[B7]]
        [[B7]] <- [[B2]], [[B3]], [[B5]], [[B6]]:
          [[V11]] = Phi (#false, #true, #false, #true)
          BranchNullish ([[V11]]) -> [[B8]], [[B9]]
        [[B8]] <- [[B7]]:
          Jump [[B11]]
        [[B9]] <- [[B7]]:
          Jump [[B10]]
        [[B10]] (pc 40) <- [[B9]]:
          Return ([[V1]])
        [[B11]] (pc 32) <- [[B8]]:
          Return ([[V11]])
        ",
    );
}

/// `let i = 0; while (i < a0) i = i + 1; return i;`, with arithmetic
/// feedback `feedback` for the comparison and the addition.
#[test]
fn checks_and_conversions_branch_to_cold_slow_paths() {
    // The second check of the same value, and the conversion of a value
    // known to be an object, fold away.
    let program = assemble(|_| {
        vec![
            Instruction::Enter,
            Instruction::ThrowIfNotObject { src: a(0) },
            Instruction::ThrowIfNotObject { src: a(0) },
            Instruction::ToObject { dst: r(5), value: a(0) },
            Instruction::ToLength { dst: r(6), value: r(5) },
            Instruction::Return { value: r(6) },
        ]
    });
    let text = dump_of(&program);
    assert_eq!(text.matches("BranchObject").count(), 1, "{text}");
    assert_eq!(text.matches("CallSlowPath ThrowIfNotObject").count(), 1, "{text}");
    assert!(!text.contains("CallSlowPath ToObject"), "{text}");
    assert!(text.contains("BranchNonNegativeInt32"), "{text}");
    assert!(text.contains("CallSlowPath ToLength"), "{text}");
    assert_eq!(text.matches(" cold:").count(), 2, "{text}");
}

fn counting_loop(feedback: u8, exit_sites: Vec<(u32, ExitKind)>) -> String {
    let program = assemble(|label| {
        vec![
            Instruction::Enter,
            Instruction::Mov { dst: l(0), src: c(0) },
            Instruction::JumpLessThan {
                arith_feedback: 0,
                lhs: l(0),
                rhs: a(0),
                true_target: label(3),
                false_target: label(5),
            },
            Instruction::AddRhsInt32 {
                arith_feedback: 1,
                dst: l(0),
                lhs: l(0),
                rhs: 1,
            },
            Instruction::Jump { target: label(2) },
            Instruction::Return { value: l(0) },
        ]
    });
    let mut snapshot = snapshot_for(&program, test_layout());
    snapshot.executables[0].feedback.arith = vec![feedback, feedback];
    snapshot.executables[0].exit_sites = exit_sites;
    dump(&optimized_graph(&snapshot), &test_layout())
}

#[test]
fn int32_feedback_builds_int32_operations_on_unboxed_phis() {
    let text = counting_loop(1, Vec::new());
    assert!(text.contains(":int32 = Phi (#i32:0, "), "{text}");
    assert!(text.contains("BranchInt32LessThan ("), "{text}");
    assert!(text.contains(":int32 = Int32Add ("), "{text}");
    // The argument is checked once per iteration, the counter never.
    assert_eq!(text.matches("CheckInt32").count(), 1, "{text}");
}

#[test]
fn other_arithmetic_feedback_and_failed_speculations_branch_on_kinds() {
    // Int32 and double operands: int32 and double paths, then the slow path.
    let text = counting_loop(3, Vec::new());
    assert!(
        text.contains("BranchInt32Value") && text.contains("BranchDouble"),
        "{text}"
    );
    assert!(
        text.contains("Int32LessThan") && text.contains("Float64LessThan"),
        "{text}"
    );
    assert!(text.contains("CallSlowPath JumpLessThan"), "{text}");
    // An overflow at the addition: it adds doubles.
    let text = counting_loop(1, vec![(48, ExitKind::Overflow)]);
    assert!(text.contains("Int32LessThan"), "{text}");
    assert!(text.contains("Float64Add") && !text.contains("Int32Add"), "{text}");
}

#[test]
fn strict_equality_with_constants_compares_bits_and_feeds_branches_directly() {
    // `if (a0 === undefined || l1 === undefined) return a0; return l1;`
    let program = assemble(|label| {
        vec![
            Instruction::StrictlyEquals {
                arith_feedback: 0,
                dst: r(5),
                lhs: a(0),
                rhs: c(1),
            },
            Instruction::JumpTrue {
                condition: r(5),
                target: label(3),
            },
            Instruction::StrictlyEquals {
                arith_feedback: 0,
                dst: r(5),
                lhs: l(1),
                rhs: c(1),
            },
            Instruction::JumpIf {
                condition: r(5),
                true_target: label(4),
                false_target: label(5),
            },
            Instruction::Return { value: a(0) },
            Instruction::Return { value: l(1) },
        ]
    });
    let mut snapshot = snapshot_for(&program, test_layout());
    snapshot.executables[0].constants[1] = value::UNDEFINED;
    let text = dump(&optimized_graph(&snapshot), &test_layout());
    assert_eq!(text.matches("TaggedEquals").count(), 2, "{text}");
    assert!(!text.contains("BranchTruthy"), "{text}");
}

#[test]
fn strict_equality_of_identity_comparable_values_compares_bits() {
    // `return a0 === l1`, with feedback that saw no numbers or strings.
    let program = assemble(|_| {
        vec![
            Instruction::StrictlyEquals {
                arith_feedback: 0,
                dst: r(5),
                lhs: a(0),
                rhs: l(1),
            },
            Instruction::Return { value: r(5) },
        ]
    });
    let mut snapshot = snapshot_for(&program, test_layout());
    snapshot.executables[0].feedback.arith = vec![1 << 5];
    let text = dump(&optimized_graph(&snapshot), &test_layout());
    assert!(text.contains("CheckIdentityComparable"), "{text}");
    assert!(text.contains("TaggedEquals"), "{text}");
    // Numbers compare as numbers, other values in the slow path.
    snapshot.executables[0].feedback.arith = vec![(1 << 5) | (1 << 1)];
    let text = dump(&optimized_graph(&snapshot), &test_layout());
    assert!(text.contains("Float64StrictlyEquals"), "{text}");
    assert!(text.contains("CallSlowPath StrictlyEquals"), "{text}");
    assert!(!text.contains("CheckIdentityComparable"), "{text}");
}

#[test]
fn instructions_that_never_ran_exit_with_the_live_values() {
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
    assert_dump(
        &dump(&graph, &test_layout()),
        "
        [[B0]]:
          Jump [[B1]]
        [[B1]] (pc 0) <- [[B0]]:
          [[V2]] = LoadSlot a0
          Exit NoFeedback [[[FS0]]]
        frame states:
          [[FS0]]: pc 32 at {r5=#10, r6=[[V2]]}
        ",
    );
}

#[test]
fn calls_write_back_their_arguments_and_values_survive_them() {
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
            Instruction::Mov { dst: l(0), src: r(7) },
            Instruction::Return { value: r(6) },
        ]
    });
    assert_dump(
        &dump_of(&program),
        "
        [[B0]]:
          Jump [[B1]]
        [[B1]] (pc 0) <- [[B0]]:
          [[V1]] = LoadSlot a0
          [[V2]] = LoadSlot r5
          [[V3]] = CallSlowPath Call @16 ([[V2]], [[V2]], [[V1]]) [[[FS0]]]
          Return ([[V1]])
        frame states:
          [[FS0]]: pc 16 after -> r7 {r6=[[V1]]} frame [r7]
        ",
    );
}

#[test]
fn trivial_frame_operations_are_native() {
    let program = assemble(|_| {
        vec![
            Instruction::GetLexicalEnvironment { dst: r(5) },
            Instruction::MovUndefined2 {
                c0_dst: r(6),
                c1_dst: r(3),
            },
            Instruction::IsCallable { dst: r(7), value: r(5) },
            Instruction::SetLexicalEnvironment { environment: r(5) },
            Instruction::LeavePrivateEnvironment,
            Instruction::Mov3 {
                c0_dst: l(0),
                c0_src: r(7),
                c1_dst: r(7),
                c1_src: r(6),
                c2_dst: l(1),
                c2_src: r(2),
            },
            Instruction::Throw { src: l(0) },
        ]
    });
    assert_dump(
        &dump_of(&program),
        "
        [[B0]]:
          Jump [[B1]]
        [[B1]] (pc 0) <- [[B0]]:
          [[V1]]:pointer = LoadFrameField LexicalEnvironment
          [[V2]] = BoxCell ([[V1]])
          StoreSlot r3 (#undefined)
          [[V5]] = IsCallable ([[V2]])
          [[V6]]:pointer = SetLexicalEnvironment ([[V2]])
          LeavePrivateEnvironment
          CallSlowPath Throw @88 ([[V5]]) [[[FS0]]]
          Unreachable
        frame states:
          [[FS0]]: pc 88 after {}
        ",
    );
}

pub(crate) mod property_access {
    use super::*;
    use crate::bytecode::PropertyKeyTableIndex;
    use crate::code::ExitKind;
    use crate::snapshot::CellId;
    use crate::snapshot::PropertyCacheEntrySnapshot;
    use crate::snapshot::PropertyCacheEntryType;
    use crate::snapshot::PropertyCacheKind;
    use crate::snapshot::PropertyCacheSnapshot;

    pub(crate) fn get_by_id(dst: Operand, base: Operand, cache: u32) -> Instruction {
        Instruction::GetById {
            value_feedback: 0,
            dst,
            base,
            property: PropertyKeyTableIndex(0),
            base_identifier: None,
            cache,
        }
    }

    pub(crate) fn put_by_id(base: Operand, src: Operand, cache: u32) -> Instruction {
        Instruction::PutById {
            base,
            property: PropertyKeyTableIndex(0),
            src,
            kind: 0,
            cache,
            base_identifier: None,
        }
    }

    pub(crate) fn own(entry_type: PropertyCacheEntryType, shape: u64, offset: u32) -> PropertyCacheEntrySnapshot {
        PropertyCacheEntrySnapshot {
            entry_type,
            property_offset: offset,
            shape_dictionary_generation: 0,
            shape_is_dictionary: false,
            shape_is_stable: false,
            writes_data_property: entry_type == PropertyCacheEntryType::ChangeOwnProperty,
            from_shape: None,
            shape: Some(CellId(shape)),
            prototype: None,
            prototype_chain_validity: None,
            prototype_chain_valid: false,
            key: None,
            key_value: 0,
            prototype_property: None,
            prototype_property_intrinsic: None,
            accessor_function: None,
            holds_accessor: false,
        }
    }

    pub(crate) fn cache(entries: Vec<PropertyCacheEntrySnapshot>) -> PropertyCacheSnapshot {
        PropertyCacheSnapshot {
            kind: if entries.len() == 1 {
                PropertyCacheKind::Monomorphic
            } else {
                PropertyCacheKind::Polymorphic
            },
            entries,
        }
    }

    fn dump_with_caches(
        program: &Program,
        caches: Vec<PropertyCacheSnapshot>,
        exit_sites: Vec<(u32, ExitKind)>,
    ) -> String {
        let mut snapshot = snapshot_for(program, test_layout());
        snapshot.executables[0].property_caches = caches;
        snapshot.executables[0].exit_sites = exit_sites;
        snapshot.runtime.heap_region_offset_mask = (1 << 48) - 1;
        dump(&optimized_graph(&snapshot), &test_layout())
    }

    use PropertyCacheEntryType::ChangeOwnProperty;
    use PropertyCacheEntryType::GetOwnProperty;

    #[test]
    fn monomorphic_loads_are_checked_once_and_reused() {
        let program = assemble(|_| {
            vec![
                get_by_id(r(5), a(0), 0),
                get_by_id(r(6), a(0), 0),
                get_by_id(r(7), a(0), 1),
                Instruction::Mov { dst: l(0), src: r(5) },
                Instruction::Mov { dst: l(1), src: r(6) },
                Instruction::Return { value: r(7) },
            ]
        });
        let caches = vec![
            cache(vec![own(GetOwnProperty, 0x100, 2)]),
            cache(vec![own(GetOwnProperty, 0x100, 3)]),
        ];
        assert_dump(
            &dump_with_caches(&program, caches, Vec::new()),
            "
            [[B0]]:
              Jump [[B1]]
            [[B1]] (pc 0) <- [[B0]]:
              [[V1]] = LoadSlot a0
              CheckObject ([[V1]]) [[[FS0]]]
              [[V3]]:pointer = CellAddress ([[V1]])
              CheckShape [0x100] ([[V1]], [[V3]]) [[[FS0]]]
              [[V6]] = LoadNamed +2 ([[V1]], [[V3]]) [[[FS0]]]
              [[V16]] = LoadNamed +3 ([[V1]], [[V3]]) [[[FS1]]]
              [[V17]] = EmptyToUndefined ([[V16]])
              Return ([[V17]])
            frame states:
              [[FS0]]: pc 0 at {} frame [a0]
              [[FS1]]: pc 48 at {r5=[[V6]], r6=[[V6]]} frame [a0]
            ",
        );
    }

    #[test]
    fn polymorphic_loads_check_a_shape_set_or_switch() {
        let program = assemble(|_| {
            vec![
                get_by_id(r(5), a(0), 0),
                get_by_id(r(6), a(0), 1),
                Instruction::Mov { dst: l(0), src: r(5) },
                Instruction::Return { value: r(6) },
            ]
        });
        let caches = vec![
            cache(vec![own(GetOwnProperty, 0x100, 2), own(GetOwnProperty, 0x200, 2)]),
            cache(vec![own(GetOwnProperty, 0x100, 4), own(GetOwnProperty, 0x200, 5)]),
        ];
        assert_dump(
            &dump_with_caches(&program, caches, Vec::new()),
            "
            [[B0]]:
              Jump [[B1]]
            [[B1]] (pc 0) <- [[B0]]:
              [[V1]] = LoadSlot a0
              CheckObject ([[V1]]) [[[FS0]]]
              [[V3]]:pointer = CellAddress ([[V1]])
              CheckShape [0x100, 0x200] ([[V1]], [[V3]]) [[[FS0]]]
              [[V6]] = LoadNamed +2 ([[V1]], [[V3]]) [[[FS0]]]
              ShapeSwitch ([[V1]], [[V3]]) [0x100 -> [[B2]], 0x200 -> [[B3]]] [[[FS1]]]
            [[B2]] <- [[B1]]:
              [[V12]] = LoadNamed +4 ([[V1]], [[V3]]) [[[FS1]]]
              Jump [[B4]]
            [[B3]] <- [[B1]]:
              [[V16]] = LoadNamed +5 ([[V1]], [[V3]]) [[[FS1]]]
              Jump [[B4]]
            [[B4]] <- [[B2]], [[B3]]:
              [[V18]] = Phi ([[V12]], [[V16]])
              [[V19]] = EmptyToUndefined ([[V18]])
              Return ([[V19]])
            frame states:
              [[FS0]]: pc 0 at {} frame [a0]
              [[FS1]]: pc 24 at {r5=[[V6]]} frame [a0]
            ",
        );
    }

    #[test]
    fn shape_switches_on_objects_of_known_shapes_fold() {
        let program = assemble(|_| {
            vec![
                get_by_id(r(5), a(0), 0),
                get_by_id(r(6), a(0), 1),
                Instruction::Mov { dst: l(0), src: r(5) },
                Instruction::Return { value: r(6) },
            ]
        });
        let caches = vec![
            cache(vec![own(GetOwnProperty, 0x100, 2)]),
            cache(vec![own(GetOwnProperty, 0x100, 4), own(GetOwnProperty, 0x200, 5)]),
        ];
        let text = dump_with_caches(&program, caches, Vec::new());
        assert!(!text.contains("ShapeSwitch") && !text.contains("Phi"), "{text}");
        assert!(
            text.contains("LoadNamed +4") && !text.contains("LoadNamed +5"),
            "{text}"
        );
    }

    #[test]
    fn checks_and_loads_in_loops_reuse_what_holds_on_entry() {
        let program = assemble(|label| {
            vec![
                get_by_id(r(5), a(0), 0),
                Instruction::Mov { dst: l(0), src: r(5) },
                get_by_id(r(7), a(0), 0),
                Instruction::JumpUndefined {
                    condition: r(7),
                    true_target: label(2),
                    false_target: label(4),
                },
                Instruction::Return { value: r(7) },
            ]
        });
        let caches = vec![cache(vec![own(GetOwnProperty, 0x100, 2)])];
        let text = dump_with_caches(&program, caches, Vec::new());
        assert_eq!(text.matches("CheckShape").count(), 1, "{text}");
        assert_eq!(text.matches("LoadNamed").count(), 1, "{text}");
    }

    #[test]
    fn invariant_checks_and_loads_at_loop_headers_move_before_the_loop() {
        // `do { x = a0.p } while (x !== undefined)`, with the loop header
        // loading the property.
        let program = assemble(|label| {
            vec![
                Instruction::Mov { dst: l(0), src: c(0) },
                get_by_id(r(7), a(0), 0),
                Instruction::JumpUndefined {
                    condition: r(7),
                    true_target: label(3),
                    false_target: label(1),
                },
                Instruction::Return { value: r(7) },
            ]
        });
        let caches = vec![cache(vec![own(GetOwnProperty, 0x100, 2)])];
        let text = dump_with_caches(&program, caches, Vec::new());
        let header = text.find(" loop:").expect("the loop has a header");
        for hoisted in ["CheckObject", "CheckShape", "LoadNamed"] {
            assert!(
                text.find(hoisted).is_some_and(|position| position < header),
                "{hoisted}: {text}"
            );
        }
    }

    #[test]
    fn prototype_chain_loads_check_the_chain_and_load_from_the_holder() {
        let program = assemble(|_| vec![get_by_id(r(5), a(0), 0), Instruction::Return { value: r(5) }]);
        let mut entry = own(PropertyCacheEntryType::GetPropertyInPrototypeChain, 0x100, 1);
        entry.prototype = Some(CellId(0x7000));
        entry.prototype_chain_validity = Some(CellId(0x8000));
        entry.shape_dictionary_generation = 3;
        entry.shape_is_dictionary = true;
        assert_dump(
            &dump_with_caches(&program, vec![cache(vec![entry])], Vec::new()),
            "
            [[B0]]:
              Jump [[B1]]
            [[B1]] (pc 0) <- [[B0]]:
              [[V1]] = LoadSlot a0
              CheckObject ([[V1]]) [[[FS0]]]
              [[V3]]:pointer = CellAddress ([[V1]])
              CheckShape [0x100@3] ([[V1]], [[V3]]) [[[FS0]]]
              CheckPrototypeChainValid 0x8000 [[[FS0]]]
              [[V8]] = LoadNamed +1 (#0xfff9000000007000, #pointer:0x7000) [[[FS0]]]
              [[V9]] = EmptyToUndefined ([[V8]])
              Return ([[V9]])
            frame states:
              [[FS0]]: pc 0 at {} frame [a0]
            ",
        );
    }

    #[test]
    fn valid_prototype_chains_are_depended_on_after_other_code_ran() {
        let program = assemble(|_| {
            vec![
                get_by_id(r(5), a(0), 0),
                Instruction::ToString { dst: r(7), value: a(0) },
                get_by_id(r(6), a(0), 0),
                Instruction::Mov { dst: l(0), src: r(5) },
                Instruction::Return { value: r(6) },
            ]
        });
        let mut entry = own(PropertyCacheEntryType::GetPropertyInPrototypeChain, 0x100, 1);
        entry.prototype = Some(CellId(0x7000));
        entry.prototype_chain_validity = Some(CellId(0x8000));
        entry.prototype_chain_valid = true;
        let mut snapshot = snapshot_for(&program, test_layout());
        snapshot.executables[0].property_caches = vec![cache(vec![entry])];
        let graph = optimized_graph(&snapshot);
        let text = dump(&graph, &test_layout());
        // The code is valid where it starts, so the first load needs nothing,
        // but the second one relies on the chain after other code ran.
        assert!(!text.contains("CheckPrototypeChainValid"), "{text}");
        let generic = text
            .find("CallSlowPath ToString")
            .expect("ToString is a slow path call");
        let assumed = text.find("AssumeValid").expect("the code assumes it is valid");
        assert!(generic < assumed, "{text}");
        assert_eq!(text.matches("AssumeValid").count(), 1, "{text}");
        assert_eq!(
            graph.dependencies,
            vec![crate::code::Dependency::PrototypeChainValid(CellId(0x8000))]
        );
    }

    #[test]
    fn truthiness_and_typeof_depend_on_no_htmldda_objects_existing() {
        let program = assemble(|label| {
            vec![
                Instruction::Typeof { dst: r(5), src: a(0) },
                Instruction::ToString { dst: r(7), value: a(0) },
                Instruction::JumpIf {
                    condition: a(0),
                    true_target: label(3),
                    false_target: label(4),
                },
                Instruction::Return { value: r(5) },
                Instruction::Return { value: r(7) },
            ]
        });
        let mut snapshot = snapshot_for(&program, test_layout());
        snapshot.runtime.layout.typeof_strings = crate::snapshot::TypeofStrings {
            number: 1,
            undefined: 2,
            object: 3,
            string: 4,
            symbol: 5,
            boolean: 6,
            bigint: 7,
            function: 8,
        };
        snapshot.runtime.no_htmldda_objects = true;
        let graph = optimized_graph(&snapshot);
        let text = dump(&graph, &test_layout());
        // Nothing ran before the typeof, but the branch comes after other code.
        assert_eq!(text.matches("AssumeValid").count(), 1, "{text}");
        assert!(text.find("Generic ToString") < text.find("AssumeValid"), "{text}");
        assert_eq!(graph.dependencies, vec![crate::code::Dependency::NoHtmlDdaObjects]);
        snapshot.runtime.no_htmldda_objects = false;
        let graph = optimized_graph(&snapshot);
        assert!(graph.dependencies.is_empty());
    }

    #[test]
    fn prototype_methods_become_constants_checked_once() {
        let program = assemble(|_| {
            vec![
                get_by_id(r(5), a(0), 0),
                get_by_id(r(6), a(0), 0),
                Instruction::Mov { dst: l(0), src: r(5) },
                Instruction::Return { value: r(6) },
            ]
        });
        let mut entry = own(PropertyCacheEntryType::GetPropertyInPrototypeChain, 0x100, 1);
        entry.prototype = Some(CellId(0x7000));
        entry.prototype_chain_validity = Some(CellId(0x8000));
        entry.prototype_property = Some(CellId(0x9000));
        assert_dump(
            &dump_with_caches(&program, vec![cache(vec![entry])], Vec::new()),
            "
            [[B0]]:
              Jump [[B1]]
            [[B1]] (pc 0) <- [[B0]]:
              [[V1]] = LoadSlot a0
              CheckObject ([[V1]]) [[[FS0]]]
              [[V3]]:pointer = CellAddress ([[V1]])
              CheckShape [0x100] ([[V1]], [[V3]]) [[[FS0]]]
              CheckPrototypeChainValid 0x8000 [[[FS0]]]
              [[V9]] = LoadNamed +1 (#0xfff9000000007000, #pointer:0x7000) [[[FS0]]]
              CheckValue 0xfff9000000009000 ([[V9]]) [[[FS0]]]
              Return (#0xfff9000000009000)
            frame states:
              [[FS0]]: pc 0 at {} frame [a0]
            ",
        );
        // Where the method turned out to change, it is loaded like any value.
        let text = dump_with_caches(
            &program,
            vec![cache(vec![entry])],
            vec![
                (0, ExitKind::UnexpectedValue),
                (program.offsets[1], ExitKind::UnexpectedValue),
            ],
        );
        assert!(!text.contains("CheckValue"), "{text}");
    }

    fn get_by_value(dst: Operand, base: Operand, property: Operand) -> Instruction {
        Instruction::GetByValue {
            value_feedback: 0,
            keyed_feedback: 0,
            dst,
            base,
            property,
            base_identifier: None,
            cache: 0,
        }
    }

    /// `return a0[l0] + a0[l0]` with keyed feedback `keyed_bits`, where the
    /// runtime's typed array kind 3 is Uint16Array.
    fn dump_element_loads(keyed_bits: u32, exit_sites: Vec<(u32, ExitKind)>) -> String {
        let program = assemble(|_| {
            vec![
                get_by_value(r(5), a(0), l(0)),
                get_by_value(r(6), a(0), l(0)),
                Instruction::Mov { dst: l(1), src: r(5) },
                Instruction::Return { value: r(6) },
            ]
        });
        let mut snapshot = snapshot_for(&program, test_layout());
        snapshot.executables[0].feedback.keyed = vec![crate::snapshot::KeyedFeedbackSnapshot { bits: keyed_bits }];
        snapshot.executables[0].exit_sites = exit_sites;
        snapshot.runtime.layout.typed_array_kind_uint16 = 3;
        dump(&optimized_graph(&snapshot), &test_layout())
    }

    #[test]
    fn keyed_feedback_of_one_elements_kind_loads_elements_directly() {
        // Int32 indices into Uint16Arrays: the kind, the length and the
        // bounds checked once, loaded unboxed.
        let text = dump_element_loads(1 | (1 << (9 + 3)), Vec::new());
        assert_eq!(text.matches("CheckElements TypedArray(Uint16)").count(), 1, "{text}");
        assert_eq!(text.matches("CheckInt32").count(), 1, "{text}");
        assert_eq!(text.matches("LoadTypedArrayLength").count(), 1, "{text}");
        assert_eq!(text.matches("CheckBounds").count(), 1, "{text}");
        // NB: The first load is dead.
        assert_eq!(
            text.matches(":int32 = LoadElementAt TypedArray(Uint16)").count(),
            1,
            "{text}"
        );
        // Packed and holey arrays are holey, and holes exit.
        let text = dump_element_loads(1 | (1 << 5) | (1 << 6), Vec::new());
        assert!(text.contains("LoadElementAt Holey"), "{text}");
        assert!(text.contains("CheckNotHole"), "{text}");
        // Out of bounds accesses, other keys, several kinds and failed
        // speculations branch on the elements kinds the feedback saw, and
        // take the slow path for anything else.
        for (bits, exit_sites) in [
            (1 | (1 << 5) | (1 << 8), Vec::new()),
            (1 | 2 | (1 << 5), Vec::new()),
            (1 | (1 << 5) | (1 << 9), Vec::new()),
            (
                1 | (1 << 5),
                vec![(0, ExitKind::BadElements), (32, ExitKind::OutOfBounds)],
            ),
        ] {
            let text = dump_element_loads(bits, exit_sites);
            assert!(
                !text.contains("CheckBounds")
                    && text.contains("BranchElementsKind Packed")
                    && text.contains("BranchIndexInBounds")
                    && text.contains("= LoadElementAt Packed")
                    && text.contains("CallSlowPath GetByValue"),
                "{text}"
            );
        }
        // Other keys also try the property lookup cache.
        let text = dump_element_loads(1 | 2 | (1 << 5), Vec::new());
        assert!(
            text.contains("BranchInt32Value") && text.contains("ProbeKeyedCache"),
            "{text}"
        );
        let text = dump_element_loads(1 | (1 << 5) | (1 << 9), Vec::new());
        assert!(!text.contains("ProbeKeyedCache"), "{text}");
    }

    const KEY: u64 = 0xfffa_0000_0000_4000;

    fn keyed(entry_type: PropertyCacheEntryType, offset: u32) -> PropertyCacheEntrySnapshot {
        PropertyCacheEntrySnapshot {
            key: Some(CellId(0x4000)),
            key_value: KEY,
            ..own(entry_type, 0x100, offset)
        }
    }

    fn dump_keyed(
        program: &Program,
        entries: Vec<PropertyCacheEntrySnapshot>,
        exit_sites: Vec<(u32, ExitKind)>,
    ) -> String {
        let mut snapshot = snapshot_for(program, test_layout());
        snapshot.executables[0].constants[1] = KEY;
        snapshot.executables[0].property_caches = vec![cache(entries)];
        snapshot.executables[0].exit_sites = exit_sites;
        snapshot.runtime.heap_region_offset_mask = (1 << 48) - 1;
        dump(&optimized_graph(&snapshot), &test_layout())
    }

    #[test]
    fn keyed_accesses_with_known_keys_are_named_accesses() {
        // A constant key picks its entries.
        let program = assemble(|_| vec![get_by_value(r(5), a(0), c(1)), Instruction::Return { value: r(5) }]);
        let mut other_key = keyed(GetOwnProperty, 3);
        other_key.key_value = KEY + 8;
        assert_dump(
            &dump_keyed(&program, vec![other_key, keyed(GetOwnProperty, 2)], Vec::new()),
            "
            [[B0]]:
              Jump [[B1]]
            [[B1]] (pc 0) <- [[B0]]:
              [[V2]] = LoadSlot a0
              CheckObject ([[V2]]) [[[FS0]]]
              [[V4]]:pointer = CellAddress ([[V2]])
              CheckShape [0x100] ([[V2]], [[V4]]) [[[FS0]]]
              [[V7]] = LoadNamed +2 ([[V2]], [[V4]]) [[[FS0]]]
              [[V8]] = EmptyToUndefined ([[V7]])
              Return ([[V8]])
            frame states:
              [[FS0]]: pc 0 at {} frame [a0]
            ",
        );

        // A key that is not known is checked to be the one key the cache saw.
        let program = assemble(|_| vec![get_by_value(r(5), a(0), r(6)), Instruction::Return { value: r(5) }]);
        let text = dump_keyed(&program, vec![keyed(GetOwnProperty, 2)], Vec::new());
        assert!(text.contains(&format!("CheckValue {KEY:#x}")), "{text}");
        assert!(text.contains("LoadNamed +2"), "{text}");
        let text = dump_keyed(&program, vec![other_key, keyed(GetOwnProperty, 2)], Vec::new());
        assert!(text.contains("ProbeKeyedCache"), "{text}");
        let text = dump_keyed(
            &program,
            vec![keyed(GetOwnProperty, 2)],
            vec![(0, ExitKind::UnexpectedValue)],
        );
        assert!(text.contains("ProbeKeyedCache"), "{text}");

        // Stores of plain assignments.
        let program = assemble(|_| {
            vec![
                Instruction::PutByValue {
                    keyed_feedback: 0,
                    base: a(0),
                    property: c(1),
                    src: c(0),
                    kind: 0,
                    base_identifier: None,
                    cache: 0,
                },
                Instruction::Return { value: a(0) },
            ]
        });
        let text = dump_keyed(&program, vec![keyed(ChangeOwnProperty, 2)], Vec::new());
        assert!(text.contains("StoreNamed +2"), "{text}");
    }

    #[test]
    fn calls_forget_shapes_and_stores_forward_to_loads() {
        let program = assemble(|_| {
            vec![
                put_by_id(a(0), c(1), 0),
                get_by_id(r(5), a(0), 1),
                Instruction::Exp {
                    arith_feedback: 0,
                    dst: l(1),
                    lhs: l(1),
                    rhs: c(1),
                },
                get_by_id(r(6), a(0), 1),
                Instruction::Mov { dst: l(0), src: r(5) },
                Instruction::Return { value: r(6) },
            ]
        });
        let caches = vec![
            cache(vec![own(ChangeOwnProperty, 0x100, 2)]),
            cache(vec![own(GetOwnProperty, 0x100, 2)]),
        ];
        assert_dump(
            &dump_with_caches(&program, caches, Vec::new()),
            "
            [[B0]]:
              Jump [[B1]]
            [[B1]] (pc 0) <- [[B0]]:
              [[V1]] = LoadSlot a0
              CheckObject ([[V1]]) [[[FS0]]]
              [[V4]]:pointer = CellAddress ([[V1]])
              CheckShape [0x100] ([[V1]], [[V4]]) [[[FS0]]]
              StoreNamed +2 ([[V1]], #10, [[V4]]) [[[FS0]]]
              [[V13]] = LoadSlot l1
              [[V14]] = CallSlowPath Exp @56 ([[V13]], #10) [[[FS1]]]
              CheckShape [0x100] ([[V1]], [[V4]]) [[[FS2]]]
              [[V19]] = LoadNamed +2 ([[V1]], [[V4]]) [[[FS2]]]
              [[V20]] = EmptyToUndefined ([[V19]])
              Return ([[V20]])
            frame states:
              [[FS0]]: pc 0 at {} frame [l1, a0]
              [[FS1]]: pc 56 after -> l1 {r5=#10} frame [a0]
              [[FS2]]: pc 72 at {r5=#10} frame [a0]
            ",
        );
    }

    #[test]
    fn unusable_caches_and_failed_speculations_stay_generic() {
        let program = assemble(|_| {
            vec![
                get_by_id(r(5), a(0), 0),
                get_by_id(r(6), a(0), 1),
                get_by_id(r(7), a(0), 2),
                put_by_id(a(0), r(5), 3),
                Instruction::Mov { dst: l(0), src: r(6) },
                Instruction::Return { value: r(7) },
            ]
        });
        let missing = own(PropertyCacheEntryType::GetMissingProperty, 0x100, 1);
        let megamorphic = PropertyCacheSnapshot {
            kind: PropertyCacheKind::Megamorphic,
            entries: vec![own(GetOwnProperty, 0x100, 1)],
        };
        let caches = vec![
            cache(vec![missing]),
            megamorphic,
            cache(vec![own(GetOwnProperty, 0x100, 1)]),
            cache(vec![own(PropertyCacheEntryType::AddOwnProperty, 0x100, 1)]),
        ];
        let exit_sites = vec![(program.offsets[2], ExitKind::BadShape)];
        let dump = dump_with_caches(&program, caches, exit_sites);
        assert!(!dump.contains("Check"), "{dump}");
        // NB: Unspeculated accesses probe the live caches instead.
        assert_eq!(dump.matches("= ProbePropertyCache").count(), 3, "{dump}");
        assert_eq!(dump.matches("= ProbePropertyStore").count(), 1, "{dump}");
    }

    #[test]
    fn properties_holding_accessors_are_not_loaded_as_data() {
        let program = assemble(|_| vec![get_by_id(r(5), a(0), 0), Instruction::Return { value: r(5) }]);
        let accessor = PropertyCacheEntrySnapshot {
            holds_accessor: true,
            ..own(GetOwnProperty, 0x100, 1)
        };
        let dump = dump_with_caches(&program, vec![cache(vec![accessor])], Vec::new());
        assert!(!dump.contains("LoadNamed"), "{dump}");
        assert_eq!(dump.matches("= ProbePropertyCache").count(), 1, "{dump}");
    }
}

pub(crate) mod inlining {
    use super::*;
    use crate::code::ExitKind;
    use crate::snapshot::CallFeedbackSnapshot;
    use crate::snapshot::CellId;
    use crate::snapshot::DirectCallTarget;
    use crate::snapshot::InlinedFunctionSnapshot;
    use crate::snapshot::InliningLimits;

    pub(crate) const CALLEE_FUNCTION: u64 = 0x5000;
    pub(crate) const GLOBAL_THIS: u64 = 0x6000;
    pub(crate) const REALM: u64 = 0x8000;
    pub(crate) const ENVIRONMENT: u64 = 0x9000;

    pub(crate) fn call(dst: Operand, callee: Operand, this_value: Operand, arguments: Vec<Operand>) -> Instruction {
        Instruction::Call {
            value_feedback: 0,
            call_feedback: 0,
            dst,
            callee,
            this_value,
            argument_count: u32::try_from(arguments.len()).unwrap(),
            expression_string: None,
            arguments,
        }
    }

    pub(crate) fn function(strict: bool, uses_this: bool) -> InlinedFunctionSnapshot {
        InlinedFunctionSnapshot {
            function: CellId(CALLEE_FUNCTION),
            formal_parameter_count: 1,
            strict,
            uses_this,
            global_this: CellId(GLOBAL_THIS),
            realm: CellId(REALM),
            shared_data: crate::snapshot::CellId(0x5d00),
        }
    }

    /// A snapshot of `caller` whose call feedback slot 0 offers `callee` (with
    /// `test_layout()`) for inlining.
    pub(crate) fn snapshot_with_callee(
        caller: &Program,
        callee: &Program,
        function: InlinedFunctionSnapshot,
    ) -> Snapshot {
        let mut snapshot = snapshot_for(caller, test_layout());
        snapshot.runtime.heap_region_offset_mask = (1 << 48) - 1;
        snapshot.executables[0].cell = CellId(0x100);
        snapshot.executables[0].feedback.call = vec![CallFeedbackSnapshot {
            target: Some(function.function),
            inline_executable: Some(1),
            ..CallFeedbackSnapshot::default()
        }];
        let mut callee_snapshot = snapshot_for(callee, test_layout()).executables.remove(0);
        callee_snapshot.cell = CellId(0x200);
        callee_snapshot.function = Some(function);
        callee_snapshot.environment = Some(CellId(ENVIRONMENT));
        snapshot.executables.push(callee_snapshot);
        let callee_bytes = snapshot.executables[1].bytecode.as_ptr() as u64;
        snapshot.executables[1].bytecode_address = callee_bytes;
        snapshot.options.inlining = InliningLimits {
            always_inlined_instructions: 10,
            max_construct_instructions: 50,
            max_instructions: 30,
            budget_instructions: 250,
            max_depth: 5,
        };
        snapshot
    }

    fn dump_inlined(caller: &Program, callee: &Program, function: InlinedFunctionSnapshot) -> String {
        let snapshot = snapshot_with_callee(caller, callee, function);
        dump(&optimized_graph(&snapshot), &test_layout())
    }

    /// `return callee(10)` with the callee in a0.
    fn caller() -> Program {
        assemble(|_| {
            vec![
                Instruction::Enter,
                call(r(5), a(0), c(0), vec![c(1)]),
                Instruction::Return { value: r(5) },
            ]
        })
    }

    /// The IR of `return a0.value` whose cache entries call getters that
    /// are closures of the callee `return this`.
    fn dump_getter_call(getters: &[u64], exit_sites: Vec<(u32, ExitKind)>) -> String {
        use super::property_access::{cache, get_by_id, own};
        use crate::snapshot::{AccessorFunctionSnapshot, PropertyCacheEntrySnapshot, PropertyCacheEntryType};
        let program = assemble(|_| vec![get_by_id(r(5), a(0), 0), Instruction::Return { value: r(5) }]);
        let callee = assemble(|_| {
            vec![
                Instruction::Enter,
                Instruction::Return {
                    value: r(crate::bytecode::THIS_VALUE_REGISTER),
                },
            ]
        });
        let mut snapshot = snapshot_with_callee(&program, &callee, function(true, true));
        let entries = getters
            .iter()
            .enumerate()
            .map(|(index, getter)| PropertyCacheEntrySnapshot {
                accessor_function: Some(AccessorFunctionSnapshot {
                    function: CellId(*getter),
                    inline_executable: Some(1),
                }),
                holds_accessor: true,
                ..own(PropertyCacheEntryType::GetOwnProperty, 0x100 + 0x100 * index as u64, 0)
            })
            .collect();
        snapshot.executables[0].property_caches = vec![cache(entries)];
        snapshot.executables[0].exit_sites = exit_sites;
        dump(&optimized_graph(&snapshot), &test_layout())
    }

    #[test]
    fn getters_are_inlined_for_their_function_then_their_executable() {
        let dump = dump_getter_call(&[CALLEE_FUNCTION], Vec::new());
        assert!(dump.contains("CheckAccessorFunction"), "{dump}");
        assert!(!dump.contains("CheckClosure"), "{dump}");

        // Objects of the shape whose accessors have other closures of the
        // getter, once a check of the function failed, or seen by the cache.
        for dump in [
            dump_getter_call(&[CALLEE_FUNCTION], vec![(0, ExitKind::UnexpectedValue)]),
            dump_getter_call(&[CALLEE_FUNCTION, CALLEE_FUNCTION + 0x100], Vec::new()),
        ] {
            assert!(!dump.contains("CheckAccessorFunction"), "{dump}");
            assert!(dump.contains("LoadAccessorFunction"), "{dump}");
            assert!(dump.contains("CheckClosure"), "{dump}");
        }

        let dump = dump_getter_call(
            &[CALLEE_FUNCTION],
            vec![(0, ExitKind::UnexpectedValue), (0, ExitKind::BadCallTarget)],
        );
        assert!(!dump.contains("Accessor"), "{dump}");
    }

    #[test]
    fn native_callees_forward_their_arguments() {
        // function identity(v) { return v; }
        let callee = assemble(|_| vec![Instruction::Enter, Instruction::Return { value: a(0) }]);
        assert_dump(
            &dump_inlined(&caller(), &callee, function(true, false)),
            "
            [[B0]]:
              Jump [[B1]]
            [[B1]] (pc 0) <- [[B0]]:
              [[V4]] = LoadSlot a0
              CheckValue 0xfff9000000005000 ([[V4]]) [[[FS0]]]
              Jump [[B2]]
            [[B2]] (pc 0) <- [[B1]]:
              Jump [[B3]]
            [[B3]] <- [[B2]]:
              Return (#10)
            frame states:
              [[FS0]]: pc 8 at {} frame [a0]
            ",
        );
    }

    #[test]
    fn slow_path_calls_in_callees_take_values_and_run_in_published_frames() {
        // function twice(v) { let x = v + v; return x; }
        let callee = assemble(|_| {
            vec![
                Instruction::Enter,
                Instruction::Exp {
                    arith_feedback: 0,
                    dst: r(5),
                    lhs: a(0),
                    rhs: a(0),
                },
                Instruction::Return { value: r(5) },
            ]
        });
        assert_dump(
            &dump_inlined(&caller(), &callee, function(true, false)),
            "
            [[B0]]:
              Jump [[B1]]
            [[B1]] (pc 0) <- [[B0]]:
              [[V4]] = LoadSlot a0
              CheckValue 0xfff9000000005000 ([[V4]]) [[[FS0]]]
              Jump [[B2]]
            [[B2]] (pc 0) <- [[B1]]:
              PublishFrame
              [[V9]] = CallSlowPath Exp @e1:8 (#10, #10) [[[FS2]]]
              [[V10]] = EmptyToUndefined ([[V9]])
              Jump [[B3]]
            [[B3]] <- [[B2]]:
              Return ([[V10]])
            frame states:
              [[FS0]]: pc 8 at {} frame [a0]
              [[FS1]]: pc 8 after -> r5 {}
              [[FS2]]: e1 pc 8 after -> r5 {r0=#empty, r1=#empty, r2=#empty, r3=#empty, r4=#empty, r5=#empty} in [[FS1]]
            ",
        );

        // A callee's `this` that its caller did not bind is resolved by the
        // slow path, in a cold block.
        let resolves_this = assemble(|_| {
            vec![
                Instruction::Enter,
                Instruction::ResolveThisBinding,
                Instruction::Return { value: r(2) },
            ]
        });
        let text = dump_inlined(&caller(), &resolves_this, function(true, false));
        assert!(text.contains("CallSlowPath ResolveThisBinding @e1:8 [fs2]"), "{text}");
        assert!(!text.contains("Generic"), "{text}");
    }

    #[test]
    fn bindings_and_global_probes_take_the_execution_context_as_inputs() {
        // function closure() { counter = counter + step; return counter; }
        // with `step` a global variable and `counter` a binding of the
        // function's environment.
        let program = |dst: Operand| {
            assemble(move |_| {
                vec![
                    Instruction::Enter,
                    Instruction::GetInitializedBinding {
                        value_feedback: 0,
                        dst: r(5),
                        identifier: crate::bytecode::IdentifierTableIndex(0),
                        cache: crate::bytecode::EnvironmentCoordinate { hops: 1, index: 2 },
                    },
                    Instruction::GetGlobal {
                        value_feedback: 0,
                        dst: r(6),
                        identifier: crate::bytecode::IdentifierTableIndex(1),
                        cache: 0,
                    },
                    Instruction::Add {
                        arith_feedback: 0,
                        dst,
                        lhs: r(5),
                        rhs: r(6),
                    },
                    Instruction::SetLexicalBinding {
                        identifier: crate::bytecode::IdentifierTableIndex(0),
                        src: dst,
                        cache: crate::bytecode::EnvironmentCoordinate { hops: 1, index: 2 },
                    },
                    Instruction::Return { value: dst },
                ]
            })
        };
        // The compiled function reads them from its frame, once, and walks
        // to the binding's environment once.
        assert_dump(
            &dump(&build(&program(r(7))), &test_layout()),
            "
            [[B0]]:
              Jump [[B1]]
            [[B1]] (pc 0) <- [[B0]]:
              [[V4]]:pointer = LoadFrameField LexicalEnvironment
              [[V5]]:pointer = LoadOuterEnvironment ([[V4]])
              [[V6]] = LoadEnvironmentBinding 2 ([[V5]])
              [[V7]]:pointer = LoadFrameField Realm
              [[V8]]:pointer = LoadFrameField Executable
              [[V9]] = ProbeGlobalCache cache 0 ([[V7]], [[V8]])
              BranchTaggedEquals ([[V9]], #empty) -> [[B7]], [[B2]]
            [[B2]] <- [[B1]]:
              Jump [[B3]]
            [[B3]] <- [[B2]], [[B7]]:
              [[V15]] = Phi ([[V9]], [[V12]])
              InitializeFrame
              [[V16]] = CallSlowPath Add @48 ([[V6]], [[V15]]) [[[FS1]]]
              BranchBindingMutable 2 ([[V5]]) -> [[B4]], [[B8]]
            [[B4]] <- [[B3]]:
              [[V20]] = LoadEnvironmentBinding 2 ([[V5]])
              BranchTaggedNotEquals ([[V20]], #empty) -> [[B5]], [[B9]]
            [[B5]] <- [[B4]]:
              StoreEnvironmentBinding 2 ([[V5]], [[V16]])
              Jump [[B6]]
            [[B6]] <- [[B5]], [[B10]]:
              [[V29]] = EmptyToUndefined ([[V16]])
              Return ([[V29]])
            [[B7]] <- [[B1]] cold:
              [[V12]] = CallSlowPath GetGlobal @32 [[[FS0]]]
              Jump [[B3]]
            [[B8]] <- [[B3]] cold:
              Jump [[B10]]
            [[B9]] <- [[B4]] cold:
              Jump [[B10]]
            [[B10]] <- [[B8]], [[B9]] cold:
              CallSlowPath SetLexicalBinding @64 ([[V16]]) [[[FS2]]]
              Jump [[B6]]
            frame states:
              [[FS0]]: pc 32 at {r5=[[V6]]}
              [[FS1]]: pc 48 after -> r7 {} frame [r7]
              [[FS2]]: pc 64 at {r7=[[V16]]}
            ",
        );
        // Inlined callees have constant ones: those of their function.
        assert_dump(
            &dump_inlined(&caller(), &program(r(7)), function(true, false)),
            "
            [[B0]]:
              Jump [[B1]]
            [[B1]] (pc 0) <- [[B0]]:
              [[V4]] = LoadSlot a0
              CheckValue 0xfff9000000005000 ([[V4]]) [[[FS0]]]
              Jump [[B2]]
            [[B2]] (pc 0) <- [[B1]]:
              [[V10]]:pointer = LoadOuterEnvironment (#pointer:0x9000)
              [[V11]] = LoadEnvironmentBinding 2 ([[V10]])
              [[V14]] = ProbeGlobalCache cache 0 (#pointer:0x8000, #pointer:0x200)
              BranchTaggedEquals ([[V14]], #empty) -> [[B9]], [[B3]]
            [[B3]] <- [[B2]]:
              Jump [[B4]]
            [[B4]] <- [[B3]], [[B9]]:
              [[V20]] = Phi ([[V14]], [[V17]])
              PublishFrame
              [[V21]] = CallSlowPath Add @e1:48 ([[V11]], [[V20]]) [[[FS3]]]
              BranchBindingMutable 2 ([[V10]]) -> [[B5]], [[B10]]
            [[B5]] <- [[B4]]:
              [[V25]] = LoadEnvironmentBinding 2 ([[V10]])
              BranchTaggedNotEquals ([[V25]], #empty) -> [[B6]], [[B11]]
            [[B6]] <- [[B5]]:
              StoreEnvironmentBinding 2 ([[V10]], [[V21]])
              Jump [[B7]]
            [[B7]] <- [[B6]], [[B12]]:
              [[V34]] = EmptyToUndefined ([[V21]])
              Jump [[B8]]
            [[B8]] <- [[B7]]:
              Return ([[V34]])
            [[B9]] <- [[B2]] cold:
              PublishFrame
              [[V17]] = CallSlowPath GetGlobal @e1:32 [[[FS2]]]
              Jump [[B4]]
            [[B10]] <- [[B4]] cold:
              Jump [[B12]]
            [[B11]] <- [[B5]] cold:
              Jump [[B12]]
            [[B12]] <- [[B10]], [[B11]] cold:
              CallSlowPath SetLexicalBinding @e1:64 ([[V21]]) [[[FS4]]]
              Jump [[B7]]
            frame states:
              [[FS0]]: pc 8 at {} frame [a0]
              [[FS1]]: pc 8 after -> r5 {}
              [[FS2]]: e1 pc 32 at {r0=#empty, r1=#empty, r2=#empty, r3=#empty, r4=#empty, r5=[[V11]]} in [[FS1]]
              [[FS3]]: e1 pc 48 after -> r7 {r0=#empty, r1=#empty, r2=#empty, r3=#empty, r4=#empty, r7=#empty} in [[FS1]]
              [[FS4]]: e1 pc 64 at {r0=#empty, r1=#empty, r2=#empty, r3=#empty, r4=#empty, r7=[[V21]]} in [[FS1]]
            ",
        );
    }

    #[test]
    fn exits_in_callees_describe_every_frame() {
        let callee = assemble(|_| {
            vec![
                Instruction::Enter,
                Instruction::Mov { dst: l(0), src: a(0) },
                Instruction::Add {
                    arith_feedback: 0,
                    dst: r(5),
                    lhs: l(0),
                    rhs: l(0),
                },
                Instruction::Return { value: r(5) },
            ]
        });
        let caller = assemble(|_| {
            vec![
                Instruction::Enter,
                Instruction::Mov { dst: r(6), src: c(0) },
                call(r(5), a(0), c(0), vec![c(1)]),
                Instruction::Add {
                    arith_feedback: 1,
                    dst: r(7),
                    lhs: r(5),
                    rhs: r(6),
                },
                Instruction::Return { value: r(7) },
            ]
        });
        let mut snapshot = snapshot_with_callee(&caller, &callee, function(true, false));
        // The callee's Add never ran; the caller's did.
        snapshot.executables[1].feedback.arith = vec![0];
        snapshot.executables[0].feedback.arith = vec![0, 1];
        assert_dump(
            &dump(&optimized_graph(&snapshot), &test_layout()),
            "
            [[B0]]:
              Jump [[B1]]
            [[B1]] (pc 0) <- [[B0]]:
              [[V5]] = LoadSlot a0
              CheckValue 0xfff9000000005000 ([[V5]]) [[[FS0]]]
              Jump [[B2]]
            [[B2]] (pc 0) <- [[B1]]:
              Exit NoFeedback [[[FS2]]]
            frame states:
              [[FS0]]: pc 24 at {r6=#0} frame [a0]
              [[FS1]]: pc 24 after -> r5 {r6=#0}
              [[FS2]]: e1 pc 24 at {r0=#empty, r1=#empty, r2=#empty, r3=#empty, r4=#empty, l0=#10} in [[FS1]]
            ",
        );
    }

    #[test]
    fn sloppy_callees_get_the_global_this_or_an_object() {
        let callee = assemble(|_| vec![Instruction::Enter, Instruction::Return { value: r(2) }]);
        let with_this = |this| {
            assemble(move |_| {
                vec![
                    Instruction::Enter,
                    call(r(5), a(0), this, vec![]),
                    Instruction::Return { value: r(5) },
                ]
            })
        };
        // An undefined `this` becomes the global this value.
        let undefined_this = assemble(|_| {
            vec![
                Instruction::MovSrcUndefined { dst: r(6) },
                call(r(5), a(0), r(6), vec![]),
                Instruction::Return { value: r(5) },
            ]
        });
        let dump = dump_inlined(&undefined_this, &callee, function(false, true));
        assert!(dump.contains("Return (#0xfff9000000006000)"), "{dump}");
        // Any other `this` must be an object.
        let dump = dump_inlined(&with_this(r(2)), &callee, function(false, true));
        assert!(dump.contains("CheckObject"), "{dump}");
        // Strict callees take `this` as it is.
        let dump = dump_inlined(&with_this(r(2)), &callee, function(true, true));
        assert!(!dump.contains("CheckObject"), "{dump}");
        assert!(dump.contains("CheckValue"), "{dump}");
    }

    #[test]
    fn callees_keep_their_reserved_registers_across_merges() {
        // function f(v) { if (v) {} return this; }
        let callee = assemble(|label| {
            vec![
                Instruction::Enter,
                Instruction::JumpIf {
                    condition: a(0),
                    true_target: label(2),
                    false_target: label(3),
                },
                Instruction::Jump { target: label(3) },
                Instruction::Return { value: r(2) },
            ]
        });
        let caller = assemble(|_| {
            vec![
                Instruction::Enter,
                call(r(5), a(0), r(2), vec![r(2)]),
                Instruction::Return { value: r(5) },
            ]
        });
        // The caller's `this` is loaded once, and the callee's flows through
        // the merge without being reloaded.
        let dump = dump_inlined(&caller, &callee, function(true, true));
        assert!(dump.contains("v2 = LoadSlot r2\n"), "{dump}");
        assert!(dump.contains("v16 = EmptyToUndefined (v2)\n"), "{dump}");
        assert_eq!(dump.matches("LoadSlot r2").count(), 1, "{dump}");
    }

    #[test]
    fn calls_that_cannot_be_inlined_stay_generic() {
        let identity = assemble(|_| vec![Instruction::Enter, Instruction::Return { value: a(0) }]);
        let is_inlined = |snapshot: &Snapshot| {
            let dump = dump(&optimized_graph(snapshot), &test_layout());
            !dump.contains("CallSlowPath Call")
        };
        assert!(is_inlined(&snapshot_with_callee(
            &caller(),
            &identity,
            function(true, false)
        )));

        // A call target that already turned out wrong here.
        let mut snapshot = snapshot_with_callee(&caller(), &identity, function(true, false));
        snapshot.executables[0].exit_sites = vec![(caller().offsets[1], ExitKind::BadCallTarget)];
        assert!(!is_inlined(&snapshot));

        // Recursion.
        let mut snapshot = snapshot_with_callee(&caller(), &identity, function(true, false));
        snapshot.executables[1].cell = snapshot.executables[0].cell;
        assert!(!is_inlined(&snapshot));

        // A callee too big to inline.
        let big = assemble(|_| {
            let mut instructions = vec![Instruction::Enter];
            instructions.extend((0..40).map(|_| Instruction::Mov { dst: r(5), src: a(0) }));
            instructions.push(Instruction::Return { value: r(5) });
            instructions
        });
        assert!(!is_inlined(&snapshot_with_callee(
            &caller(),
            &big,
            function(true, false)
        )));

        // A callee that needs its frame in memory.
        let needs_environment = assemble(|_| {
            vec![
                Instruction::Enter,
                Instruction::GetLexicalEnvironment { dst: r(5) },
                Instruction::Return { value: r(5) },
            ]
        });
        assert!(!is_inlined(&snapshot_with_callee(
            &caller(),
            &needs_environment,
            function(true, false)
        )));

        // A callee that creates an environment, which its inlined frame
        // would lose.
        let creates_environment = assemble(|_| {
            vec![
                Instruction::Enter,
                Instruction::CreateVariableEnvironment { capacity: 1 },
                Instruction::Return { value: a(0) },
            ]
        });
        assert!(!is_inlined(&snapshot_with_callee(
            &caller(),
            &creates_environment,
            function(true, false)
        )));

        // A for-in loop, which leaves slots in frame memory.
        let for_in = assemble(|_| {
            vec![
                Instruction::Enter,
                Instruction::ObjectPropertyIteratorNext {
                    dst_value: r(5),
                    dst_done: r(6),
                    receiver: a(0),
                    keys: a(0),
                    cursor: r(7),
                },
                Instruction::Return { value: r(5) },
            ]
        });
        assert!(!is_inlined(&snapshot_with_callee(
            &caller(),
            &for_in,
            function(true, false)
        )));
    }

    #[test]
    fn methods_loaded_as_constants_are_inlined_without_a_callee_check() {
        use crate::builder::tests::property_access::cache;
        use crate::builder::tests::property_access::get_by_id;
        use crate::builder::tests::property_access::own;
        use crate::snapshot::PropertyCacheEntryType;

        // this.method(10), with the method on a prototype.
        let caller = assemble(|_| {
            vec![
                Instruction::Enter,
                get_by_id(r(6), a(0), 0),
                call(r(5), r(6), a(0), vec![c(1)]),
                Instruction::Return { value: r(5) },
            ]
        });
        let identity = assemble(|_| vec![Instruction::Enter, Instruction::Return { value: a(0) }]);
        let mut snapshot = snapshot_with_callee(&caller, &identity, function(true, false));
        let mut entry = own(PropertyCacheEntryType::GetPropertyInPrototypeChain, 0x100, 1);
        entry.prototype = Some(CellId(0x7000));
        entry.prototype_chain_validity = Some(CellId(0x7100));
        entry.prototype_property = Some(CellId(CALLEE_FUNCTION));
        snapshot.executables[0].property_caches = vec![cache(vec![entry])];
        let text = dump(&optimized_graph(&snapshot), &test_layout());
        // The method's load is checked; the call of it is not.
        assert_eq!(text.matches("CheckValue").count(), 1, "{text}");
        assert!(text.contains("Return (#10)"), "{text}");
    }

    #[test]
    fn calls_forwarded_through_call_and_bound_functions_are_inlined() {
        use crate::builder::tests::property_access::cache;
        use crate::builder::tests::property_access::get_by_id;
        use crate::builder::tests::property_access::own;
        use crate::snapshot::ForwardedCallSnapshot;
        use crate::snapshot::Forwarding;
        use crate::snapshot::Intrinsic;
        use crate::snapshot::PropertyCacheEntryType;

        let identity = assemble(|_| vec![Instruction::Enter, Instruction::Return { value: a(0) }]);
        let forwarded = |forwarding| ForwardedCallSnapshot {
            forwarding,
            target: CellId(CALLEE_FUNCTION),
            target_intrinsic: None,
            argument_count: 1,
            inline_executable: Some(1),
            bound_this: value::UNDEFINED,
            bound_arguments: [value::int32(7), 0, 0, 0],
            bound_argument_count: 1,
        };

        // a0.call(c0, c1), with `call` found on a prototype.
        let caller = assemble(|_| {
            vec![
                Instruction::Enter,
                get_by_id(r(6), a(0), 0),
                call(r(5), r(6), a(0), vec![c(0), c(1)]),
                Instruction::Return { value: r(5) },
            ]
        });
        let mut snapshot = snapshot_with_callee(&caller, &identity, function(true, false));
        let mut entry = own(PropertyCacheEntryType::GetPropertyInPrototypeChain, 0x100, 1);
        entry.prototype = Some(CellId(0x7000));
        entry.prototype_chain_validity = Some(CellId(0x7100));
        entry.prototype_property = Some(CellId(0x9000));
        entry.prototype_property_intrinsic = Some(Intrinsic::FunctionPrototypeCall);
        snapshot.executables[0].property_caches = vec![cache(vec![entry])];
        snapshot.executables[0].feedback.call[0].target = Some(CellId(0x9000));
        snapshot.executables[0].feedback.call[0].inline_executable = None;
        snapshot.executables[0].feedback.call[0].forwarded = Some(forwarded(Forwarding::Call));
        let text = dump(&optimized_graph(&snapshot), &test_layout());
        // The function `call` is called on is checked, and gets the second argument.
        assert!(text.contains("CheckValue 0xfff9000000005000 (v4)"), "{text}");
        assert!(text.contains("Return (#10)"), "{text}");

        // A bound function in a0 called with c1, bound to 7.
        let caller = assemble(|_| {
            vec![
                Instruction::Enter,
                call(r(5), a(0), c(0), vec![c(1)]),
                Instruction::Return { value: r(5) },
            ]
        });
        let mut snapshot = snapshot_with_callee(&caller, &identity, function(true, false));
        snapshot.executables[0].feedback.call[0].target = Some(CellId(0x9100));
        snapshot.executables[0].feedback.call[0].inline_executable = None;
        snapshot.executables[0].feedback.call[0].forwarded = Some(forwarded(Forwarding::Bound));
        let graph = optimized_graph(&snapshot);
        let text = dump(&graph, &test_layout());
        assert!(text.contains("CheckValue 0xfff9000000009100"), "{text}");
        assert!(text.contains("Return (#7)"), "{text}");
        assert!(graph.embedded_cells.contains(&CellId(0x9100)));
    }

    #[test]
    fn calls_that_are_not_inlined_call_direct_targets_directly() {
        let target = DirectCallTarget {
            function: function(false, true),
            executable: CellId(0x7000),
            entry: 0x7100,
            registers_and_locals_count: 10,
            registers_and_locals_and_constants_count: 12,
            function_fields: crate::snapshot::FunctionFrameFields::default(),
            environment: None,
            closures: false,
        };
        let identity = assemble(|_| vec![Instruction::Enter, Instruction::Return { value: a(0) }]);
        let mut snapshot = snapshot_with_callee(&caller(), &identity, function(true, false));
        snapshot.executables[0].feedback.call[0].direct_call = Some(DirectCallTarget {
            function: function(true, false),
            ..target
        });
        // Inlining comes first.
        let text = dump(&optimized_graph(&snapshot), &test_layout());
        assert!(!text.contains("CallDirect"), "{text}");

        snapshot.executables[0].feedback.call[0].inline_executable = None;
        snapshot.executables[0].feedback.call[0].direct_call = Some(target);
        let graph = optimized_graph(&snapshot);
        let text = dump(&graph, &test_layout());
        // NB: The callee, the `this` value (the sloppy callee reads it) and
        //     the argument are inputs.
        let expected = format!("CallDirect @{} (v4, #0, #10) [fs", caller().offsets[1]);
        assert!(text.contains(&expected), "{text}");
        // NB: The call's value is its result.
        assert!(!text.contains("LoadSlot r5"), "{text}");
        for cell in [CALLEE_FUNCTION, 0x7000, REALM, GLOBAL_THIS] {
            assert!(graph.embedded_cells.contains(&CellId(cell)), "{cell:#x}");
        }

        // Calls with more operands than a direct call takes as inputs in
        // registers take them anywhere, and store them to the frame.
        let many_arguments = assemble(|_| {
            vec![
                Instruction::Enter,
                call(r(5), a(0), c(0), vec![c(1); crate::codegen::DIRECT_CALL_MAX_INPUTS]),
                Instruction::Return { value: r(5) },
            ]
        });
        let mut many = snapshot_with_callee(&many_arguments, &identity, function(true, false));
        many.executables[0].feedback.call[0].inline_executable = None;
        many.executables[0].feedback.call[0].direct_call = Some(target);
        let text = dump(&optimized_graph(&many), &test_layout());
        let expected = format!(
            "CallDirect @{} storing operands (v4, #0, #10, #10, #10, #10, #10, #10) [fs",
            many_arguments.offsets[1]
        );
        assert!(text.contains(&expected), "{text}");

        // Without a target, the call is a call of the runtime's call helper.
        snapshot.executables[0].feedback.call[0].direct_call = None;
        let text = dump(&optimized_graph(&snapshot), &test_layout());
        assert!(text.contains("CallSlowPath Call"), "{text}");
    }
}

mod arguments {
    use super::*;
    use crate::builder::tests::inlining::call;
    use crate::builder::tests::property_access::cache;
    use crate::builder::tests::property_access::get_by_id;
    use crate::builder::tests::property_access::own;
    use crate::snapshot::CellId;
    use crate::snapshot::Intrinsic;
    use crate::snapshot::PropertyCacheEntryType;

    fn create_arguments(mapped: bool) -> Instruction {
        Instruction::CreateArguments {
            dst: Some(l(0)),
            kind: u32::from(!mapped),
            is_immutable: false,
            creates_parameter_bindings: false,
        }
    }

    fn build(program: &Program, configure: impl FnOnce(&mut Snapshot)) -> String {
        let mut snapshot = snapshot_for(program, test_layout());
        snapshot.runtime.heap_region_offset_mask = (1 << 48) - 1;
        configure(&mut snapshot);
        dump(&optimized_graph(&snapshot), &test_layout())
    }

    #[test]
    fn apply_forwards_the_frame_arguments() {
        // a0.method.apply(a0, arguments), with `apply` found on a prototype.
        let program = assemble(|_| {
            vec![
                Instruction::Enter,
                create_arguments(true),
                get_by_id(r(6), a(0), 0),
                get_by_id(r(7), r(6), 1),
                call(r(5), r(7), r(6), vec![a(0), l(0)]),
                Instruction::Return { value: r(5) },
            ]
        });
        let mut apply = own(PropertyCacheEntryType::GetPropertyInPrototypeChain, 0x200, 1);
        apply.prototype = Some(CellId(0x7000));
        apply.prototype_chain_validity = Some(CellId(0x7100));
        apply.prototype_property = Some(CellId(0x7200));
        apply.prototype_property_intrinsic = Some(Intrinsic::FunctionPrototypeApply);
        let text = build(&program, |snapshot| {
            snapshot.executables[0].property_caches = vec![
                cache(vec![own(PropertyCacheEntryType::GetOwnProperty, 0x100, 0)]),
                cache(vec![apply]),
            ];
        });
        assert!(text.contains("CallForwardingArguments"), "{text}");
        assert!(!text.contains("CreateArguments"), "{text}");

        // Without knowing the callee is apply, the object is needed.
        let mut not_apply = apply;
        not_apply.prototype_property_intrinsic = None;
        let text = build(&program, |snapshot| {
            snapshot.executables[0].property_caches = vec![
                cache(vec![own(PropertyCacheEntryType::GetOwnProperty, 0x100, 0)]),
                cache(vec![not_apply]),
            ];
        });
        assert!(text.contains("Generic CreateArguments"), "{text}");
    }

    #[test]
    fn spreads_forward_the_frame_arguments() {
        let program = assemble(|_| {
            vec![
                Instruction::Enter,
                create_arguments(false),
                Instruction::NewArray {
                    dst: r(7),
                    element_count: 0,
                    elements: Vec::new(),
                },
                Instruction::ArrayAppend {
                    dst: r(7),
                    src: l(0),
                    is_spread: true,
                },
                Instruction::CallWithArgumentArray {
                    value_feedback: 0,
                    call_feedback: 0,
                    dst: r(5),
                    callee: a(0),
                    this_value: c(0),
                    arguments: r(7),
                    expression_string: None,
                },
                Instruction::Return { value: r(5) },
            ]
        });
        let text = build(&program, |_| {});
        assert!(text.contains("CallForwardingArguments"), "{text}");
        assert!(
            !text.contains("CreateArguments") && !text.contains("NewArray"),
            "{text}"
        );
    }

    #[test]
    fn other_uses_create_the_object() {
        let returns_it = assemble(|_| {
            vec![
                Instruction::Enter,
                create_arguments(false),
                Instruction::Return { value: l(0) },
            ]
        });
        let text = build(&returns_it, |_| {});
        assert!(text.contains("Generic CreateArguments"), "{text}");

        // Writes to arguments keep it from being virtual, and so do writes to
        // the parameters a mapped object aliases.
        let writes_argument = assemble(|_| {
            vec![
                Instruction::Enter,
                create_arguments(false),
                Instruction::Mov { dst: a(0), src: c(1) },
                Instruction::GetLength {
                    value_feedback: 0,
                    dst: r(5),
                    base: l(0),
                    base_identifier: None,
                    cache: 0,
                },
                Instruction::Return { value: r(5) },
            ]
        });
        assert!(build(&writes_argument, |_| {}).contains("Generic CreateArguments"));
        let reads_length = |mapped| {
            assemble(move |_| {
                vec![
                    Instruction::Enter,
                    create_arguments(mapped),
                    Instruction::GetLength {
                        value_feedback: 0,
                        dst: r(5),
                        base: l(0),
                        base_identifier: None,
                        cache: 0,
                    },
                    Instruction::Return { value: r(5) },
                ]
            })
        };
        assert!(build(&reads_length(false), |_| {}).contains("ArgumentCount"));
        let aliased = |snapshot: &mut Snapshot| snapshot.executables[0].mapped_arguments_alias_parameters = true;
        assert!(build(&reads_length(true), aliased).contains("ArgumentCount"));
    }
}
