/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Inline accessors over the columns of the syntax tree, for code generation. A node is only ever looked at through
//! what a generating function needs of it right then: nothing here builds a view of a whole node, and the small structs
//! are the fields of one tag, which are gone again once inlined.

use crate::ast::AssignmentOp;
use crate::ast::BinaryOp;
use crate::ast::ClassMethodKind;
use crate::ast::DeclarationKind;
use crate::ast::LocalType;
use crate::ast::LogicalOp;
use crate::ast::Name;
use crate::ast::SyntaxTree;
use crate::ast::UnaryOp;
use crate::ast::UpdateOp;
use crate::parser::columns::NONE;
use crate::parser::columns::tag;
use crate::parser::literals;
pub(super) use crate::parser::literals::EscapeMode;
use crate::parser::token::T;
use crate::utf16::Utf16View;
use ak::Utf16String;

/// A node of the tree. Zero stands for no node.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct NodeId(pub(super) u32);

impl NodeId {
    #[inline(always)]
    pub(super) fn index(self) -> usize {
        self.0 as usize
    }
}

/// The nodes of a list of the tree.
#[derive(Clone, Copy)]
pub(super) struct NodeList<'t>(&'t [u32]);

impl<'t> NodeList<'t> {
    #[inline(always)]
    pub(super) fn len(self) -> usize {
        self.0.len()
    }

    #[inline(always)]
    pub(super) fn is_empty(self) -> bool {
        self.0.is_empty()
    }

    #[inline(always)]
    pub(super) fn at(self, index: usize) -> NodeId {
        NodeId(self.0[index])
    }

    #[inline(always)]
    pub(super) fn iter(self) -> impl DoubleEndedIterator<Item = NodeId> + ExactSizeIterator + 't {
        self.0.iter().map(|&node| NodeId(node))
    }
}

/// An identifier with what scope analysis found out about it.
#[derive(Clone, Copy)]
pub(super) struct Identifier {
    pub(super) name: Name,
    pub(super) local_type: Option<LocalType>,
    pub(super) local_index: u32,
    pub(super) is_global: bool,
    pub(super) declaration_kind: Option<DeclarationKind>,
}

impl Identifier {
    #[inline(always)]
    pub(super) fn is_local(&self) -> bool {
        self.local_type.is_some()
    }
}

/// The parts of a class node.
#[derive(Clone, Copy)]
pub(super) struct ClassData<'t> {
    pub(super) name: Option<Name>,
    pub(super) super_class: Option<NodeId>,
    /// The constructor that the class spells out, as a function node.
    pub(super) constructor: Option<NodeId>,
    /// Every element of the class, including the constructor that `constructor_index` tells apart.
    pub(super) elements: NodeList<'t>,
    pub(super) constructor_index: Option<usize>,
    pub(super) source_text_start: u32,
    pub(super) source_text_end: u32,
}

impl ClassData<'_> {
    /// The elements of the class, without its constructor.
    #[inline(always)]
    pub(super) fn elements_without_constructor(&self) -> impl Iterator<Item = NodeId> + '_ {
        self.elements
            .iter()
            .enumerate()
            .filter(|(index, _)| Some(*index) != self.constructor_index)
            .map(|(_, element)| element)
    }
}

/// The key of a class element.
#[derive(Clone, Copy)]
pub(super) enum ClassElementKey {
    /// A name that stands for itself as a string.
    Name(Name),
    /// Anything else: a literal, a private name or a computed key.
    Expression(NodeId),
}

/// A class element, with `static` taken off.
#[derive(Clone, Copy)]
pub(super) enum ClassElementData {
    Method {
        key: ClassElementKey,
        function: NodeId,
        kind: ClassMethodKind,
        is_static: bool,
    },
    Field {
        key: ClassElementKey,
        initializer: Option<NodeId>,
        /// The field itself, which the function that evaluates `initializer` goes by.
        field: NodeId,
        is_static: bool,
    },
    StaticInitializer {
        block: NodeId,
    },
}

#[derive(Clone, Copy)]
pub(super) struct SwitchData<'t> {
    pub(super) cases: NodeList<'t>,
}

