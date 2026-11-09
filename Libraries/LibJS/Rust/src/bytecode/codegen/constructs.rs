/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Constructs whose code does not come in the order of their rows: statements that have to look at their parts before
//! running them, and patterns, which a value goes through after everything in the rows that follow them. These walk their
//! rows by what they are, and let `sweep` go through the pieces of code in them. All of it goes to the same emission core.

use libjs_abi::ClassElementKind;

use crate::ast::BinaryOp;
use crate::ast::ClassMethodKind;
use crate::ast::DeclarationKind;
use crate::ast::LocalType;
use crate::ast::Name;
use crate::ast::SyntaxTree;
use crate::ast::UnaryOp;
use crate::bytecode::constant_fold::ascii_code_units_to_string;
use crate::bytecode::constant_fold::constant_to_string;
use crate::bytecode::constant_fold::parse_bigint;
use crate::bytecode::executable_data::PendingClassBlueprint;
use crate::bytecode::executable_data::PendingClassElement;
use crate::bytecode::executable_data::PendingLiteralValueKind;
use crate::bytecode::instruction::typed as op;
use crate::bytecode::operand::*;
use crate::bytecode::source_map::SourceOffset;
use crate::parser::OutOfStack;
use crate::parser::columns::tag;
use crate::parser::literals::EscapeMode;
use crate::u32_from_usize;
use crate::utf16::Utf16StringBuilder;

use super::bindings::*;
use super::context::Generator;
use super::generators::generate_await_of_value;
use super::members::*;
use super::nodes::*;
use super::operators::emit_to_primitive_property_key;
use super::registers::ScopedOperand;
use super::scope::ParameterShape;
use super::scope::ScopeBindings;
use super::stream::ClassPrivateEnvironment;
use super::stream::Control;
use super::stream::Handler;
use super::stream::LoopKind;
use super::stream::OpenIterator;
use super::stream::ScopeEnvironment;
use super::stream::TargetBinding;
use super::stream::ValKind;
use super::sweep::declaration_kind_of_mark;
use super::sweep::is_anonymous_function_definition;
use super::sweep::next_tag;
use super::sweep::resource_of_mark;
use crate::parser::literals::raw_template_value;

/// Where the parameters of the function that a tree is of start.
const FIRST_ROW_OF_REPARSED_FUNCTION: usize = 1;

/// The name of a field whose key is not computed, which a computed key gets only when the class is defined.
fn class_field_name(tree: &SyntaxTree, field: NodeId) -> Option<ak::Utf16String> {
    if tree.node_tag(field) == tag::COMPUTED_CLASS_FIELD {
        return None;
    }
    let key = NodeId(tree.node_a(field));
    match tree.node_tag(key) {
        tag::PRIVATE_NAME => {
            let mut text = Utf16StringBuilder::new();
            text.append_ascii("#");
            text.append(tree.name_text(Name(tree.node_a(key))));
            Some(text.to_utf16_string())
        }
        tag::IDENT | tag::IDENT_KEY | tag::STR_KEY | tag::NUM_KEY => Some(tree.pattern_key_text(key).view().into()),
        _ => {
            let literal = tree.source_range_units(tree.node_pos(key), tree.node_a(key));
            let digits = ascii_code_units_to_string(crate::ast::bigint_literal_digits(literal))?;
            let value = parse_bigint(&digits).expect("the lexer only produces valid BigInt literals");
            Some(ak::Utf16String::from_utf8(&value.to_string()))
        }
    }
}

/// What the targets of a pattern are.
#[derive(Clone, Copy, PartialEq, Eq)]
enum PatternMode {
    /// Names that a declaration of this kind declares.
    Declares(DeclarationKind),
    /// References, that get evaluated right before the value they get is.
    Assigns,
}

/// What the head of a `for-in` or `for-of` loop gives each value to.
#[derive(Clone, Copy)]
enum LoopTarget {
    Declared {
        target: NodeId,
        first_row: usize,
        kind: DeclarationKind,
        /// Whether the value is a resource that gets disposed of, and if so whether that is asynchronous.
        resource: Option<bool>,
    },
    Expression {
        target: NodeId,
        first_row: usize,
    },
    Pattern {
        target: NodeId,
        first_row: usize,
    },
}

