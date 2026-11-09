/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A sweep over the rows of a tree: one loop over the rows of the body of a function in evaluation order, without
//! recursion. Children come before their parents, and the parser left mark rows where something has to happen before a
//! parent is reached: after the test of a conditional, say. Every row is one call to the emission core in `stream`.
//!
//! What does not happen in the order of its rows has a mark at its start that says where it ends. That goes to `constructs`,
//! which walks it by what it is and comes back here for the pieces of code in it.

use libjs_abi::FunctionNamePrefix;

use crate::ast::AssignmentOp;
use crate::ast::BinaryOp;
use crate::ast::DeclarationKind;
use crate::ast::Name;
use crate::ast::SyntaxTree;
use crate::ast::UnaryOp;
use crate::bytecode::instruction::typed as op;
use crate::bytecode::operand::IdentifierTableIndex;
use crate::bytecode::operand::Operand;
use crate::bytecode::source_map::SourceOffset;
use crate::parser::OutOfStack;
use crate::parser::columns::NONE;
use crate::parser::columns::function_flag;
use crate::parser::columns::tag;
use crate::parser::token::N_PROTO;

use super::context::Generator;
use super::nodes::*;
use super::stream::TargetBinding;
use super::stream::ValKind;
use crate::bytecode::constant_fold::ascii_code_units_to_string;

/// The operator of an assignment that also computes something.
pub(super) fn binary_operator_of_assignment(operator: AssignmentOp) -> Option<BinaryOp> {
    Some(match operator {
        AssignmentOp::AdditionAssignment => BinaryOp::Addition,
        AssignmentOp::SubtractionAssignment => BinaryOp::Subtraction,
        AssignmentOp::MultiplicationAssignment => BinaryOp::Multiplication,
        AssignmentOp::DivisionAssignment => BinaryOp::Division,
        AssignmentOp::ModuloAssignment => BinaryOp::Modulo,
        AssignmentOp::ExponentiationAssignment => BinaryOp::Exponentiation,
        AssignmentOp::BitwiseAndAssignment => BinaryOp::BitwiseAnd,
        AssignmentOp::BitwiseOrAssignment => BinaryOp::BitwiseOr,
        AssignmentOp::BitwiseXorAssignment => BinaryOp::BitwiseXor,
        AssignmentOp::LeftShiftAssignment => BinaryOp::LeftShift,
        AssignmentOp::RightShiftAssignment => BinaryOp::RightShift,
        AssignmentOp::UnsignedRightShiftAssignment => BinaryOp::UnsignedRightShift,
        _ => return None,
    })
}

/// The tag after `row`, parentheses aside, which is what decides what `row` is for.
#[inline(always)]
pub(super) fn next_tag(tags: &[u8], row: usize) -> u8 {
    let mut next = row + 1;
    while tags[next] == tag::PAREN {
        next += 1;
    }
    tags[next]
}

/// Generates the body of `function`.
pub(super) fn sweep_function_body(generator: &mut Generator<'_>, function: NodeId) -> Result<(), OutOfStack> {
    let tree = generator.tree;
    let function_tree = tree.node_a(function);
    let first_row = tree.extra_at(function_tree + 7) as usize;
    let is_expression_body = tree.extra_at(function_tree + 6) != 0;
    let last_row = function.0 as usize;
    if generator.is_in_generator_function() {
        // NB: The code of a generator starts when something asks for its first value.
        let start = generator.new_label();
        generator.reference_label(start);
        let undefined = generator.context.constants.add_undefined();
        generator.put(op::Yield {
            continuation_label: Some(start),
            value: undefined,
        });
        generator.bind_label(start);
    }
    generator.sweep_rows(first_row, last_row)?;
    generator.end_disposal();
    if is_expression_body {
        generator.current_source_offset = SourceOffset(tree.ast.pos[last_row]);
        generator.return_statement(true);
    }
    Ok(())
}