/// The parts of a try statement.
#[derive(Clone, Copy)]
pub(super) struct TryData {
    pub(super) block: NodeId,
    pub(super) parameter: Option<NodeId>,
    /// The body of the catch clause, if there is one.
    pub(super) handler: Option<NodeId>,
    pub(super) finalizer: Option<NodeId>,
}

/// The text of a string literal: a view of the source if it has no escape sequences, which most do not.
pub(super) enum StringLiteralText<'t> {
    Source(Utf16View<'t>),
    Processed(Utf16String),
}

impl StringLiteralText<'_> {
    #[inline(always)]
    pub(super) fn view(&self) -> Utf16View<'_> {
        match self {
            StringLiteralText::Source(view) => *view,
            StringLiteralText::Processed(string) => Utf16View::of_string(string),
        }
    }
}

impl SyntaxTree {
    #[inline(always)]
    pub(super) fn node_tag(&self, node: NodeId) -> u8 {
        self.ast.tag[node.index()]
    }

    #[inline(always)]
    pub(super) fn node_pos(&self, node: NodeId) -> u32 {
        self.ast.pos[node.index()]
    }

    #[inline(always)]
    pub(super) fn node_a(&self, node: NodeId) -> u32 {
        self.ast.a[node.index()]
    }

    #[inline(always)]
    pub(super) fn node_b(&self, node: NodeId) -> u32 {
        self.ast.b[node.index()]
    }

    #[inline(always)]
    pub(super) fn extra_at(&self, index: u32) -> u32 {
        self.ast.extra[index as usize]
    }

