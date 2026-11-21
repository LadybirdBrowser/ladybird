/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The tests of `if` statements and loops branch at each `&&`, `||` and `!`, rather than making a boolean out of them
//! first. Before the rows of such a test, a walk down its operators queues what each of those rows does instead.

use crate::ast::{LogicalOp, SyntaxTree, UnaryOp};
use crate::bytecode::operand::Label;
use crate::parser::OutOfStack;
use crate::parser::columns::tag;

use super::context::Generator;
use super::nodes::{BinaryOrLogical, NodeId};
use super::stream::Control;

#[derive(Clone, Copy)]
pub(super) struct Branch {
    true_target: Label,
    false_target: Label,
    /// The label that the code right after the branch gets.
    fallthrough: Label,
    back_target: Option<Label>,
}

#[derive(Clone, Copy)]
enum Step {
    /// Branches on the operand that the row before this one left.
    Branch(Branch),
    Skip,
    Bind(Label),
    /// The test of an `if` is done, and its consequent comes next.
    BeginIf {
        consequent: Label,
        alternate: Label,
    },
}

/// A step that takes the place of the row `row`.
#[derive(Clone, Copy)]
pub(super) struct ConditionStep {
    row: u32,
    step: Step,
}

enum Shape {
    Operand,
    Group(NodeId),
    Not(NodeId),
    Logical(LogicalOp, NodeId, NodeId),
}

fn shape(tree: &SyntaxTree, node: NodeId) -> Shape {
    match tree.node_tag(node) {
        tag::PAREN => Shape::Group(NodeId(tree.node_a(node))),
        tag::UNARY if tree.unary_operator(node) == UnaryOp::Not => Shape::Not(NodeId(tree.node_a(node))),
        node_tag if (tag::BINARY_BASE..tag::MARK_CALLEE).contains(&node_tag) => {
            match SyntaxTree::binary_operator(node_tag) {
                BinaryOrLogical::Logical(operator @ (LogicalOp::And | LogicalOp::Or)) => {
                    Shape::Logical(operator, NodeId(tree.node_a(node)), NodeId(tree.node_b(node)))
                }
                _ => Shape::Operand,
            }
        }
        _ => Shape::Operand,
    }
}

fn branches_by_operators(tree: &SyntaxTree, test: NodeId) -> bool {
    matches!(shape(tree, tree.skip_parens(test)), Shape::Not(_) | Shape::Logical(..))
}