/// Generates the top level of a program.
pub(super) fn sweep_program(generator: &mut Generator<'_>, program: NodeId) -> Result<(), OutOfStack> {
    generator.sweep_rows(1, program.index())?;
    generator.end_disposal();
    Ok(())
}

impl Generator<'_> {
    /// Goes through the rows from `first_row` up to `end_row`, which are all of some piece of code.
    pub(super) fn sweep_rows(&mut self, first_row: usize, end_row: usize) -> Result<(), OutOfStack> {
        let tree = self.tree;
        let tags = &tree.ast.tag[..];
        let positions = &tree.ast.pos[..];
        let a_column = &tree.ast.a[..];
        let b_column = &tree.ast.b[..];
        let mut declaration_kind = DeclarationKind::Var;
        let mut declared_ident = None;
        let mut declared_resource = None;

        let mut row = first_row;
        while row < end_row {
            // SAFETY: All columns have a row for every node of the tree, and `end_row` is at most one past the last.
            let (row_tag, a, b, position) = unsafe {
                (
                    *tags.get_unchecked(row),
                    *a_column.get_unchecked(row),
                    *b_column.get_unchecked(row),
                    *positions.get_unchecked(row),
                )
            };
            let node = NodeId(row as u32);
            self.current_source_offset = SourceOffset(position);
            match row_tag {
                tag::IDENT => {
                    let ident = self.identifier_of(Name(a));
                    self.value_of_identifier(ident);
                }
                tag::IDENT_CALLEE => {
                    let ident = self.identifier_of(Name(a));
                    if self.builtin_abstract_operations_enabled
                        && ident.is_global
                        && ident.declaration_kind.is_none()
                        && self.is_builtin_operation(ident.name)
                    {
                        self.callee_of_builtin_operation(ident.name);
                    } else {
                        self.callee_of_identifier(ident, SourceOffset(b));
                    }
                }
                tag::IDENT_TARGET => {
                    let ident = self.identifier_of(Name(a));
                    self.target_of_identifier(ident, TargetBinding::MightAppear);
                }
                tag::IDENT_READ_TARGET => {
                    let ident = self.identifier_of(Name(a));
                    self.target_of_identifier(ident, TargetBinding::Known);
                    self.read_target(Some(ident));
                }
                tag::IDENT_DECLARATION => {
                    declaration_kind = declaration_kind_of_mark(b);
                    declared_resource = resource_of_mark(b);
                    if let Some(is_async) = declared_resource {
                        self.begin_disposal(is_async);
                    }
                    let ident = self.declare_binding(Name(a), declaration_kind);
                    self.target_of_declaration(ident);
                    declared_ident = Some(ident);
                }
                tag::IDENT_TYPEOF => {
                    let ident = self.identifier_of(Name(a));
                    self.value_of_typeof_identifier(ident, SourceOffset(b));
                }
                tag::IDENT_DELETE => {
                    let ident = self.identifier_of(Name(a));
                    self.value_of_delete_identifier(ident);
                }
                tag::IDENT_KEY | tag::STR_KEY | tag::NUM_KEY => {}
                tag::MEMBER => self.value_of_member(Name(b)),
                tag::MEMBER_CALLEE => self.callee_of_member(Name(b)),
                tag::MEMBER_TARGET | tag::MEMBER_DELETE => self.target_of_member(Name(b)),
                tag::MEMBER_READ_TARGET => {
                    self.target_of_member(Name(b));
                    self.read_target(None);
                }
                tag::INDEX => self.value_of_index(),
                tag::INDEX_CALLEE => self.callee_of_index(),
                tag::INDEX_TARGET | tag::INDEX_DELETE => self.target_of_index(),
                tag::INDEX_READ_TARGET => {
                    self.target_of_index();
                    self.read_target(None);
                }
                tag::NUM => self.value_of_number(tree.number_value(node)),
                tag::STR => {
                    let operand = match tree.string_literal_text(node) {
                        StringLiteralText::Source(text) => self.add_constant_string_view(text),
                        StringLiteralText::Processed(value) => self.context.constants.add_string(value),
                    };
                    self.value_of_constant(operand);
                }
                tag::TRUE => {
                    let operand = self.context.constants.add_boolean(true);
                    self.value_of_constant(operand);
                }
                tag::FALSE => {
                    let operand = self.context.constants.add_boolean(false);
                    self.value_of_constant(operand);
                }
                tag::NULL => {
                    let operand = self.context.constants.add_null();
                    self.value_of_constant(operand);
                }
                tag::HOLE => {
                    let operand = self.context.constants.add_empty();
                    self.value_of_constant(operand);
                }
                tag::REGEX => {
                    let pattern = tree.source_range_units(positions[row] + 1, a);
                    let flags = tree.source_range_units(a + 1, b);
                    let source_index = self.intern_string_text(pattern);
                    let flags_index = self.intern_string_text(flags);
                    self.value_of_regular_expression(source_index, flags_index);
                }
                tag::THIS => self.value_of_this(),
                tag::PAREN => {}
                tag::CALL | tag::OPT_CALL => self.call(tree.node_list(b).len()),
                tag::NEW => self.construct(tree.node_list(b).len()),
                tag::CALL_SPREAD => self.call_with_array(),
                tag::CALL_DIRECT_EVAL => self.direct_eval(tree.node_list(b).len()),
                tag::CALL_DIRECT_EVAL_SPREAD => self.direct_eval_with_array(),
                tag::NEW_SPREAD => self.construct_with_array(),
                tag::ARRAY_SPREAD => {}
                tag::MARK_SPREAD_START => self.spread_begin(a as usize),
                tag::SPREAD => self.append_element(true),
                tag::MARK_APPEND => self.append_element(false),
                tag::OBJECT_SPREAD => self.object_spread(),
                tag::MARK_OPTIONAL => self.optional_link(a != 0),
                tag::CHAIN => {
                    if self.top_value_kind() == ValKind::Callee {
                        self.chain_end_of_callee();
                    } else {
                        let is_delete = self.is_top_value_target();
                        if is_delete {
                            self.delete_target();
                        }
                        self.chain_end(is_delete);
                    }
                }
                tag::PRIVATE_NAME => self.value_of_constant(Operand::invalid()),
                tag::SUPER => self.value_of_super_this(),
                tag::SUPER_CALLEE => self.value_of_super_constructor(),
                tag::SUPER_CALL => {
                    if !tree
                        .node_list(b)
                        .iter()
                        .any(|argument| tree.node_tag(argument) == tag::SPREAD)
                    {
                        self.array(tree.node_list(b).len());
                    }
                    self.super_call();
                }
                tag::SUPER_MEMBER => self.value_of_super_property(Some(Name(b)), false),
                tag::SUPER_MEMBER_CALLEE => self.value_of_super_property(Some(Name(b)), true),
                tag::SUPER_MEMBER_TARGET | tag::SUPER_MEMBER_DELETE => self.target_of_super_property(Some(Name(b))),
                tag::SUPER_MEMBER_READ_TARGET => {
                    self.target_of_super_property(Some(Name(b)));
                    self.read_target(None);
                }
                tag::SUPER_INDEX => self.value_of_super_property(None, false),
                tag::SUPER_INDEX_CALLEE => self.value_of_super_property(None, true),
                tag::SUPER_INDEX_TARGET | tag::SUPER_INDEX_DELETE => self.target_of_super_property(None),
                tag::SUPER_INDEX_READ_TARGET => {
                    self.target_of_super_property(None);
                    self.read_target(None);
                }
                tag::PRIVATE_MEMBER | tag::OPT_PRIVATE_MEMBER => self.value_of_private_member(Name(b)),
                tag::PRIVATE_MEMBER_CALLEE => self.callee_of_private_member(Name(b)),
                tag::PRIVATE_MEMBER_TARGET => self.target_of_private_member(Name(b)),
                tag::PRIVATE_MEMBER_READ_TARGET => {
                    self.target_of_private_member(Name(b));
                    self.read_target(None);
                }
                tag::NEW_TARGET => self.value_of_new_target(),
                tag::IMPORT_META => self.value_of_import_meta(),
                tag::IMPORT_CALL => self.import_call(b != 0),
                tag::DEBUGGER => self.put(op::Debugger {}),
                tag::BIGINT => {
                    let literal = tree.source_range_units(positions[row], a);
                    let digits = crate::ast::bigint_literal_digits(literal);
                    self.value_of_bigint(ascii_code_units_to_string(digits).expect("the digits of a BigInt are ASCII"));
                }
                tag::TAGGED_TEMPLATE => {
                    let elements = tree.node_list(tree.node_a(NodeId(b))).len();
                    self.call(elements / 2 + 1);
                }
                tag::MARK_LOGICAL_ASSIGNMENT => {
                    self.logical_assignment_after_read(logical_operator_of_assignment(a as u8));
                }
                tag::OPT_MEMBER => self.value_of_member(Name(b)),
                tag::OPT_MEMBER_CALLEE => self.callee_of_member(Name(b)),
                tag::OPT_INDEX => self.value_of_index(),
                tag::OPT_INDEX_CALLEE => self.callee_of_index(),
                tag::UNARY => {
                    let operator = tree.unary_operator(node);
                    match operator {
                        UnaryOp::Typeof if tree.node_tag(tree.skip_parens(NodeId(a))) == tag::IDENT_TYPEOF => {}
                        UnaryOp::Delete => match tree.node_tag(tree.skip_parens(NodeId(a))) {
                            tag::IDENT_DELETE => {}
                            tag::CHAIN
                                if matches!(
                                    tree.node_tag(NodeId(tree.node_a(tree.skip_parens(NodeId(a))))),
                                    tag::MEMBER_DELETE | tag::INDEX_DELETE
                                ) => {}
                            _ if self.top_value_kind() == ValKind::Value => self.delete_value(),
                            _ => self.delete_target(),
                        },
                        _ => self.unary(operator),
                    }
                }
                tag::PREFIX_UPDATE | tag::POSTFIX_UPDATE => {
                    if self.top_value_kind() == ValKind::InvalidTarget {
                        self.result_of_invalid_target();
                        row += 1;
                        continue;
                    }
                    let value_is_used = (next_tag(tags, row) != tag::EXPR_STMT || self.tracks_completion())
                        && !self.is_unused_value_row(row);
                    self.update(tree.update_operator(node), row_tag == tag::PREFIX_UPDATE, value_is_used);
                }
                tag::CONDITIONAL => self.conditional_end(),
                tag::MARK_CONDITIONAL_TEST => self.conditional_after_test(),
                tag::MARK_CONDITIONAL_ELSE => self.conditional_after_consequent(),
                tag::MARK_LOGICAL => self.logical_after_left(logical_operator_of_mark(a)),
                tag::SEQUENCE => self.sequence(tree.node_list(a).len()),
                tag::ARRAY => self.array(tree.node_list(a).len()),
                tag::MARK_OBJECT => self.object_begin(a != 0),
                tag::OBJECT => self.object_end(),
                tag::PROPERTY | tag::SHORTHAND => {
                    let key = if row_tag == tag::SHORTHAND {
                        self.intern_property_key(Name(a))
                    } else {
                        let key = NodeId(a);
                        if is_proto_key(tree, key) {
                            self.object_prototype_property();
                            row += 1;
                            continue;
                        }
                        match tree.node_tag(key) {
                            tag::IDENT_KEY => self.intern_property_key(Name(tree.node_a(key))),
                            tag::STR_KEY => {
                                let text = tree.string_literal_text(key);
                                self.intern_property_key_text(text.view())
                            }
                            tag::NUM_KEY => {
                                self.object_number_property(tree.number_value(key), libjs_abi::PutKind::Own);
                                row += 1;
                                continue;
                            }
                            _ => {
                                self.sweep_computed_property(b);
                                row += 1;
                                continue;
                            }
                        }
                    };
                    self.object_property(key);
                }
                tag::COMPUTED_PROPERTY => self.sweep_computed_property(b),
                tag::METHOD | tag::GETTER | tag::SETTER => {
                    let key = NodeId(a);
                    let kind = put_kind_of_method(row_tag);
                    let property_key = match tree.node_tag(key) {
                        tag::IDENT_KEY => self.intern_property_key(Name(tree.node_a(key))),
                        tag::STR_KEY => {
                            let text = tree.string_literal_text(key);
                            self.intern_property_key_text(text.view())
                        }
                        tag::NUM_KEY => {
                            self.object_number_property(tree.number_value(key), kind);
                            row += 1;
                            continue;
                        }
                        _ => {
                            self.object_computed_method(kind, function_name_prefix_of_method(row_tag));
                            row += 1;
                            continue;
                        }
                    };
                    self.object_put_by_id(property_key, kind);
                }
                tag::COMPUTED_METHOD | tag::COMPUTED_GETTER | tag::COMPUTED_SETTER => {
                    let (kind, prefix) = match row_tag {
                        tag::COMPUTED_GETTER => (libjs_abi::PutKind::Getter, FunctionNamePrefix::Get),
                        tag::COMPUTED_SETTER => (libjs_abi::PutKind::Setter, FunctionNamePrefix::Set),
                        _ => (libjs_abi::PutKind::Own, FunctionNamePrefix::None),
                    };
                    self.object_computed_method(kind, prefix);
                }
                tag::MARK_COMPUTED_KEY => self.object_computed_key(),
                tag::FUNCTION => {
                    if tags[row + 1] == tag::FUNCTION_DECL {
                        if self.context.scope.is_in_block() {
                            self.function_declaration_in_block(Name(b_column[row + 1]));
                        } else {
                            self.function_declaration(Name(b_column[row + 1]), b);
                        }
                    } else if (tag::METHOD..=tag::COMPUTED_SETTER).contains(&tags[row + 1]) {
                        self.sweep_object_method(b, row);
                    } else {
                        self.sweep_function_expression(node, row);
                    }
                }
                tag::FUNCTION_DECL => {}
                tag::EXPR_STMT => self.end_of_expression_statement(),
                tag::DECLARATOR => {
                    let ident = declared_ident.take().expect("a declarator comes after its name");
                    self.declare(ident, b != 0, declaration_kind, declared_resource);
                }
                tag::VAR | tag::LET | tag::CONST | tag::USING | tag::AWAIT_USING | tag::EMPTY => {}
                tag::MARK_IF_TEST => {
                    self.reset_completion();
                    self.if_after_test();
                }
                tag::MARK_ELSE => self.if_else(),
                tag::IF => self.if_end(),
                tag::MARK_LOOP_HEAD => {
                    self.reset_completion();
                    self.while_head();
                }
                tag::MARK_WHILE_TEST => self.while_after_test(),
                tag::WHILE => self.while_end(),
                tag::MARK_DO_HEAD => {
                    self.reset_completion();
                    self.do_head();
                }
                tag::MARK_DO_TEST => self.do_before_test(),
                tag::DO_WHILE => self.do_end(),
                tag::MARK_BLOCK => {
                    self.block_begin(a);
                    if b != 0 {
                        self.instantiate_functions_of_block(NodeId(b));
                    }
                }
                tag::BLOCK => {
                    self.context.scope.note_block_range(position, tree.end_of_block(node.0));
                    self.block_end();
                }
                tag::IF_FUNCTION_BLOCK => self.block_end(),
                tag::RETURN => self.return_statement(a != 0),
                tag::THROW => self.throw_statement(),
                tag::AWAIT => self.await_value(),
                tag::YIELD => self.yield_value(a != 0, false),
                tag::YIELD_STAR => self.yield_value(true, true),
                tag::BREAK => self.break_statement((a != NONE).then_some(Name(a))),
                tag::CONTINUE => self.continue_statement((a != NONE).then_some(Name(a))),
                // NB: Anything but a pattern that a mark for a target still stands alone for is nothing that can be written.
                tag::MARK_TARGET | tag::MARK_READ_TARGET
                    if !matches!(tags[row - 1], tag::ARRAY_PATTERN | tag::OBJECT_PATTERN) =>
                {
                    self.invalid_target();
                }
                tag::MARK_KEY if tags[row - 1] == tag::BIGINT => self.object_computed_key(),
                tag::MARK_TARGET
                | tag::MARK_READ_TARGET
                | tag::MARK_CALLEE
                | tag::MARK_DECLARATION
                | tag::MARK_TYPEOF
                | tag::MARK_DELETE
                | tag::MARK_KEY => {}
                tag::MARK_SUBTREE => {
                    // NB: An assignment that turned into an element of a pattern goes by the rows of the pattern.
                    if a != 0 && tags[a as usize] != tag::ASSIGN_PATTERN {
                        self.sweep_subtree(NodeId(a), row, position, b)?;
                        row = a as usize + 1;
                        continue;
                    }
                }
                other if (tag::BINARY_BASE..tag::MARK_CALLEE).contains(&other) => {
                    match crate::ast::SyntaxTree::binary_operator(other) {
                        BinaryOrLogical::Binary(BinaryOp::In) if tree.node_tag(NodeId(a)) == tag::PRIVATE_NAME => {
                            self.private_name_in(Name(tree.node_a(NodeId(a))));
                        }
                        BinaryOrLogical::Binary(operator) => self.binary(operator),
                        BinaryOrLogical::Logical(_) => self.logical_end(),
                    }
                }
                other if (tag::ASSIGN_BASE..tag::BINARY_BASE).contains(&other) => self.sweep_assignment(other),
                other => unreachable!("no sweep comes across a row with the tag {other}"),
            }
            row += 1;
        }
        Ok(())
    }

    fn sweep_assignment(&mut self, row_tag: u8) {
        let operator = crate::ast::SyntaxTree::assignment_operator(row_tag);
        if matches!(
            operator,
            AssignmentOp::AndAssignment | AssignmentOp::OrAssignment | AssignmentOp::NullishAssignment
        ) {
            self.logical_assignment_end();
            return;
        }
        if self.is_invalid_target_below_top() {
            self.drop_value();
            self.result_of_invalid_target();
            return;
        }
        if operator == AssignmentOp::Assignment {
            self.assign_to_binding();
            return;
        }
        let operator = binary_operator_of_assignment(operator).expect("the logical ones are handled above");
        self.compound_assign(operator);
    }

    /// The function of a method, a getter or a setter of an object, whose row is `row`.
    fn sweep_object_method(&mut self, function_row: u32, row: usize) {
        let tree = self.tree;
        let method_tag = tree.ast.tag[row + 1];
        let key = NodeId(tree.ast.a[row + 1]);
        if matches!(
            method_tag,
            tag::COMPUTED_METHOD | tag::COMPUTED_GETTER | tag::COMPUTED_SETTER
        ) {
            let object = self.object_below(1);
            self.value_of_function_with_text_name(function_row, None, Some(object));
            return;
        }
        if tree.node_tag(key) == tag::BIGINT {
            let object = self.object_below(1);
            self.value_of_function_with_text_name(function_row, None, Some(object));
            return;
        }
        let text = tree.pattern_key_text(key);
        let name = match method_tag {
            tag::GETTER | tag::SETTER => {
                let mut prefixed = crate::utf16::Utf16StringBuilder::new();
                prefixed.append_ascii(if method_tag == tag::GETTER { "get " } else { "set " });
                prefixed.append(text.view());
                let prefixed = prefixed.to_utf16_string();
                self.intern_identifier_text(crate::utf16::Utf16View::of_string(&prefixed))
            }
            _ => self.intern_identifier_text(text.view()),
        };
        let object = self.object_below(0);
        self.value_of_function_with_text_name(function_row, Some(name), Some(object));
    }

    /// `key: value` with a key that only has a value once it gets evaluated, and with both on top of the stack.
    fn sweep_computed_property(&mut self, value: u32) {
        let tree = self.tree;
        let value = tree.skip_parens(NodeId(value));
        let is_anonymous_definition = match tree.node_tag(value) {
            tag::FUNCTION => is_anonymous(tree, value),
            // NB: A class has its name before its static elements run, which `NewClass` sees to.
            _ => false,
        };
        self.object_computed_property(is_anonymous_definition);
    }

    fn sweep_function_expression(&mut self, node: NodeId, row: usize) {
        let tree = self.tree;
        if self.eager_compile_direct_iifes && is_called_right_away(tree, row) {
            self.note_next_function_is_eager();
        }
        let function_row = tree.node_b(node);
        let flags = tree.script.index.flags[function_row as usize];
        let has_name = tree.script.index.name[function_row as usize] != NONE && flags & function_flag::METHOD == 0;
        if has_name && flags & function_flag::NAME_UNUSED == 0 {
            self.value_of_named_function_at_row(function_row, Name(tree.script.index.name[function_row as usize]));
            return;
        }
        let name = if has_name {
            None
        } else {
            self.name_of_anonymous_definition(row, row + 1)
        };
        self.value_of_function_with_text_name(function_row, name, None);
    }

    /// The name that an anonymous function or class gets from what it is the value of, if it is all there is of that
    /// value. Its rows go from `row` up to `next_row`.
    pub(super) fn name_of_anonymous_definition(&mut self, row: usize, next_row: usize) -> Option<IdentifierTableIndex> {
        let tree = self.tree;
        let tags = &tree.ast.tag[..];
        if let Some(name) = self.context.stream.pending_function_name.take() {
            return Some(name);
        }
        if row < 2 {
            return None;
        }
        // NB: `async` in front of an arrow function was a reference until the arrow came up.
        let mark_row = if tags[row - 1] == tag::EMPTY { row - 2 } else { row - 1 };
        let next = next_tag(tags, next_row - 1);
        let mark_node = NodeId(mark_row as u32);
        match tags[mark_row] {
            tag::MARK_LOGICAL_ASSIGNMENT
                if tags[mark_row - 1] == tag::IDENT_READ_TARGET
                    && (tag::ASSIGN_BASE..tag::BINARY_BASE).contains(&next) =>
            {
                Some(self.intern_identifier(Name(tree.node_a(NodeId(mark_row as u32 - 1)))))
            }
            tag::IDENT_KEY if next == tag::PROPERTY && is_proto_key(tree, mark_node) => None,
            tag::IDENT_TARGET | tag::IDENT_DECLARATION | tag::IDENT_KEY
                if next == tag::DECLARATOR || next == tag::ASSIGN_BASE || next == tag::PROPERTY =>
            {
                Some(self.intern_identifier(Name(tree.node_a(mark_node))))
            }
            tag::STR_KEY if next == tag::PROPERTY && is_proto_key(tree, mark_node) => None,
            tag::STR_KEY | tag::NUM_KEY if next == tag::PROPERTY => {
                let text = tree.pattern_key_text(mark_node);
                Some(self.intern_identifier_text(text.view()))
            }
            _ => None,
        }
    }
}