impl Generator<'_> {
    /// The rows from `first_row` up to and including `root`, which are of something that a mark at `position` left here.
    pub(super) fn sweep_subtree(
        &mut self,
        root: NodeId,
        first_row: usize,
        position: u32,
        detail: u32,
    ) -> Result<(), OutOfStack> {
        if self.should_stop_generating() {
            return Err(OutOfStack);
        }
        self.current_source_offset = SourceOffset(position);
        let tree = self.tree;
        let mark_row = first_row;
        let first_row = first_row + 1;
        match tree.node_tag(root) {
            tag::CLASS => {
                self.sweep_class(root, first_row, mark_row, root.index() + 1)?;
                Ok(())
            }
            tag::CLASS_DECL => self.sweep_class_declaration(root, first_row),
            tag::ASSIGN_BASE => self.sweep_assignment_pattern(root, first_row),
            tag::TRY => self.sweep_try(root, first_row, detail),
            tag::SWITCH => self.sweep_switch(root, first_row, detail),
            tag::LABELED => self.sweep_labeled(root, first_row),
            tag::WITH => self.sweep_with(root, first_row),
            tag::TEMPLATE if self.tree.ast.tag[root.index() + 1] == tag::TAGGED_TEMPLATE => {
                self.sweep_template_of_tag(root, first_row)
            }
            tag::TEMPLATE => self.sweep_template(root, first_row),
            tag::DECLARATOR => self.sweep_pattern_declarator(root, first_row, declaration_kind_of_mark(detail)),
            // NB: These are for loading a module, and have nothing to run in them.
            tag::IMPORT | tag::EXPORT_ALL => Ok(()),
            tag::EXPORT_NAMED if tree.node_a(root) != 0 => Ok(()),
            tag::EXPORT_NAMED => self.sweep_rows(first_row, root.index()),
            tag::EXPORT_DEFAULT => self.sweep_export_default(root, first_row),
            tag::SCOPED => {
                let statement = NodeId(tree.node_a(root));
                match tree.node_tag(statement) {
                    tag::FOR => self.sweep_for(statement, first_row, position),
                    tag::FOR_IN => self.sweep_for_in(statement, first_row, position),
                    tag::FOR_OF => self.sweep_for_of(statement, first_row, position, false),
                    tag::FOR_AWAIT_OF => self.sweep_for_of(statement, first_row, position, true),
                    other => unreachable!("a scope of the tag {other} has no mark"),
                }
            }
            other => unreachable!("a mark has no use for a row with the tag {other}"),
        }
    }

    /// `export default`, of a declaration or of a value.
    fn sweep_export_default(&mut self, statement: NodeId, first_row: usize) -> Result<(), OutOfStack> {
        let tree = self.tree;
        let exported = NodeId(tree.node_a(statement));
        let exported_tag = tree.node_tag(exported);
        // NB: The function declaration is hoisted.
        if exported_tag == tag::FUNCTION && tree.node_b(statement) != 0 {
            return Ok(());
        }
        if exported_tag == tag::CLASS_DECL {
            return self.sweep_rows(first_row, statement.index());
        }
        let name = self.intern_identifier_text(crate::utf16::Utf16View::Ascii(b"default"));
        self.set_pending_function_name(exported, Some(name));
        let value = self.sweep_value(first_row, exported)?;
        self.context.stream.pending_function_name = None;
        let local_name = self.intern_identifier_text(crate::utf16::Utf16View::Ascii(b"*default*"));
        emit_initialize_lexical_binding(self, local_name, value);
        self.end_of_statement();
        Ok(())
    }

    /// Goes through the rows of an expression, which leaves its value on the stack, and takes the value.
    fn sweep_value(&mut self, first_row: usize, expression: NodeId) -> Result<Operand, OutOfStack> {
        self.sweep_rows(first_row, expression.index() + 1)?;
        Ok(self.pop_operand().0)
    }

    fn sweep_class_declaration(&mut self, statement: NodeId, first_row: usize) -> Result<(), OutOfStack> {
        let tree = self.tree;
        let class = NodeId(tree.node_a(statement));
        let name = Name(tree.node_a(class));
        let ident = self.declare_binding(name, DeclarationKind::Let);
        self.sweep_class(class, first_row, 0, 0)?;
        self.current_source_offset = SourceOffset(tree.node_pos(statement));
        let (value, _) = self.pop_operand();
        // NB: Whatever comes after this may still see the binding uninitialized, in a function that got called early.
        self.target_of_declaration(ident);
        let target = self.pop_value();
        self.store_to_target(target, value, true);
        self.end_of_statement();
        Ok(())
    }

    /// A class, which leaves its value on the stack. Its rows are its heritage and then its elements. If it gets a name
    /// from around it, the rows in front of it and behind it say so.
    fn sweep_class(
        &mut self,
        class: NodeId,
        first_row: usize,
        mark_row: usize,
        next_row: usize,
    ) -> Result<(), OutOfStack> {
        let tree = self.tree;
        let data = tree.class_data(class);
        let pending_computed_name = self.context.stream.pending_computed_class_name.take();
        let is_value_of_computed_key = next_row != 0
            && mark_row != 0
            && match tree.ast.tag[mark_row - 1] {
                tag::MARK_COMPUTED_KEY => next_tag(&tree.ast.tag, next_row - 1) == tag::COMPUTED_PROPERTY,
                tag::MARK_KEY => next_tag(&tree.ast.tag, next_row - 1) == tag::PROPERTY,
                _ => false,
            };
        let computed_name = if data.name.is_some() {
            None
        } else if is_value_of_computed_key {
            Some(self.object_below(0))
        } else {
            pending_computed_name
        };
        let lhs_name = if data.name.is_none() && next_row != 0 && computed_name.is_none() {
            self.name_of_anonymous_definition(mark_row, next_row)
        } else {
            None
        };
        let saved_strict = std::mem::replace(&mut self.strict, true);
        let first_temporary = self.next_temporary();
        let dst = self.temporary();
        let held = self.hold_registers();
        let environment = self.begin_class_scope(data.name);
        if data.name.is_none() && lhs_name.is_none() && computed_name.is_none() {
            // NB: An anonymous class binds an empty name, which an environment shape cannot hold.
            let identifier = self.intern_identifier_text(crate::utf16::Utf16View::Ascii(b""));
            self.emit(op::CreateVariable {
                identifier,
                mode: libjs_abi::EnvironmentMode::Lexical as u32,
                is_immutable: true,
                is_global: false,
                is_strict: true,
            });
        }

        let raw_super_class = tree.extra_at(tree.node_b(class));
        let mut row = first_row;
        let super_class = if raw_super_class != 0 {
            let value = self.sweep_value(row, NodeId(raw_super_class))?;
            row = raw_super_class as usize + 1;
            let register = self.held_register();
            self.move_value_to(register, value);
            Some(register)
        } else {
            None
        };
        self.current_source_offset = SourceOffset(tree.node_pos(class));
        // NB: The heritage is checked before any class element is evaluated.
        let proto_parent = super_class.map(|super_class| {
            let proto_parent = self.held_register();
            self.put(op::GetClassProtoParent {
                dst: proto_parent,
                super_class,
            });
            proto_parent
        });

        let private_name_of = |key: ClassElementKey| match key {
            ClassElementKey::Expression(key) if tree.node_tag(key) == tag::PRIVATE_NAME => Some(Name(tree.node_a(key))),
            _ => None,
        };
        let mut has_private_environment = false;
        for element in data.elements_without_constructor() {
            let key = match tree.class_element(element) {
                ClassElementData::Method { key, .. } | ClassElementData::Field { key, .. } => key,
                ClassElementData::StaticInitializer { .. } => continue,
            };
            if let Some(name) = private_name_of(key) {
                if !has_private_environment {
                    self.enter_class_private_environment();
                    has_private_environment = true;
                }
                let identifier = intern_private_name(self, name);
                self.put(op::AddPrivateName { name: identifier });
            }
        }

        let mut element_keys = Vec::new();
        let mut class_elements = Vec::with_capacity(data.elements.len());
        for (index, element) in data.elements.iter().enumerate() {
            let first_element_row = row;
            row = element.index() + 1;
            if Some(index) == data.constructor_index {
                continue;
            }
            let element_data = tree.class_element(element);
            let key = match element_data {
                ClassElementData::Method { key, .. } | ClassElementData::Field { key, .. } => Some(key),
                ClassElementData::StaticInitializer { .. } => None,
            };
            element_keys.push(match key {
                Some(key) if private_name_of(key).is_none() => {
                    let operand = self.sweep_class_element_key(key, first_element_row)?;
                    if let ClassElementData::Method { is_static: true, .. } = element_data
                        && let ClassElementKey::Expression(expression) = key
                        && !matches!(tree.node_tag(expression), tag::STR_KEY | tag::NUM_KEY)
                    {
                        self.throw_if_key_of_static_method_is_prototype(operand);
                    }
                    Some(operand)
                }
                _ => None,
            });
            class_elements.push(self.pending_class_element(element_data, key));
        }

        let constructor_row = match data.constructor {
            Some(constructor) => tree.node_b(constructor),
            None => tree.extra_at(tree.node_b(class) + 5),
        };
        let constructor_sfd_index = self.register_function_of_class(constructor_row);
        let source_start = data.source_text_start as usize;
        let blueprint_index = u32_from_usize(self.class_blueprints.len());
        self.class_blueprints.push(PendingClassBlueprint {
            name: data.name.map(|name| ak::Utf16String::from(tree.name_text(name))),
            source_text_offset: source_start,
            source_text_length: data.source_text_end as usize - source_start,
            constructor_sfd_index,
            has_super_class: data.super_class.is_some(),
            has_name: data.name.is_some(),
            elements: class_elements,
        });

        let private_environment = has_private_environment.then(|| {
            let Some(Control::PrivateEnvironment(private_environment)) = self.pop_control() else {
                unreachable!("the private environment of a class is what is going on");
            };
            private_environment
        });
        self.end_class_scope(environment);
        self.put(op::NewClass {
            dst,
            super_class,
            proto_parent,
            computed_name,
            class_environment: environment.register,
            class_blueprint_index: blueprint_index,
            lhs_name,
            element_keys_count: u32_from_usize(element_keys.len()),
            element_keys: &element_keys,
        });
        if let Some(private_environment) = private_environment {
            self.leave_class_private_environment(private_environment);
        }
        self.strict = saved_strict;
        self.release_held_registers(held);
        self.push_value(dst, first_temporary);
        Ok(())
    }

    /// The private names of a class come next, whose environment is current until the class is done.
    fn enter_class_private_environment(&mut self) {
        self.put(op::CreatePrivateEnvironment {});
        let outer_handler = self.handler();
        // NB: An exception that leaves the function takes the private environment with its execution context.
        let handler = outer_handler.map(|_| Handler {
            label: self.new_label(),
            catches: false,
        });
        if handler.is_some() {
            self.set_handler(handler);
        }
        let private_environment = ClassPrivateEnvironment { handler, outer_handler };
        self.push_control(Control::PrivateEnvironment(private_environment));
    }

    /// The class is done, and the private environment around it is current again, also for exceptions.
    fn leave_class_private_environment(&mut self, private_environment: ClassPrivateEnvironment) {
        self.put(op::LeavePrivateEnvironment {});
        let Some(handler) = private_environment.handler else {
            return;
        };
        self.set_handler(private_environment.outer_handler);
        let end_label = self.new_label();
        self.jump(end_label);
        self.bind_reachable_label(handler.label);
        let error = self.temporary();
        let value_feedback = self.next_value_feedback();
        self.put(op::Catch {
            value_feedback,
            dst: error,
        });
        self.put(op::LeavePrivateEnvironment {});
        self.put(op::Throw { src: error });
        self.bind_label(end_label);
    }

    /// A static method gets defined on the class before the key of the next element gets evaluated, which fails for the
    /// non-configurable `prototype` of the class. `NewClass` defines them all at once, so it has to fail here instead.
    fn throw_if_key_of_static_method_is_prototype(&mut self, key: Operand) {
        let prototype = self.add_constant_string_view(crate::utf16::Utf16View::Ascii(b"prototype"));
        let first_temporary = self.next_temporary();
        let is_prototype = self.temporary();
        let arith_feedback = self.next_arith_feedback();
        self.emit(op::StrictlyEquals {
            arith_feedback,
            dst: is_prototype,
            lhs: key,
            rhs: prototype,
        });
        let not_prototype = self.new_label();
        self.jump_unless(is_prototype, not_prototype);
        let error_string = self.intern_string_text(crate::utf16::Utf16View::Ascii(
            b"Object's [[DefineOwnProperty]] method returned false",
        ));
        self.put(op::NewTypeError {
            dst: is_prototype,
            error_string,
        });
        self.put(op::Throw { src: is_prototype });
        self.bind_label(not_prototype);
        self.release_temporaries_from(first_temporary);
    }

    /// The key of an element of a class, which `NewClass` turns into a property key.
    fn sweep_class_element_key(&mut self, key: ClassElementKey, first_row: usize) -> Result<Operand, OutOfStack> {
        let tree = self.tree;
        let key = match key {
            ClassElementKey::Name(name) => return Ok(self.add_constant_string_view(tree.name_text(name))),
            ClassElementKey::Expression(key) => key,
        };
        match tree.node_tag(key) {
            tag::STR_KEY => {
                let operand = match tree.string_literal_text(key) {
                    StringLiteralText::Source(text) => self.add_constant_string_view(text),
                    StringLiteralText::Processed(value) => self.context.constants.add_string(value),
                };
                Ok(operand)
            }
            tag::NUM_KEY => Ok(self.context.constants.add_number(tree.number_value(key))),
            _ => {
                let value = self.sweep_value(first_row, key)?;
                // NB: A key is what it is by the time the next one gets evaluated.
                Ok(emit_to_primitive_property_key(self, &ScopedOperand::plain(value)).operand())
            }
        }
    }

    /// What `NewClass` has to know of an element of a class.
    fn pending_class_element(
        &mut self,
        element: ClassElementData,
        key: Option<ClassElementKey>,
    ) -> PendingClassElement {
        let tree = self.tree;
        let private_identifier = key.and_then(|key| match key {
            ClassElementKey::Expression(key) if tree.node_tag(key) == tag::PRIVATE_NAME => {
                let mut text = Utf16StringBuilder::new();
                text.append_ascii("#");
                text.append(tree.name_text(Name(tree.node_a(key))));
                Some(text.to_utf16_string())
            }
            _ => None,
        });
        let is_private = private_identifier.is_some();
        match element {
            ClassElementData::Method {
                function,
                kind,
                is_static,
                ..
            } => {
                let kind = match kind {
                    ClassMethodKind::Method => ClassElementKind::Method,
                    ClassMethodKind::Getter => ClassElementKind::Getter,
                    ClassMethodKind::Setter => ClassElementKind::Setter,
                };
                let index = self.register_function_of_class(tree.node_b(function));
                PendingClassElement {
                    kind: kind as u8,
                    is_static,
                    is_private,
                    private_identifier,
                    shared_function_data_index: Some(index),
                    has_initializer: false,
                    literal_value_kind: PendingLiteralValueKind::None,
                    literal_value_number: 0.0,
                    literal_value_string: None,
                }
            }
            ClassElementData::Field {
                initializer,
                field,
                is_static,
                ..
            } => {
                let literal = initializer.map(|initializer| literal_initializer(self, initializer));
                let is_literal = literal.as_ref().is_some_and(|literal| literal.kind.is_some());
                let shared_function_data_index = (initializer.is_some() && !is_literal).then(|| {
                    let row = tree.extra_at(tree.node_b(NodeId(tree.node_b(field))) + 1);
                    let name = class_field_name(tree, field);
                    self.register_field_initializer(row, name, is_private)
                });
                let literal = literal.unwrap_or_default();
                PendingClassElement {
                    kind: ClassElementKind::Field as u8,
                    is_static,
                    is_private,
                    private_identifier,
                    shared_function_data_index,
                    has_initializer: initializer.is_some(),
                    literal_value_kind: literal.kind.unwrap_or(PendingLiteralValueKind::None),
                    literal_value_number: literal.number,
                    literal_value_string: literal.string,
                }
            }
            ClassElementData::StaticInitializer { block } => {
                let row = tree.extra_at(tree.node_b(block) + 1);
                let index = self.register_function_of_class(row);
                PendingClassElement {
                    kind: ClassElementKind::StaticInitializer as u8,
                    is_static: true,
                    is_private: false,
                    private_identifier: None,
                    shared_function_data_index: Some(index),
                    has_initializer: false,
                    literal_value_kind: PendingLiteralValueKind::None,
                    literal_value_number: 0.0,
                    literal_value_string: None,
                }
            }
        }
    }

    /// The initializer of a field, which runs as a function of its own. Its rows come after the rows of the key.
    pub(super) fn sweep_field_initializer(&mut self, field: NodeId) -> Result<(), OutOfStack> {
        let tree = self.tree;
        let key = NodeId(tree.node_a(field));
        let initializer = NodeId(tree.node_a(NodeId(tree.node_b(field))));
        let is_computed = tree.node_tag(field) == tag::COMPUTED_CLASS_FIELD;
        let first_row = key.index() + 1 + usize::from(is_computed || tree.node_tag(key) == tag::BIGINT);
        let value = if let Some(name) = class_field_name(tree, field) {
            let name = self.intern_identifier_text(crate::utf16::Utf16View::of_string(&name));
            self.set_pending_function_name(initializer, Some(name));
            self.sweep_value(first_row, initializer)?
        } else if is_anonymous_function_definition(tree, initializer) {
            // NB: A computed key is only known once the class is defined, so the function reads it.
            let name = self.temporary();
            self.put(op::GetClassFieldInitializerName { dst: name });
            if tree.node_tag(tree.skip_parens(initializer)) == tag::CLASS {
                self.context.stream.pending_computed_class_name = Some(name);
                self.sweep_value(first_row, initializer)?
            } else {
                let value = self.sweep_value(first_row, initializer)?;
                self.put(op::SetFunctionName {
                    function: value,
                    name,
                    prefix: libjs_abi::FunctionNamePrefix::None as u32,
                });
                value
            }
        } else {
            self.sweep_value(first_row, initializer)?
        };
        self.put(op::Return { value });
        Ok(())
    }

    /// A static block, which runs as a function of its own.
    pub(super) fn sweep_static_block(&mut self, block: NodeId) -> Result<(), OutOfStack> {
        self.create_arguments_object_of_static_block();
        self.sweep_rows(FIRST_ROW_OF_REPARSED_FUNCTION, block.index())?;
        self.end_disposal();
        Ok(())
    }

    /// The default constructor of a class, which has no code in the source.
    pub(super) fn generate_default_constructor(&mut self, is_derived: bool) {
        if !is_derived {
            return;
        }
        let arguments = self.temporary();
        self.put(op::CreateRestParams {
            dst: arguments,
            rest_index: 0,
        });
        let super_constructor = self.temporary();
        self.put(op::GetSuperConstructor { dst: super_constructor });
        let dst = self.temporary();
        let value_feedback = self.next_value_feedback();
        let call_feedback = self.next_call_feedback();
        self.put(op::SuperCallWithArgumentArray {
            value_feedback,
            call_feedback,
            dst,
            super_constructor,
            arguments,
            is_synthetic: true,
        });
        self.put(op::Return { value: dst });
    }

    /// The arguments that a tag of a template gets: the object of its strings and then its substitutions, on the stack.
    fn sweep_template_of_tag(&mut self, template: NodeId, first_row: usize) -> Result<(), OutOfStack> {
        let tree = self.tree;
        let elements = tree.node_list(tree.node_a(template));
        let mut strings = Vec::with_capacity(elements.len() + 1);
        for element in elements.iter().step_by(2) {
            let value = match tree.template_chunk_text(element, EscapeMode::TaggedTemplate) {
                Some(StringLiteralText::Source(text)) => self.add_constant_string_view(text),
                Some(StringLiteralText::Processed(value)) => self.context.constants.add_string(value),
                None => self.context.constants.add_undefined(),
            };
            strings.push(value);
        }
        for element in elements.iter().step_by(2) {
            let units = tree.template_chunk_units(element);
            strings.push(if units.code_units().any(|unit| unit == u16::from(b'\r')) {
                self.context.constants.add_string(raw_template_value(units))
            } else {
                self.add_constant_string_view(units)
            });
        }
        let first_temporary = self.next_temporary();
        let dst = self.temporary();
        let cache = self.next_template_object_cache();
        self.put(op::GetTemplateObject {
            dst,
            strings_count: u32_from_usize(strings.len()),
            cache,
            strings: &strings,
        });
        self.push_value(dst, first_temporary);
        let mut row = first_row;
        for (index, element) in elements.iter().enumerate() {
            if index % 2 == 1 {
                self.sweep_rows(row, element.index() + 1)?;
            }
            row = element.index() + 1;
        }
        Ok(())
    }

    /// An untagged template. Each substitution becomes a string before the next one gets evaluated.
    fn sweep_template(&mut self, template: NodeId, first_row: usize) -> Result<(), OutOfStack> {
        let tree = self.tree;
        let first_temporary = self.next_temporary();
        let dst = self.temporary();
        let mut is_first = true;
        let mut row = first_row;
        let elements = tree.node_list(tree.node_a(template));
        let is_single_segment = elements
            .iter()
            .filter(|&element| !tree.is_empty_template_element(element))
            .count()
            == 1;
        for element in elements.iter() {
            let start_row = row;
            row = element.index() + 1;
            if tree.is_empty_template_element(element) {
                continue;
            }
            let (value, is_string) = if tree.node_tag(element) == tag::TEMPLATE_CHUNK {
                let text = tree
                    .template_chunk_text(element, EscapeMode::StringLiteral)
                    .expect("only a tagged template can have an invalid escape");
                let operand = match text {
                    StringLiteralText::Source(text) => self.add_constant_string_view(text),
                    StringLiteralText::Processed(value) => self.context.constants.add_string(value),
                };
                (operand, true)
            } else {
                let is_string = tree.node_tag(tree.skip_parens(element)) == tag::STR;
                (self.sweep_value(start_row, element)?, is_string)
            };
            self.current_source_offset = SourceOffset(tree.node_pos(template));
            // OPTIMIZATION: Convert a constant at compile time.
            if is_single_segment && let Some(string) = self.context.constants.get(value).and_then(constant_to_string) {
                let string = self.context.constants.add_string(string);
                self.release_temporaries_from(first_temporary);
                self.push_value(string, first_temporary);
                return Ok(());
            }
            if !is_first {
                self.put(op::ConcatString { dst, src: value });
            } else if is_string {
                self.put(op::Mov { dst, src: value });
            } else {
                self.put(op::ToString { dst, value });
            }
            is_first = false;
            self.release_temporaries_from(dst.index() + 1);
        }
        if is_first {
            let empty = self.context.constants.add_string(ak::Utf16String::default());
            self.release_temporaries_from(first_temporary);
            self.push_value(empty, first_temporary);
            return Ok(());
        }
        self.push_value(dst, first_temporary);
        Ok(())
    }

    // --- Statements ---

    /// `with`, in whose body a name can mean anything.
    fn sweep_with(&mut self, statement: NodeId, first_row: usize) -> Result<(), OutOfStack> {
        let tree = self.tree;
        let object = NodeId(tree.node_a(statement));
        let held = self.hold_registers();
        let value = self.sweep_value(first_row, object)?;
        self.current_source_offset = SourceOffset(tree.node_pos(statement));
        self.reset_completion();
        let environment = self.enter_object_environment(value, held);
        self.push_control(Control::Block {
            environment: Some(environment),
        });
        self.end_of_statement();
        self.sweep_rows(object.index() + 1, statement.index())?;
        self.pop_control();
        self.leave_object_environment(environment);
        self.end_of_statement();
        Ok(())
    }

    fn sweep_labeled(&mut self, statement: NodeId, first_row: usize) -> Result<(), OutOfStack> {
        let break_label = self.new_label();
        self.push_control(Control::Labeled {
            name: Name(self.tree.node_a(statement)),
            break_label,
        });
        self.sweep_rows(first_row, statement.index())?;
        self.pop_control();
        self.bind_label(break_label);
        Ok(())
    }

    /// `for`, whose rows come as init, test, update and body, and whose code comes as init, body, update and test.
    fn sweep_for(&mut self, statement: NodeId, first_row: usize, position: u32) -> Result<(), OutOfStack> {
        let tree = self.tree;
        let init = tree.node_a(statement);
        let extra = tree.node_b(statement);
        let (test, update, body) = (tree.extra_at(extra), tree.extra_at(extra + 1), tree.extra_at(extra + 2));

        self.reset_completion();
        let bindings = self.scope_bindings_at(position);
        let environment = self.begin_scope(position, false);
        self.push_control(Control::Block { environment });

        let mut row = first_row;
        if init != 0 {
            self.sweep_rows(row, init as usize + 1)?;
            row = init as usize + 1;
            if !matches!(
                tree.node_tag(NodeId(init)),
                tag::VAR | tag::LET | tag::CONST | tag::USING | tag::AWAIT_USING
            ) {
                self.drop_value();
            }
            self.end_of_statement();
        }
        // NB: Every iteration has a copy of the bindings of a `let` that closures can hold on to.
        let per_iteration_bindings = (init != 0 && tree.node_tag(NodeId(init)) == tag::LET)
            .then_some(())
            .and(bindings.zip(environment));
        if let Some((bindings, environment)) = per_iteration_bindings {
            self.copy_scope_environment(bindings, environment);
        }

        let test_first_row = row;
        if test != 0 {
            row = test as usize + 1;
        }
        let update_first_row = row;
        if update != 0 {
            row = update as usize + 1;
        }

        let body_label = self.new_label();
        let continue_label = self.new_label();
        let test_label = self.new_label();
        let end_label = self.new_label();
        if test != 0 {
            self.jump(test_label);
        }
        self.bind_reachable_label(body_label);
        let break_depth = self.control_depth() + 1;
        self.push_control(Control::Loop {
            continue_label,
            break_label: end_label,
            break_depth,
            kind: LoopKind::Structured,
        });
        self.sweep_rows(row, body as usize + 1)?;
        self.pop_control();

        self.bind_label(continue_label);
        if let Some((bindings, environment)) = per_iteration_bindings {
            self.copy_scope_environment(bindings, environment);
        }
        if update != 0 {
            self.set_unused_value_row(update);
            self.sweep_rows(update_first_row, update as usize + 1)?;
            self.drop_value();
            self.end_of_statement();
        }
        if test != 0 {
            self.bind_reachable_label(test_label);
            let value = self.sweep_value(test_first_row, NodeId(test))?;
            self.end_of_statement();
            self.jump_back_if(value, body_label);
        } else {
            self.jump_back(body_label);
        }
        self.bind_label(end_label);

        self.end_disposal();
        self.pop_control();
        self.end_scope(environment);
        Ok(())
    }

    fn sweep_switch(&mut self, statement: NodeId, first_row: usize, first_function: u32) -> Result<(), OutOfStack> {
        let tree = self.tree;
        let discriminant = NodeId(tree.node_a(statement));
        let cases = tree.switch_data(statement).cases;

        let held = self.hold_registers();
        let value = self.sweep_value(first_row, discriminant)?;
        // NB: A test can write what the discriminant was read from.
        let register = self.held_register();
        self.move_value_to(register, value);
        self.end_of_statement();
        self.reset_completion();

        let environment = self.begin_scope(tree.node_pos(statement), false);
        self.push_control(Control::Block { environment });
        if first_function != 0 {
            self.instantiate_functions_of_block(NodeId(first_function));
        }
        let end_label = self.new_label();

        let mut labels = Vec::with_capacity(cases.len());
        let mut default_label = None;
        let mut row = discriminant.index() + 1;
        for case in cases.iter() {
            let label = self.new_label();
            labels.push(label);
            let test = tree.node_a(case);
            if test != 0 {
                self.sweep_rows(row, test as usize + 1)?;
                self.value_of_constant(register);
                self.binary(BinaryOp::StrictlyEquals);
                let (is_equal, _) = self.pop_operand();
                self.jump_if(is_equal, label);
                self.end_of_statement();
            } else {
                default_label = Some(label);
            }
            row = case.index() + 1;
        }
        self.jump(default_label.unwrap_or(end_label));

        self.push_control(Control::Switch { break_label: end_label });
        let initialized_locals = self.context.initialized_locals.clone();
        let mut row = discriminant.index() + 1;
        for (index, case) in cases.iter().enumerate() {
            self.bind_label(labels[index]);
            // NB: The code of a case runs without the cases before it having run.
            self.context.initialized_locals.clone_from(&initialized_locals);
            let test = tree.node_a(case);
            let first_statement_row = if test != 0 { test as usize + 1 } else { row };
            self.sweep_rows(first_statement_row, case.index())?;
            row = case.index() + 1;
        }
        self.pop_control();
        self.bind_label(end_label);
        self.context.initialized_locals = initialized_locals;

        self.pop_control();
        self.end_scope(environment);
        self.release_held_registers(held);
        self.end_of_statement();
        Ok(())
    }

    // --- try ---

    /// The code of a `try` got to its end, and whatever comes next.
    fn leave_try_part(&mut self, finally: Option<u32>, end_label: Label) {
        let Some(finally) = finally else {
            self.jump(end_label);
            return;
        };
        self.leave_finally_part(finally);
    }

    fn sweep_try(&mut self, statement: NodeId, first_row: usize, first_function: u32) -> Result<(), OutOfStack> {
        let tree = self.tree;
        let data = tree.try_data(statement);
        let catch_position = tree.extra_at(tree.node_b(statement) + 3);

        self.reset_completion();
        let held = self.hold_registers();
        let outer_handler = self.handler();
        let entry_environment = self.current_lexical_environment();
        let end_label = self.new_label();
        let finally = data.finalizer.map(|_| {
            let index = self.begin_finally();
            self.push_control(Control::Finally(index));
            index
        });
        let finally_handler = finally.map(|_| Handler {
            label: self.new_label(),
            catches: false,
        });
        let catch_handler = data.handler.map(|_| Handler {
            label: self.new_label(),
            catches: true,
        });

        self.set_handler(catch_handler.or(finally_handler).or(outer_handler));
        self.sweep_rows(first_row, data.block.index() + 1)?;
        self.leave_try_part(finally, end_label);
        let mut row = data.block.index() + 1;

        if let (Some(handler), Some(body)) = (catch_handler, data.handler) {
            self.set_handler(finally_handler.or(outer_handler));
            self.bind_reachable_label(handler.label);
            self.current_source_offset = SourceOffset(tree.node_pos(statement));
            self.sweep_catch(
                data.parameter,
                body,
                row,
                catch_position,
                &entry_environment,
                first_function,
            )?;
            self.leave_try_part(finally, end_label);
            row = body.index() + 1;
        }

        if let (Some(handler), Some(finalizer), Some(finally)) = (finally_handler, data.finalizer, finally) {
            self.pop_control();
            self.set_handler(outer_handler);
            let finally = finally as usize;
            let body = self.bind_finally_handler(finally, handler, &entry_environment);
            self.bind_label(body);
            let saved_completion = self.save_completion();
            self.sweep_rows(row, finalizer.index() + 1)?;
            self.restore_completion(saved_completion);
            self.finally_dispatch(finally, end_label);
        } else {
            self.set_handler(outer_handler);
        }

        self.bind_label(end_label);
        self.release_held_registers(held);
        self.end_of_statement();
        Ok(())
    }

    fn sweep_catch(
        &mut self,
        parameter: Option<NodeId>,
        body: NodeId,
        first_row: usize,
        position: u32,
        entry_environment: &ScopedOperand,
        first_function: u32,
    ) -> Result<(), OutOfStack> {
        let tree = self.tree;
        let held = self.hold_registers();
        let value = self.held_register();
        let value_feedback = self.next_value_feedback();
        self.put(op::Catch {
            value_feedback,
            dst: value,
        });
        self.restore_lexical_environment(entry_environment);
        self.reset_completion();

        let environment = self.begin_scope(position, true);
        self.push_control(Control::Block { environment });
        let mut row = first_row;
        if let Some(parameter) = parameter {
            let position = self.current_source_offset;
            self.declare_target_names(parameter, DeclarationKind::Let)?;
            self.current_source_offset = position;
            if matches!(tree.node_tag(parameter), tag::IDENT) {
                self.note_catch_parameter(Name(tree.node_a(parameter)));
            }
            self.bind_target(parameter, first_row, value, PatternMode::Declares(DeclarationKind::Let))?;
            row = parameter.index() + 1;
        }

        let body_environment = self.begin_scope(tree.node_pos(body), false);
        self.push_control(Control::Block {
            environment: body_environment,
        });
        if first_function != 0 {
            self.instantiate_functions_of_block(NodeId(first_function));
        }
        self.sweep_rows(row, body.index())?;
        self.end_disposal();
        self.pop_control();
        self.context
            .scope
            .note_block_range(tree.node_pos(body), tree.end_of_block(body.0));
        self.end_scope(body_environment);

        self.pop_control();
        self.end_scope(environment);
        self.release_held_registers(held);
        Ok(())
    }

    // --- Loops over values ---

    fn loop_target(&mut self, statement: NodeId, first_row: usize) -> Result<LoopTarget, OutOfStack> {
        let tree = self.tree;
        let left = NodeId(tree.node_a(statement));
        let left_tag = tree.node_tag(left);
        if matches!(
            left_tag,
            tag::VAR | tag::LET | tag::CONST | tag::USING | tag::AWAIT_USING
        ) {
            let declarator = tree.node_list(tree.node_a(left)).at(0);
            // NB: `for (var a = b in c)` is a declaration like any other first, which comes before the loop has a scope.
            if tree.node_b(declarator) != 0 {
                self.sweep_rows(first_row, left.index() + 1)?;
                self.end_of_statement();
            }
            // NB: The pattern of a declaration has a mark of its own, which nothing goes through here.
            let first_row = first_row + usize::from(tree.ast.tag[first_row] == tag::MARK_SUBTREE);
            return Ok(LoopTarget::Declared {
                target: NodeId(tree.node_a(declarator)),
                first_row,
                kind: declaration_kind_of_mark(u32::from(left_tag)),
                resource: resource_of_mark(u32::from(left_tag)),
            });
        }
        if matches!(left_tag, tag::ARRAY_PATTERN | tag::OBJECT_PATTERN) {
            return Ok(LoopTarget::Pattern {
                target: left,
                first_row: first_row + 1,
            });
        }
        Ok(LoopTarget::Expression {
            target: tree.skip_parens(left),
            first_row,
        })
    }

    /// Gives `value` to the target of a loop.
    fn assign_to_loop_target(&mut self, target: LoopTarget, value: Operand) -> Result<(), OutOfStack> {
        match target {
            LoopTarget::Declared {
                target,
                first_row,
                kind,
                resource,
            } => {
                if let Some(is_async) = resource {
                    self.begin_disposal(is_async);
                    self.add_disposable_resource(value, is_async);
                }
                self.bind_target(target, first_row, value, PatternMode::Declares(kind))
            }
            LoopTarget::Pattern { target, first_row } => {
                self.bind_target(target, first_row, value, PatternMode::Assigns)
            }
            LoopTarget::Expression { target, first_row } => {
                let position = self.current_source_offset;
                self.sweep_reference(target, first_row)?;
                self.current_source_offset = position;
                if self.tree.node_tag(target) == tag::IDENT {
                    let ident = self.identifier_of(Name(self.tree.node_a(target)));
                    self.check_target_is_initialized(&ident);
                }
                let target = self.pop_value();
                self.store_to_target(target, value, false);
                Ok(())
            }
        }
    }

    /// The head of a `for-in` or `for-of` loop: its bindings get declared, and the expression gets evaluated with them
    /// being in their dead zone. Gives what the bindings need in each iteration, and leaves the value in `source`.
    fn sweep_loop_head(
        &mut self,
        statement: NodeId,
        target: LoopTarget,
        position: u32,
        source: Operand,
    ) -> Result<Option<ScopeBindings>, OutOfStack> {
        let tree = self.tree;
        let right = NodeId(tree.extra_at(tree.node_b(statement)));
        let bindings = self.scope_bindings_at(position);
        let environment = self.begin_scope(position, false);
        self.push_control(Control::Block { environment });
        if let LoopTarget::Declared { target, kind, .. } = target {
            self.declare_target_names(target, kind)?;
        }
        let value = self.sweep_value(tree.node_a(statement) as usize + 1, right)?;
        self.current_source_offset = SourceOffset(tree.node_pos(statement));
        self.move_value_to(source, value);
        self.pop_control();
        if let Some(environment) = environment {
            self.leave_scope_environment(environment);
        }
        self.end_of_statement();
        Ok(bindings.filter(|_| environment.is_some()))
    }

    /// The code that runs for each value of a loop.
    fn sweep_loop_body(
        &mut self,
        statement: NodeId,
        target: LoopTarget,
        bindings: Option<ScopeBindings>,
        value: Operand,
    ) -> Result<(), OutOfStack> {
        let tree = self.tree;
        let extra = tree.node_b(statement);
        let (right, body) = (tree.extra_at(extra), tree.extra_at(extra + 1));
        let environment: Option<ScopeEnvironment> =
            bindings.map(|bindings| self.enter_scope_environment(bindings, false, None));
        self.push_control(Control::Block { environment });
        self.current_source_offset = SourceOffset(tree.node_pos(statement));
        if let LoopTarget::Declared {
            target: pattern, kind, ..
        } = target
            && kind != DeclarationKind::Var
            && matches!(tree.node_tag(pattern), tag::ARRAY_PATTERN | tag::OBJECT_PATTERN)
            && tree.pattern_contains_expression(pattern.0)
        {
            // NB: The bindings of each iteration are new, and the code of the pattern sees them in their dead zone.
            for_each_bound_name(tree, pattern, &mut |name| {
                let ident = self.identifier_of(name);
                self.put_in_dead_zone(ident);
            });
        }
        self.assign_to_loop_target(target, value)?;
        self.end_of_statement();
        self.sweep_rows(right as usize + 1, body as usize + 1)?;
        self.end_disposal();
        self.pop_control();
        if let Some(environment) = environment {
            self.leave_scope_environment(environment);
        }
        Ok(())
    }

    fn sweep_for_in(&mut self, statement: NodeId, first_row: usize, position: u32) -> Result<(), OutOfStack> {
        let target = self.loop_target(statement, first_row)?;
        let held = self.hold_registers();
        let source = self.held_register();
        let bindings = self.sweep_loop_head(statement, target, position, source)?;
        self.reset_completion();

        let receiver = self.held_register();
        let keys = self.held_register();
        let cursor = self.held_register();
        let value = self.held_register();
        let done = self.held_register();
        let end_label = self.new_label();
        let update_label = self.new_label();
        let not_nullish = self.new_label();
        self.reference_label(end_label);
        self.reference_label(not_nullish);
        self.put(op::JumpNullish {
            condition: source,
            true_target: end_label,
            false_target: not_nullish,
        });
        self.bind_label(not_nullish);
        let cache = self.next_object_property_iterator_cache();
        self.put(op::GetObjectPropertyIterator {
            dst_keys: keys,
            dst_receiver: receiver,
            object: source,
            cache,
        });
        let zero = self.add_constant_i32(0);
        self.put(op::Mov {
            dst: cursor,
            src: zero.operand(),
        });

        self.bind_reachable_label(update_label);
        self.put(op::ObjectPropertyIteratorNext {
            dst_value: value,
            dst_done: done,
            receiver,
            keys,
            cursor,
        });
        self.jump_if(done, end_label);

        let break_depth = self.control_depth() + 1;
        self.push_control(Control::Loop {
            continue_label: update_label,
            break_label: end_label,
            break_depth,
            kind: LoopKind::Structured,
        });
        self.sweep_loop_body(statement, target, bindings, value)?;
        self.pop_control();
        self.jump_back(update_label);
        self.bind_label(end_label);

        self.context.scope.block_end();
        self.release_held_registers(held);
        self.end_of_statement();
        Ok(())
    }

    fn sweep_for_of(
        &mut self,
        statement: NodeId,
        first_row: usize,
        position: u32,
        is_await: bool,
    ) -> Result<(), OutOfStack> {
        let target = self.loop_target(statement, first_row)?;
        let held = self.hold_registers();
        let source = self.held_register();
        let bindings = self.sweep_loop_head(statement, target, position, source)?;
        self.reset_completion();

        let object = self.held_register();
        let next = self.held_register();
        let iterator_done = self.held_register();
        let value = self.held_register();
        let done = self.held_register();
        let cache = self.next_property_lookup_cache();
        self.put(op::GetIterator {
            dst_iterator_object: object,
            dst_iterator_next: next,
            dst_iterator_done: iterator_done,
            iterable: source,
            hint: if is_await {
                libjs_abi::IteratorHint::Async
            } else {
                libjs_abi::IteratorHint::Sync
            } as u32,
            fast_array_mode_allowed: !is_await,
            cache,
        });

        let outer_handler = self.handler();
        let entry_environment = self.current_lexical_environment();
        let end_label = self.new_label();
        let update_label = self.new_label();
        let close_handler = Handler {
            label: self.new_label(),
            catches: false,
        };
        let iterator = OpenIterator {
            object,
            next,
            done: iterator_done,
            outer_handler,
            may_be_done: false,
            is_async: is_await,
        };
        let iterator_depth = self.control_depth();
        self.push_control(Control::Iterator(iterator));
        self.push_control(Control::Loop {
            continue_label: update_label,
            break_label: end_label,
            break_depth: iterator_depth,
            kind: LoopKind::Structured,
        });

        self.bind_reachable_label(update_label);
        if is_await {
            let held = self.hold_registers();
            let result = self.allocate_register();
            // NB: A result that is not an object only throws once it is awaited.
            let value_feedback = self.next_value_feedback();
            let call_feedback = self.next_call_feedback();
            self.emit(op::Call {
                value_feedback,
                call_feedback,
                dst: result.operand(),
                callee: next,
                this_value: object,
                argument_count: 0,
                expression_string: None,
                arguments: &[],
            });
            let awaited = generate_await_of_value(self, &result);
            self.put(op::ThrowIfNotObject { src: awaited.operand() });
            let done_key = self.intern_property_key_text(crate::utf16::Utf16View::Ascii(b"done"));
            emit_get_by_key(self, &ScopedOperand::plain(done), &awaited, done_key, None);
            self.jump_if(done, end_label);
            let value_key = self.intern_property_key_text(crate::utf16::Utf16View::Ascii(b"value"));
            emit_get_by_key(self, &ScopedOperand::plain(value), &awaited, value_key, None);
            self.release_held_registers(held);
        } else {
            self.put(op::IteratorNextUnpack {
                dst_value: value,
                dst_done: done,
                iterator_object: object,
                iterator_next: next,
                iterator_done,
            });
            self.jump_if(done, end_label);
        }

        // NB: What goes wrong in the iterator itself is no reason to close it.
        self.set_handler(Some(close_handler));
        self.sweep_loop_body(statement, target, bindings, value)?;
        self.jump_back(update_label);
        self.pop_control();
        self.pop_control();
        self.set_handler(outer_handler);

        self.bind_reachable_label(close_handler.label);
        self.close_iterator_after_exception(iterator, &entry_environment);

        self.bind_label(end_label);
        self.context.scope.block_end();
        self.release_held_registers(held);
        self.end_of_statement();
        Ok(())
    }

    /// Closes an iterator on the way to whoever handles an exception, which stays what gets thrown.
    fn close_iterator_after_exception(&mut self, iterator: OpenIterator, entry_environment: &ScopedOperand) {
        let error = self.temporary();
        let value_feedback = self.next_value_feedback();
        self.put(op::Catch {
            value_feedback,
            dst: error,
        });
        self.restore_lexical_environment(entry_environment);
        let rethrow = self.new_label();
        if iterator.is_async {
            // NB: Whatever goes wrong while closing makes no difference to what gets thrown.
            let handler = self.handler();
            let ignore_error = self.new_label();
            self.set_handler(Some(Handler {
                label: ignore_error,
                catches: false,
            }));
            let held = self.hold_registers();
            self.put_async_iterator_return(iterator, rethrow);
            self.release_held_registers(held);
            self.jump(rethrow);
            self.set_handler(handler);
            self.bind_reachable_label(ignore_error);
            let ignored = self.temporary();
            let value_feedback = self.next_value_feedback();
            self.put(op::Catch {
                value_feedback,
                dst: ignored,
            });
            self.bind_reachable_label(rethrow);
            self.put(op::Throw { src: error });
            return;
        }
        if iterator.may_be_done {
            self.jump_if(iterator.done, rethrow);
        }
        let cache = self.next_property_lookup_cache();
        self.put(op::IteratorClose {
            iterator_object: iterator.object,
            iterator_next: iterator.next,
            iterator_done: iterator.done,
            completion_type: libjs_abi::CompletionType::Throw as u32,
            completion_value: error,
            cache,
        });
        self.bind_label(rethrow);
        self.put(op::Throw { src: error });
    }

    // --- Parameters ---

    /// What the parameters of `function` are, for `declare_parameters()`.
    pub(super) fn sweep_parameter_names(&mut self, function: NodeId) {
        let tree = self.tree;
        let parameters = parameters_of(tree, function);
        let mut names = std::mem::take(&mut self.context.scope.spare_parameter_names);
        let mut shapes = std::mem::take(&mut self.context.scope.spare_parameter_shapes);
        if self.context.scope.has_simple_parameters() {
            names.extend(parameters.iter().map(|parameter| Name(tree.node_a(parameter))));
        } else {
            for parameter in parameters.iter() {
                let first_name = names.len();
                for_each_bound_name(tree, parameter, &mut |name| names.push(name));
                let target = match tree.node_tag(parameter) {
                    tag::REST | tag::ASSIGN_PATTERN => NodeId(tree.node_a(parameter)),
                    _ => parameter,
                };
                shapes.push(ParameterShape {
                    name_count: u32_from_usize(names.len() - first_name),
                    is_name: tree.node_tag(target) == tag::IDENT,
                    has_expression: parameter_has_expression(tree, parameter),
                });
            }
        }
        self.declare_parameters(&names, &shapes);
        names.clear();
        shapes.clear();
        self.context.scope.spare_parameter_names = names;
        self.context.scope.spare_parameter_shapes = shapes;
    }

    /// The functions that a block declares exist from its start. `function` is the first one, and each knows the next.
    pub(super) fn instantiate_functions_of_block(&mut self, mut function: NodeId) {
        let tree = self.tree;
        let mut names: Vec<Name> = Vec::new();
        while function.0 != 0 {
            let name = Name(tree.node_b(NodeId(function.0 + 1)));
            self.instantiate_function_of_block(name, tree.node_b(function), !names.contains(&name));
            names.push(name);
            function = NodeId(tree.node_a(function));
        }
    }

    /// The code that gives parameters that are more than a name their values, which comes first in the rows.
    pub(super) fn sweep_parameters(&mut self, function: NodeId) -> Result<(), OutOfStack> {
        let tree = self.tree;
        let parameters = parameters_of(tree, function);
        if !self.checks_parameters() {
            self.end_of_parameters();
            return Ok(());
        }
        let mut row = FIRST_ROW_OF_REPARSED_FUNCTION;
        for (index, parameter) in parameters.iter().enumerate() {
            let first_row = row;
            row = parameter.index() + 1;
            let index = u32_from_usize(index);
            let argument = Operand::argument(index);
            self.current_source_offset = SourceOffset(tree.node_pos(parameter));
            let target = match tree.node_tag(parameter) {
                tag::REST => {
                    self.put(op::CreateRestParams {
                        dst: argument,
                        rest_index: index,
                    });
                    NodeId(tree.node_a(parameter))
                }
                tag::ASSIGN_PATTERN => {
                    let target = NodeId(tree.node_a(parameter));
                    let default = NodeId(tree.node_b(parameter));
                    let name = (tree.node_tag(target) == tag::IDENT).then(|| Name(tree.node_a(target)));
                    self.apply_pattern_default(default, target.index() + 1, argument, name)?;
                    target
                }
                _ => parameter,
            };
            if tree.node_tag(target) == tag::IDENT {
                let ident = self.identifier_of(Name(tree.node_a(target)));
                if ident.is_local() {
                    self.mark_argument_initialized(index);
                } else {
                    let identifier = self.intern_identifier(ident.name);
                    emit_initialize_lexical_binding(self, identifier, argument);
                }
            } else {
                self.bind_target(target, first_row, argument, PatternMode::Declares(DeclarationKind::Let))?;
                self.mark_argument_initialized(index);
            }
            self.end_of_statement();
        }
        self.end_of_parameters();
        Ok(())
    }

    // --- Patterns ---

    /// `let { a } = b`, whose names exist before the initializer and get their values after it.
    fn sweep_pattern_declarator(
        &mut self,
        declarator: NodeId,
        first_row: usize,
        kind: DeclarationKind,
    ) -> Result<(), OutOfStack> {
        let tree = self.tree;
        let target = NodeId(tree.node_a(declarator));
        let init = NodeId(tree.node_b(declarator));
        let position = self.current_source_offset;
        self.declare_target_names(target, kind)?;
        let value = self.sweep_value(target.index() + 1, init)?;
        self.current_source_offset = position;
        // NB: The object of a `with` can have a property of the name of a `var`, which makes it a reference like any other.
        let mode = if kind == DeclarationKind::Var && self.context.scope.is_in_with() {
            PatternMode::Assigns
        } else {
            PatternMode::Declares(kind)
        };
        self.bind_target(target, first_row, value, mode)?;
        self.end_of_statement();
        Ok(())
    }

    /// `[a, b] = c`, whose value is what is on the right side.
    fn sweep_assignment_pattern(&mut self, assignment: NodeId, first_row: usize) -> Result<(), OutOfStack> {
        let tree = self.tree;
        let pattern = NodeId(tree.node_a(assignment));
        let right = NodeId(tree.node_b(assignment));
        let first_temporary = self.next_temporary();
        // NB: Whether some of what follows runs depends on what the pattern finds.
        self.copy_locals_on_stack();
        let value = self.sweep_value(pattern.index() + 1, right)?;
        let dst = self.temporary();
        self.move_value_to(dst, value);
        let stream_mark = self.hold_registers();
        self.current_source_offset = SourceOffset(tree.node_pos(assignment));
        self.bind_target(pattern, first_row, dst, PatternMode::Assigns)?;
        self.release_held_registers(stream_mark);
        self.push_value(dst, first_temporary);
        Ok(())
    }

    /// Evaluates the reference that `element` of an assignment pattern stands for, which stays on the stack until
    /// `bind_target()` gets to it.
    fn prepare_pattern_target(
        &mut self,
        element: NodeId,
        first_row: usize,
        mode: PatternMode,
    ) -> Result<(), OutOfStack> {
        if mode != PatternMode::Assigns {
            return Ok(());
        }
        let tree = self.tree;
        let target = if tree.node_tag(element) == tag::ASSIGN_PATTERN {
            NodeId(tree.node_a(element))
        } else {
            element
        };
        let target = tree.skip_parens(target);
        if matches!(tree.node_tag(target), tag::ARRAY_PATTERN | tag::OBJECT_PATTERN) {
            return Ok(());
        }
        let position = self.current_source_offset;
        self.sweep_reference(target, first_row)?;
        self.current_source_offset = position;
        Ok(())
    }

    /// Evaluates the reference `target`, whose rows start at `first_row`, which stays on the stack for what gets written
    /// to it. Something that is no reference gets evaluated and goes no further.
    fn sweep_reference(&mut self, target: NodeId, first_row: usize) -> Result<(), OutOfStack> {
        let tree = self.tree;
        match tree.node_tag(target) {
            tag::MEMBER | tag::MEMBER_TARGET => {
                self.sweep_rows(first_row, target.index())?;
                self.target_of_member(Name(tree.node_b(target)));
            }
            tag::INDEX | tag::INDEX_TARGET => {
                self.sweep_rows(first_row, target.index())?;
                self.target_of_index();
            }
            tag::SUPER_MEMBER | tag::SUPER_MEMBER_TARGET => {
                self.sweep_rows(first_row, target.index())?;
                self.target_of_super_property(Some(Name(tree.node_b(target))));
            }
            tag::SUPER_INDEX | tag::SUPER_INDEX_TARGET => {
                self.sweep_rows(first_row, target.index())?;
                self.target_of_super_property(None);
            }
            tag::PRIVATE_MEMBER | tag::PRIVATE_MEMBER_TARGET => {
                self.sweep_rows(first_row, target.index())?;
                self.target_of_private_member(Name(tree.node_b(target)));
            }
            target_tag if target_tag == tag::IDENT || (tag::IDENT_MARKED..tag::MEMBER_MARKED).contains(&target_tag) => {
                let ident = self.identifier_of(Name(tree.node_a(target)));
                self.target_of_identifier(ident, TargetBinding::MightAppear);
            }
            _ => {
                self.sweep_rows(first_row, target.index() + 1)?;
                self.invalid_target();
            }
        }
        Ok(())
    }

    /// What is bound by `target`, an identifier or a pattern, gets declared in the order of the source.
    fn declare_target_names(&mut self, target: NodeId, kind: DeclarationKind) -> Result<(), OutOfStack> {
        let tree = self.tree;
        match tree.node_tag(target) {
            tag::ARRAY_PATTERN | tag::OBJECT_PATTERN => {
                for item in tree.node_list(tree.node_a(target)).iter() {
                    let element = match tree.node_tag(item) {
                        tag::HOLE => continue,
                        tag::REST => NodeId(tree.node_a(item)),
                        tag::PROPERTY | tag::COMPUTED_PROPERTY | tag::SHORTHAND => NodeId(tree.node_b(item)),
                        _ => item,
                    };
                    let element = if tree.node_tag(element) == tag::ASSIGN_PATTERN {
                        NodeId(tree.node_a(element))
                    } else {
                        element
                    };
                    self.declare_target_names(element, kind)?;
                }
                Ok(())
            }
            _ => {
                self.current_source_offset = SourceOffset(tree.node_pos(target));
                let ident = self.declare_binding(Name(tree.node_a(target)), kind);
                // NB: Whatever a run through this code before left in a local, it is in its dead zone here.
                if kind != DeclarationKind::Var {
                    self.put_in_dead_zone(ident);
                }
                Ok(())
            }
        }
    }

    /// Puts a lexical binding that lives in a register back in its dead zone.
    fn put_in_dead_zone(&mut self, ident: Identifier) {
        if ident.local_type == Some(LocalType::Variable) {
            let empty = self.context.constants.add_empty();
            self.put(op::Mov {
                dst: Operand::local(ident.local_index),
                src: empty,
            });
        }
    }

    /// Gives `value` to `target`, an identifier or a pattern, whose rows start at `first_row`.
    fn bind_target(
        &mut self,
        target: NodeId,
        first_row: usize,
        value: Operand,
        mode: PatternMode,
    ) -> Result<(), OutOfStack> {
        let tree = self.tree;
        let target = tree.skip_parens(target);
        match (tree.node_tag(target), mode) {
            (tag::ARRAY_PATTERN, _) => self.bind_array_pattern(target, first_row, value, mode),
            (tag::OBJECT_PATTERN, _) => self.bind_object_pattern(target, first_row, value, mode),
            (_, PatternMode::Assigns) => {
                let reference = self.pop_value();
                if reference.kind == ValKind::LocalTarget {
                    let ident = self.identifier_of(Name(reference.name));
                    self.check_target_is_initialized(&ident);
                }
                self.store_to_target(reference, value, false);
                Ok(())
            }
            (_, PatternMode::Declares(kind)) => {
                let name = Name(tree.node_a(target));
                let ident = if self.context.scope.is_program() {
                    self.declare_binding(name, kind)
                } else {
                    self.identifier_of(name)
                };
                self.store_declared(ident, value, kind);
                Ok(())
            }
        }
    }

    /// What `value` becomes if it is `undefined`, for a default in a pattern at `element`, which starts at `first_row`.
    fn apply_pattern_default(
        &mut self,
        default: NodeId,
        first_row: usize,
        value: Operand,
        name: Option<Name>,
    ) -> Result<(), OutOfStack> {
        let end_label = self.new_label();
        let next = self.new_label();
        self.reference_label(end_label);
        self.reference_label(next);
        self.put(op::JumpUndefined {
            condition: value,
            true_target: next,
            false_target: end_label,
        });
        self.bind_label(next);
        let name = name.map(|name| self.intern_identifier(name));
        self.set_pending_function_name(default, name);
        let first_temporary = self.next_temporary();
        let position = self.current_source_offset;
        let default_value = self.sweep_value(first_row, default)?;
        self.current_source_offset = position;
        self.context.stream.pending_function_name = None;
        self.move_value_to(value, default_value);
        self.release_temporaries_from(first_temporary);
        self.bind_label(end_label);
        Ok(())
    }

    /// Whatever anonymous function or class `value` is takes `name` as its name.
    fn set_pending_function_name(&mut self, value: NodeId, name: Option<IdentifierTableIndex>) {
        if is_anonymous_function_definition(self.tree, value) {
            self.context.stream.pending_function_name = name;
        }
    }

    /// Gives what an element of a pattern got to its target, after the default that it may have.
    fn bind_pattern_element(
        &mut self,
        element: NodeId,
        first_row: usize,
        value: Operand,
        mode: PatternMode,
    ) -> Result<(), OutOfStack> {
        let tree = self.tree;
        let (target, default) = if tree.node_tag(element) == tag::ASSIGN_PATTERN {
            (NodeId(tree.node_a(element)), Some(NodeId(tree.node_b(element))))
        } else {
            (element, None)
        };
        if let Some(default) = default {
            let target_tag = tree.node_tag(target);
            let is_name = target_tag == tag::IDENT || (tag::IDENT_MARKED..tag::MEMBER_MARKED).contains(&target_tag);
            let name = is_name.then(|| Name(tree.node_a(target)));
            self.apply_pattern_default(default, target.index() + 1, value, name)?;
        }
        self.bind_target(target, first_row, value, mode)
    }

    fn bind_object_pattern(
        &mut self,
        pattern: NodeId,
        first_row: usize,
        value: Operand,
        mode: PatternMode,
    ) -> Result<(), OutOfStack> {
        let tree = self.tree;
        let held = self.hold_registers();
        self.put(op::ThrowIfNullish { src: value });
        // NB: Every property comes from the value that the pattern started with, whatever gets written along the way.
        let object = if value.is_local() || value.is_argument() {
            let copy = self.held_register();
            self.put(op::Mov { dst: copy, src: value });
            copy
        } else {
            value
        };
        let items = tree.node_list(tree.node_a(pattern));
        let has_rest = !items.is_empty() && tree.node_tag(items.at(items.len() - 1)) == tag::REST;
        let mut excluded_names = Vec::new();
        let mut first_temporary = self.next_temporary();
        let mut row = first_row;
        for item in items.iter() {
            let item_tag = tree.node_tag(item);
            if item_tag == tag::REST {
                let target = NodeId(tree.node_a(item));
                self.prepare_pattern_target(target, row, mode)?;
                let copy = self.temporary();
                self.put(op::CopyObjectExcludingProperties {
                    dst: copy,
                    from_object: object,
                    excluded_names_count: u32_from_usize(excluded_names.len()),
                    excluded_names: &excluded_names,
                });
                self.bind_target(target, row, copy, mode)?;
                break;
            }
            let (element, element_first_row);
            let property_value;
            match item_tag {
                tag::SHORTHAND => {
                    let name = Name(tree.node_a(item));
                    self.prepare_pattern_target(NodeId(tree.node_b(item)), row, mode)?;
                    property_value = self.temporary();
                    self.put_get_by_id(property_value, object, name);
                    if has_rest {
                        let text = tree.name_text(name);
                        excluded_names.push(self.add_constant_string_view(text));
                    }
                    // NB: The row of the key is the identifier that gets bound, which has no rows of its own.
                    element = NodeId(tree.node_b(item));
                    element_first_row = row;
                }
                tag::COMPUTED_PROPERTY => {
                    let key = NodeId(tree.node_a(item));
                    let position = self.current_source_offset;
                    let key_value = self.sweep_value(row, key)?;
                    self.current_source_offset = position;
                    let converted = emit_to_primitive_property_key(self, &ScopedOperand::plain(key_value)).operand();
                    if has_rest {
                        excluded_names.push(converted);
                    }
                    first_temporary = self.next_temporary();
                    element = NodeId(tree.node_b(item));
                    // NB: A mark comes after a computed key.
                    element_first_row = key.index() + 2;
                    self.prepare_pattern_target(element, element_first_row, mode)?;
                    property_value = self.temporary();
                    self.put_get_by_value(property_value, object, converted);
                }
                _ => {
                    let key = NodeId(tree.node_a(item));
                    let text = tree.pattern_key_text(key);
                    element = NodeId(tree.node_b(item));
                    // NB: The mark of a BigInt key has nothing to go by here.
                    element_first_row = key.index() + 1 + usize::from(tree.node_tag(key) == tag::BIGINT);
                    self.prepare_pattern_target(element, element_first_row, mode)?;
                    property_value = self.temporary();
                    if tree.node_tag(key) == tag::IDENT_KEY {
                        self.put_get_by_id(property_value, object, Name(tree.node_a(key)));
                    } else {
                        let property_key = self.intern_property_key_text(text.view());
                        emit_get_by_key(
                            self,
                            &ScopedOperand::plain(property_value),
                            &ScopedOperand::plain(object),
                            property_key,
                            None,
                        );
                    }
                    if has_rest {
                        let name = match &text {
                            StringLiteralText::Source(view) => self.add_constant_string_view(*view),
                            StringLiteralText::Processed(value) => self.context.constants.add_string(value.clone()),
                        };
                        excluded_names.push(name);
                    }
                }
            }
            self.bind_pattern_element(element, element_first_row, property_value, mode)?;
            self.release_temporaries_from(first_temporary);
            row = item.index() + 1;
        }
        self.release_held_registers(held);
        Ok(())
    }

    fn bind_array_pattern(
        &mut self,
        pattern: NodeId,
        first_row: usize,
        value: Operand,
        mode: PatternMode,
    ) -> Result<(), OutOfStack> {
        let tree = self.tree;
        let held = self.hold_registers();
        let is_exhausted = self.held_register();
        let no = self.context.constants.add_boolean(false);
        self.put(op::Mov {
            dst: is_exhausted,
            src: no,
        });
        let object = self.held_register();
        let next = self.held_register();
        let done = self.held_register();
        let cache = self.next_property_lookup_cache();
        self.put(op::GetIterator {
            dst_iterator_object: object,
            dst_iterator_next: next,
            dst_iterator_done: done,
            iterable: value,
            hint: libjs_abi::IteratorHint::Sync as u32,
            fast_array_mode_allowed: true,
            cache,
        });

        let outer_handler = self.handler();
        let entry_environment = self.current_lexical_environment();
        let close_handler = Handler {
            label: self.new_label(),
            catches: false,
        };
        let iterator = OpenIterator {
            object,
            next,
            done,
            outer_handler,
            may_be_done: true,
            is_async: false,
        };
        self.push_control(Control::Iterator(iterator));
        self.set_handler(Some(close_handler));

        let items = tree.node_list(tree.node_a(pattern));
        let mut row = first_row;
        for (index, item) in items.iter().enumerate() {
            let first_temporary = self.next_temporary();
            let is_rest = tree.node_tag(item) == tag::REST;
            // NB: A rest element started out as a spread element, whose mark means nothing here.
            row += usize::from(tree.ast.tag[row] == tag::MARK_SPREAD_START);
            if tree.node_tag(item) == tag::HOLE {
                let after_step = self.new_label();
                if index != 0 {
                    self.jump_if(is_exhausted, after_step);
                }
                self.set_handler(outer_handler);
                self.put(op::IteratorStep {
                    dst_done: is_exhausted,
                    iterator_object: object,
                    iterator_next: next,
                    iterator_done: done,
                });
                self.set_handler(Some(close_handler));
                self.bind_label(after_step);
                row = item.index() + 1;
                continue;
            }
            let element = if is_rest { NodeId(tree.node_a(item)) } else { item };
            self.prepare_pattern_target(element, row, mode)?;
            let element_value = self.temporary();
            if is_rest {
                let exhausted = self.new_label();
                let end = self.new_label();
                if index != 0 {
                    self.jump_if(is_exhausted, exhausted);
                }
                self.set_handler(outer_handler);
                self.put(op::IteratorToArray {
                    dst: element_value,
                    iterator_object: object,
                    iterator_next_method: next,
                    iterator_done_property: done,
                });
                self.set_handler(Some(close_handler));
                if index != 0 {
                    self.jump(end);
                    self.bind_label(exhausted);
                    self.put(op::NewArray {
                        dst: element_value,
                        element_count: 0,
                        elements: &[],
                    });
                    self.bind_label(end);
                }
                self.bind_target(NodeId(tree.node_a(item)), row, element_value, mode)?;
                break;
            }
            let exhausted = self.new_label();
            let have_value = self.new_label();
            if index != 0 {
                self.jump_if(is_exhausted, exhausted);
            }
            self.set_handler(outer_handler);
            self.put(op::IteratorNextUnpack {
                dst_value: element_value,
                dst_done: is_exhausted,
                iterator_object: object,
                iterator_next: next,
                iterator_done: done,
            });
            self.set_handler(Some(close_handler));
            self.jump_unless(is_exhausted, have_value);
            self.bind_label(exhausted);
            let undefined = self.context.constants.add_undefined();
            self.put(op::Mov {
                dst: element_value,
                src: undefined,
            });
            self.reference_label(have_value);
            self.bind_label(have_value);
            self.bind_pattern_element(item, row, element_value, mode)?;
            self.release_temporaries_from(first_temporary);
            row = item.index() + 1;
        }

        self.pop_control();
        self.set_handler(outer_handler);
        let end_label = self.new_label();
        self.jump_if(done, end_label);
        let undefined = self.context.constants.add_undefined();
        let cache = self.next_property_lookup_cache();
        self.put(op::IteratorClose {
            iterator_object: object,
            iterator_next: next,
            iterator_done: done,
            completion_type: libjs_abi::CompletionType::Normal as u32,
            completion_value: undefined,
            cache,
        });
        self.jump(end_label);
        self.bind_reachable_label(close_handler.label);
        self.close_iterator_after_exception(iterator, &entry_environment);
        self.bind_label(end_label);
        self.release_held_registers(held);
        Ok(())
    }
}