impl Generator<'_> {
    /// Queues the steps of the rows of `test`, which go to `true_target` if it holds and to `false_target` otherwise.
    fn prepare_condition(&mut self, test: NodeId, branch: Branch) {
        let tree = self.tree;
        let mut steps = core::mem::take(&mut self.context.stream.condition_steps);
        let mut work = core::mem::take(&mut self.context.stream.condition_work);
        debug_assert!(steps.is_empty());
        work.push((test, branch));
        while let Some((node, branch)) = work.pop() {
            let mut step = |row: usize, step: Step| {
                steps.push(ConditionStep {
                    row: crate::u32_from_usize(row),
                    step,
                });
            };
            match shape(tree, node) {
                Shape::Operand => step(node.index() + 1, Step::Branch(branch)),
                Shape::Group(inner) => {
                    step(node.index(), Step::Skip);
                    work.push((inner, branch));
                }
                Shape::Not(operand) => {
                    step(node.index(), Step::Skip);
                    work.push((
                        operand,
                        Branch {
                            true_target: branch.false_target,
                            false_target: branch.true_target,
                            ..branch
                        },
                    ));
                }
                Shape::Logical(operator, left, right) => {
                    let next = self.new_label();
                    // NB: The row after the left operand is its MARK_LOGICAL.
                    step(left.index() + 1, Step::Bind(next));
                    step(node.index(), Step::Skip);
                    let left_branch = if matches!(operator, LogicalOp::And) {
                        Branch {
                            true_target: next,
                            fallthrough: next,
                            ..branch
                        }
                    } else {
                        Branch {
                            false_target: next,
                            fallthrough: next,
                            ..branch
                        }
                    };
                    work.push((right, branch));
                    work.push((left, left_branch));
                }
            }
        }
        // NB: Steps are taken from the end. A branch on an operand comes before what its row does.
        steps.sort_unstable_by_key(|step| (core::cmp::Reverse(step.row), matches!(step.step, Step::Branch(_))));
        self.context.stream.condition_steps = steps;
        self.context.stream.condition_work = work;
    }

    /// Whether the row `row` belongs to a test whose steps are queued.
    #[inline(always)]
    pub(super) fn has_condition_step(&self, row: usize) -> bool {
        self.context
            .stream
            .condition_steps
            .last()
            .is_some_and(|step| step.row as usize == row)
    }

    /// Takes the place of the row `row` of a test.
    pub(super) fn take_condition_steps(&mut self, row: usize) {
        while let Some(&ConditionStep { step, .. }) = self
            .context
            .stream
            .condition_steps
            .last()
            .filter(|step| step.row as usize == row)
        {
            self.context.stream.condition_steps.pop();
            match step {
                Step::Branch(branch) => self.branch_on_condition(branch),
                Step::Skip => {}
                Step::Bind(label) => self.bind_label(label),
                Step::BeginIf { consequent, alternate } => {
                    self.end_of_statement();
                    self.bind_label(consequent);
                    self.push_control(Control::If {
                        else_label: alternate,
                        end_label: None,
                    });
                }
            }
        }
    }

    fn branch_on_condition(&mut self, branch: Branch) {
        let (condition, first_temporary) = self.pop_operand();
        self.release_temporaries_from(first_temporary);
        self.branch(
            condition,
            branch.true_target,
            branch.false_target,
            branch.fallthrough,
            branch.back_target,
        );
    }

    /// Before the rows of the test `test` of an `if`.
    pub(super) fn begin_if_test(&mut self, test: NodeId) {
        self.reset_completion();
        if !branches_by_operators(self.tree, test) {
            return;
        }
        let consequent = self.new_label();
        let alternate = self.new_label();
        self.prepare_condition(
            test,
            Branch {
                true_target: consequent,
                false_target: alternate,
                fallthrough: consequent,
                back_target: None,
            },
        );
        // NB: The MARK_IF_TEST row follows the test, after every other step.
        self.context.stream.condition_steps.insert(
            0,
            ConditionStep {
                row: crate::u32_from_usize(test.index() + 1),
                step: Step::BeginIf { consequent, alternate },
            },
        );
    }

    /// The test of a `while` at its top, from the row `first_row`: on into the body if it holds, and to `end`
    /// otherwise.
    pub(super) fn sweep_while_test(&mut self, first_row: usize, test: NodeId, end: Label) -> Result<(), OutOfStack> {
        if branches_by_operators(self.tree, test) {
            let body = self.new_label();
            self.prepare_condition(
                test,
                Branch {
                    true_target: body,
                    false_target: end,
                    fallthrough: body,
                    back_target: None,
                },
            );
            self.sweep_rows(first_row, test.index() + 1)?;
            self.bind_label(body);
        } else {
            let value = self.sweep_value(first_row, test)?;
            self.jump_unless(value, end);
        }
        self.end_of_statement();
        Ok(())
    }

    /// The test of a loop at its bottom, from the row `first_row`: back to `body` if it holds, and otherwise on to `end`,
    /// which comes right after it.
    pub(super) fn sweep_loop_test(
        &mut self,
        first_row: usize,
        test: NodeId,
        body: Label,
        end: Label,
    ) -> Result<(), OutOfStack> {
        if branches_by_operators(self.tree, test) {
            self.prepare_condition(
                test,
                Branch {
                    true_target: body,
                    false_target: end,
                    fallthrough: end,
                    back_target: Some(body),
                },
            );
            self.sweep_rows(first_row, test.index() + 1)?;
        } else {
            let value = self.sweep_value(first_row, test)?;
            self.jump_back_if(value, body);
        }
        self.end_of_statement();
        Ok(())
    }
}