/// Whether the function expression at `row` is what the code around it calls, directly or through `call` or `apply`.
fn is_called_right_away(tree: &SyntaxTree, row: usize) -> bool {
    let mut next = row + 1;
    while tree.ast.tag[next] == tag::PAREN {
        next += 1;
    }
    match tree.ast.tag[next] {
        tag::MARK_CALLEE => true,
        tag::MEMBER_CALLEE => {
            let name = tree.name_text(Name(tree.ast.b[next]));
            name == utf16!("call") || name == utf16!("apply")
        }
        _ => false,
    }
}

/// Whether `key` of a property is what makes a property set the prototype.
fn is_proto_key(tree: &SyntaxTree, key: NodeId) -> bool {
    match tree.node_tag(key) {
        tag::IDENT_KEY => tree.node_a(key) == N_PROTO,
        tag::STR_KEY => tree.string_literal_text(key).view() == crate::utf16::Utf16View::Ascii(b"__proto__"),
        _ => false,
    }
}

/// Whether the function at `function` has no name of its own.
pub(super) fn is_anonymous(tree: &SyntaxTree, function: NodeId) -> bool {
    tree.script.index.name[tree.node_b(function) as usize] == NONE
}

/// https://tc39.es/ecma262/#sec-static-semantics-isfunctiondefinition
#[inline(always)]
fn is_function_definition(tree: &SyntaxTree, expression: NodeId) -> bool {
    matches!(tree.node_tag(expression), tag::FUNCTION | tag::CLASS)
}