/// The literal that an initializer of a field consists of, if it does.
#[derive(Default)]
pub(super) struct LiteralInitializer {
    pub(super) kind: Option<PendingLiteralValueKind>,
    pub(super) number: f64,
    pub(super) string: Option<ak::Utf16String>,
}

pub(super) fn literal_initializer(generator: &Generator<'_>, initializer: NodeId) -> LiteralInitializer {
    let tree = generator.tree;
    let of_kind = |kind| LiteralInitializer {
        kind: Some(kind),
        ..LiteralInitializer::default()
    };
    match tree.node_tag(initializer) {
        tag::NUM => LiteralInitializer {
            kind: Some(PendingLiteralValueKind::Number),
            number: tree.number_value(initializer),
            string: None,
        },
        tag::TRUE => of_kind(PendingLiteralValueKind::BooleanTrue),
        tag::FALSE => of_kind(PendingLiteralValueKind::BooleanFalse),
        tag::NULL => of_kind(PendingLiteralValueKind::Null),
        tag::STR => LiteralInitializer {
            kind: Some(PendingLiteralValueKind::String),
            number: 0.0,
            string: Some(tree.string_literal_value(initializer)),
        },
        tag::UNARY if tree.unary_operator(initializer) == UnaryOp::Minus => {
            let operand = tree.expression_a(initializer);
            if tree.node_tag(operand) == tag::NUM {
                LiteralInitializer {
                    kind: Some(PendingLiteralValueKind::Number),
                    number: -tree.number_value(operand),
                    string: None,
                }
            } else {
                LiteralInitializer::default()
            }
        }
        _ => LiteralInitializer::default(),
    }
}