    #[inline(always)]
    pub(super) fn node_list(&self, start: u32) -> NodeList<'_> {
        let start = start as usize;
        let count = self.ast.extra[start] as usize;
        NodeList(&self.ast.extra[start + 1..start + 1 + count])
    }

    /// The expression at `node`, which a pair of parentheses leaves as it is.
    #[inline(always)]
    pub(super) fn skip_parens(&self, mut node: NodeId) -> NodeId {
        while self.node_tag(node) == tag::PAREN {
            node = NodeId(self.node_a(node));
        }
        node
    }

    /// The child in the `a` operand of `node`, as an expression.
    #[inline(always)]
    pub(super) fn expression_a(&self, node: NodeId) -> NodeId {
        self.skip_parens(NodeId(self.node_a(node)))
    }

    /// The value of a numeric literal node.
    #[inline(always)]
    pub(super) fn number_value(&self, node: NodeId) -> f64 {
        self.number_at(self.node_pos(node), self.node_a(node))
    }

    /// The value of the numeric literal between two offsets of the source.
    #[inline(always)]
    pub(super) fn number_at(&self, start: u32, end: u32) -> f64 {
        let text = self.source_range_units(start, end);
        // OPTIMIZATION: Nearly all numbers in code are short decimal integers, which an f64 holds exactly.
        if let Utf16View::Ascii(digits) | Utf16View::Latin1(digits) = text
            && digits.len() <= 15
            && (digits.len() == 1 || digits[0] != b'0')
            && digits.iter().all(u8::is_ascii_digit)
        {
            let value = digits
                .iter()
                .fold(0u64, |value, digit| value * 10 + u64::from(digit - b'0'));
            return value as f64;
        }
        literals::parse_numeric_value(text)
    }

    /// The text between two offsets of the source.
    #[inline(always)]
    pub(super) fn source_range_units(&self, start: u32, end: u32) -> Utf16View<'_> {
        Utf16View::of_string(&self.script.positions.source).substring_view(start as usize, (end - start) as usize)
    }

    /// The text between the quotes of a string literal node.
    #[inline(always)]
    pub(super) fn string_literal_units(&self, node: NodeId) -> Utf16View<'_> {
        self.source_range_units(self.node_pos(node) + 1, self.node_a(node) - 1)
    }

    /// The value of a string literal node, without a copy if that is possible.
    #[inline(always)]
    pub(super) fn string_literal_text(&self, node: NodeId) -> StringLiteralText<'_> {
        let units = self.string_literal_units(node);
        if has_escape_sequence(units) {
            StringLiteralText::Processed(self.string_literal_value(node))
        } else {
            StringLiteralText::Source(units)
        }
    }

    /// The value of a string literal node.
    pub(super) fn string_literal_value(&self, node: NodeId) -> Utf16String {
        literals::process_escape_sequences_impl(self.string_literal_units(node), EscapeMode::StringLiteral).value
    }

    /// The text of a template chunk node, as the source has it.
    #[inline(always)]
    pub(super) fn template_chunk_units(&self, chunk: NodeId) -> Utf16View<'_> {
        let start = self.node_a(chunk);
        self.source_range_units(start, start + (self.node_b(chunk) >> 1))
    }

    /// The value of a template chunk node, or `None` if an escape sequence in it makes it have none.
    #[inline(always)]
    pub(super) fn template_chunk_text(&self, chunk: NodeId, mode: EscapeMode) -> Option<StringLiteralText<'_>> {
        let units = self.template_chunk_units(chunk);
        // NB: A template normalizes the line terminators in it as well.
        if !has_escape_sequence(units) && !units.code_units().any(|unit| unit == u16::from(b'\r')) {
            return Some(StringLiteralText::Source(units));
        }
        let result = literals::process_escape_sequences_impl(units, mode);
        (!result.failed || mode != EscapeMode::TaggedTemplate).then_some(StringLiteralText::Processed(result.value))
    }

    /// Whether the element of a template is a string with nothing in it, which leaves no trace in the result.
    #[inline(always)]
    pub(super) fn is_empty_template_element(&self, element: NodeId) -> bool {
        if self.node_tag(element) == tag::TEMPLATE_CHUNK {
            return match self.template_chunk_text(element, EscapeMode::StringLiteral) {
                Some(text) => text.view().length_in_code_units() == 0,
                None => false,
            };
        }
        let element = self.skip_parens(element);
        self.node_tag(element) == tag::STR && self.string_literal_text(element).view().length_in_code_units() == 0
    }

    /// The operator of a binary or logical expression tag.
    #[inline(always)]
    pub(super) fn binary_operator(node_tag: u8) -> BinaryOrLogical {
        let operator = node_tag - tag::BINARY_BASE;
        match crate::ast::BINARY_OPERATORS[operator as usize] {
            Some(op) => BinaryOrLogical::Binary(op),
            None => BinaryOrLogical::Logical(match operator {
                0 => LogicalOp::NullishCoalescing,
                1 => LogicalOp::Or,
                _ => LogicalOp::And,
            }),
        }
    }

    /// The operator of an assignment tag. A default of a pattern in expression position is a plain assignment.
    #[inline(always)]
    pub(super) fn assignment_operator(node_tag: u8) -> AssignmentOp {
        if node_tag == tag::ASSIGN_PATTERN {
            return AssignmentOp::Assignment;
        }
        crate::ast::ASSIGNMENT_OPERATORS[(node_tag - tag::ASSIGN_BASE) as usize]
    }

    #[inline(always)]
    pub(super) fn unary_operator(&self, node: NodeId) -> UnaryOp {
        match self.node_b(node) {
            token if token == T::Not as u32 => UnaryOp::Not,
            token if token == T::Tilde as u32 => UnaryOp::BitwiseNot,
            token if token == T::Plus as u32 => UnaryOp::Plus,
            token if token == T::Minus as u32 => UnaryOp::Minus,
            token if token == T::KTypeof as u32 => UnaryOp::Typeof,
            token if token == T::KVoid as u32 => UnaryOp::Void,
            _ => UnaryOp::Delete,
        }
    }

    #[inline(always)]
    pub(super) fn update_operator(&self, node: NodeId) -> UpdateOp {
        if self.node_b(node) == T::Inc as u32 {
            UpdateOp::Increment
        } else {
            UpdateOp::Decrement
        }
    }

    /// The cases of a switch statement.
    #[inline(always)]
    pub(super) fn switch_data(&self, node: NodeId) -> SwitchData<'_> {
        SwitchData {
            cases: self.node_list(self.extra_at(self.node_b(node))),
        }
    }

    /// The text of the key of a property of a pattern, which refers to no binding.
    pub(super) fn pattern_key_text(&self, key: NodeId) -> StringLiteralText<'_> {
        match self.node_tag(key) {
            tag::IDENT | tag::IDENT_KEY => StringLiteralText::Source(self.name_text(Name(self.node_a(key)))),
            tag::STR | tag::STR_KEY => self.string_literal_text(key),
            tag::NUM | tag::NUM_KEY => StringLiteralText::Processed(crate::host::js_number_to_utf16(
                literals::parse_numeric_value(self.source_range_units(self.node_pos(key), self.node_a(key))),
            )),
            _ => {
                let literal = self.source_range_units(self.node_pos(key), self.node_a(key)).to_utf8();
                let value = crate::bytecode::constant_fold::parse_bigint(literal.trim_end_matches('n'))
                    .expect("the lexer only accepts valid BigInts");
                StringLiteralText::Processed(Utf16String::from_utf8(&value.to_string()))
            }
        }
    }

    /// The parts of a class node.
    pub(super) fn class_data(&self, class: NodeId) -> ClassData<'_> {
        let name = self.node_a(class);
        let extra = self.node_b(class);
        let super_class = self.extra_at(extra);
        let constructor = self.extra_at(extra + 3);
        let elements = self.node_list(self.extra_at(extra + 1));
        let constructor_index = elements.iter().position(|element| {
            constructor != 0 && self.node_tag(element) == tag::METHOD && self.node_b(element) == constructor
        });
        ClassData {
            name: (name != NONE).then_some(Name(name)),
            super_class: (super_class != 0).then(|| self.skip_parens(NodeId(super_class))),
            constructor: constructor_index.map(|_| NodeId(constructor)),
            elements,
            constructor_index,
            source_text_start: self.node_pos(class),
            source_text_end: self.extra_at(extra + 4),
        }
    }

    #[inline(always)]
    fn class_element_key(&self, key: NodeId, computed: bool) -> ClassElementKey {
        if !computed && matches!(self.node_tag(key), tag::IDENT | tag::IDENT_KEY) {
            ClassElementKey::Name(Name(self.node_a(key)))
        } else {
            ClassElementKey::Expression(self.skip_parens(key))
        }
    }

    /// What the class element at `element` is.
    pub(super) fn class_element(&self, element: NodeId) -> ClassElementData {
        let is_static = self.node_tag(element) == tag::STATIC_ELEMENT;
        let inner = if is_static {
            NodeId(self.node_a(element))
        } else {
            element
        };
        let (a, b) = (NodeId(self.node_a(inner)), self.node_b(inner));
        let inner_tag = self.node_tag(inner);
        match inner_tag {
            tag::CLASS_FIELD | tag::COMPUTED_CLASS_FIELD => ClassElementData::Field {
                key: self.class_element_key(a, inner_tag == tag::COMPUTED_CLASS_FIELD),
                initializer: (b != 0).then(|| self.expression_a(NodeId(b))),
                field: inner,
                is_static,
            },
            tag::CLASS_STATIC_BLOCK => ClassElementData::StaticInitializer { block: inner },
            other => ClassElementData::Method {
                key: self.class_element_key(
                    a,
                    matches!(
                        other,
                        tag::COMPUTED_METHOD | tag::COMPUTED_GETTER | tag::COMPUTED_SETTER
                    ),
                ),
                function: self.skip_parens(NodeId(b)),
                kind: match other {
                    tag::GETTER | tag::COMPUTED_GETTER => ClassMethodKind::Getter,
                    tag::SETTER | tag::COMPUTED_SETTER => ClassMethodKind::Setter,
                    _ => ClassMethodKind::Method,
                },
                is_static,
            },
        }
    }

    /// The one child of a node that holds a statement as its `a` operand, as a list.
    #[inline(always)]
    pub(super) fn try_data(&self, node: NodeId) -> TryData {
        let extra = self.node_b(node);
        let optional = |index: u32| {
            let node = self.extra_at(index);
            (node != 0).then_some(NodeId(node))
        };
        TryData {
            block: NodeId(self.node_a(node)),
            parameter: optional(extra),
            handler: optional(extra + 1),
            finalizer: optional(extra + 2),
        }
    }
}

/// Whether the text of a string literal or a template has an escape sequence in it.
#[inline(always)]
fn has_escape_sequence(units: Utf16View<'_>) -> bool {
    match units {
        Utf16View::Ascii(units) | Utf16View::Latin1(units) => units.contains(&b'\\'),
        Utf16View::Utf16(units) => units.contains(&u16::from(b'\\')),
    }
}

#[derive(Clone, Copy)]
pub(super) enum BinaryOrLogical {
    Binary(BinaryOp),
    Logical(LogicalOp),
}