/// https://tc39.es/ecma262/#sec-static-semantics-hasname
#[inline(always)]
fn has_name(tree: &SyntaxTree, expression: NodeId) -> bool {
    match tree.node_tag(expression) {
        tag::FUNCTION => !is_anonymous(tree, expression),
        tag::CLASS => tree.node_a(expression) != NONE,
        _ => false,
    }
}

/// https://tc39.es/ecma262/#sec-isanonymousfunctiondefinition
pub(super) fn is_anonymous_function_definition(tree: &SyntaxTree, expression: NodeId) -> bool {
    // NB: IsFunctionDefinition and HasName of a ParenthesizedExpression are those of the expression in it.
    let expression = tree.skip_parens(expression);

    // 1. If IsFunctionDefinition of expr is false, return false.
    if !is_function_definition(tree, expression) {
        return false;
    }

    // 2. Let hasName be HasName of expr.
    // 3. If hasName is true, return false.
    if has_name(tree, expression) {
        return false;
    }

    // 4. Return true.
    true
}

/// The operator of a logical assignment, by the token that it is.
fn logical_operator_of_assignment(token: u8) -> crate::ast::LogicalOp {
    use crate::parser::token::T;
    if token == T::LogAndAssign as u8 {
        crate::ast::LogicalOp::And
    } else if token == T::LogOrAssign as u8 {
        crate::ast::LogicalOp::Or
    } else {
        crate::ast::LogicalOp::NullishCoalescing
    }
}