/// Whether a parameter has anything that gets evaluated in it.
fn parameter_has_expression(tree: &SyntaxTree, parameter: NodeId) -> bool {
    let target = match tree.node_tag(parameter) {
        tag::ASSIGN_PATTERN => return true,
        tag::REST => NodeId(tree.node_a(parameter)),
        _ => parameter,
    };
    matches!(tree.node_tag(target), tag::ARRAY_PATTERN | tag::OBJECT_PATTERN)
        && tree.pattern_contains_expression(target.0)
}

/// Calls `callback` with each of the BoundNames of `target`, an identifier or a binding pattern, in the order of the
/// source.
fn for_each_bound_name(tree: &SyntaxTree, target: NodeId, callback: &mut impl FnMut(Name)) {
    match tree.node_tag(target) {
        tag::ARRAY_PATTERN | tag::OBJECT_PATTERN => {
            for item in tree.node_list(tree.node_a(target)).iter() {
                match tree.node_tag(item) {
                    tag::HOLE => {}
                    tag::PROPERTY | tag::COMPUTED_PROPERTY => {
                        for_each_bound_name(tree, NodeId(tree.node_b(item)), callback);
                    }
                    tag::SHORTHAND => callback(Name(tree.node_a(item))),
                    _ => for_each_bound_name(tree, item, callback),
                }
            }
        }
        tag::REST | tag::ASSIGN_PATTERN => for_each_bound_name(tree, NodeId(tree.node_a(target)), callback),
        _ => callback(Name(tree.node_a(target))),
    }
}

/// What a function has for parameters.
#[inline(always)]
fn parameters_of(tree: &SyntaxTree, function: NodeId) -> NodeList<'_> {
    tree.node_list(tree.extra_at(tree.node_a(function)))
}