fn function_name_prefix_of_method(method_tag: u8) -> FunctionNamePrefix {
    match method_tag {
        tag::GETTER => FunctionNamePrefix::Get,
        tag::SETTER => FunctionNamePrefix::Set,
        _ => FunctionNamePrefix::None,
    }
}

fn put_kind_of_method(method_tag: u8) -> libjs_abi::PutKind {
    match method_tag {
        tag::GETTER => libjs_abi::PutKind::Getter,
        tag::SETTER => libjs_abi::PutKind::Setter,
        _ => libjs_abi::PutKind::Own,
    }
}

pub(super) fn declaration_kind_of_mark(kind: u32) -> DeclarationKind {
    match kind as u8 {
        tag::LET => DeclarationKind::Let,
        tag::CONST | tag::USING | tag::AWAIT_USING => DeclarationKind::Const,
        _ => DeclarationKind::Var,
    }
}

/// Whether what a declaration of the kind of a mark declares is a resource, and if so whether it goes asynchronously.
pub(super) fn resource_of_mark(kind: u32) -> Option<bool> {
    match kind as u8 {
        tag::USING => Some(false),
        tag::AWAIT_USING => Some(true),
        _ => None,
    }
}

fn logical_operator_of_mark(operator_tag: u32) -> crate::ast::LogicalOp {
    match crate::ast::SyntaxTree::binary_operator(operator_tag as u8) {
        BinaryOrLogical::Logical(operator) => operator,
        BinaryOrLogical::Binary(_) => unreachable!("the parser marks logical operators"),
    }
}
