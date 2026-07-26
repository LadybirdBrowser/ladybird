/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Flap abstract syntax tree.

use super::diagnostic::SourceSpan;
pub(crate) use crate::types::ParameterMode;
use crate::types::{BlockTemperature, Type};

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct Parameter {
    pub(crate) name: String,
    pub(crate) mode: ParameterMode,
    pub(crate) ty: Type,
    pub(crate) is_else: bool,
    pub(crate) span: SourceSpan,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct Program {
    pub(crate) declarations: Vec<Declaration>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum Declaration {
    InlineFunction(InlineFunctionDeclaration),
    Handler(HandlerDeclaration),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct InlineFunctionDeclaration {
    pub(crate) name: String,
    pub(crate) parameters: Vec<Parameter>,
    pub(crate) return_type: Option<Type>,
    pub(crate) body: Block,
    pub(crate) span: SourceSpan,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct HandlerDeclaration {
    pub(crate) name: String,
    pub(crate) parameters: Vec<Parameter>,
    pub(crate) temperature: BlockTemperature,
    pub(crate) body: Block,
    pub(crate) span: SourceSpan,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct Block {
    pub(crate) statements: Vec<Statement>,
    pub(crate) value: Option<Expression>,
    pub(crate) span: SourceSpan,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct Expression {
    pub(crate) kind: ExpressionKind,
    pub(crate) span: SourceSpan,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum UnaryOperator {
    Negate,
    LogicalNot,
    BitwiseNot,
    Reference,
    ValueTypeTest { type_name: String, negated: bool },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum BinaryOperator {
    Add,
    Subtract,
    Multiply,
    Divide,
    Modulo,
    BitwiseAnd,
    BitwiseXor,
    BitwiseOr,
    BitTest { set: bool },
    LeftShift,
    RightShift,
    UnsignedRightShift,
    LessThan,
    GreaterThan,
    LessThanOrEqual,
    GreaterThanOrEqual,
    LooseEqual,
    LooseNotEqual,
    Identity { negated: bool },
    LogicalAnd,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum ExpressionKind {
    Name(String),
    Integer(i64),
    Call {
        callee: String,
        arguments: Vec<Expression>,
    },
    Unary {
        operator: UnaryOperator,
        operand: Box<Expression>,
    },
    Binary {
        operator: BinaryOperator,
        lhs: Box<Expression>,
        rhs: Box<Expression>,
    },
    Index {
        base: Box<Expression>,
        index: Box<Expression>,
    },
    Memory(Vec<Expression>),
    Tuple(Vec<Expression>),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct Statement {
    pub(crate) kind: StatementKind,
    pub(crate) span: SourceSpan,
}

impl Statement {
    pub(crate) fn new(kind: StatementKind, span: SourceSpan) -> Self {
        Self { kind, span }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum Pattern {
    Binding {
        name: String,
        ty: Option<Type>,
    },
    Tuple(Vec<Pattern>),
    Wildcard,
    Expression(Expression),
    ValueRepresentation {
        representation: String,
        binding: Option<Box<Pattern>>,
    },
    Alternatives(Vec<Pattern>),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum ElseContinuation {
    Label(String),
    Invocation { target: String, arguments: Vec<Expression> },
    Block { temperature: BlockTemperature, body: Block },
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum StatementKind {
    Let {
        pattern: Pattern,
        initializer: Option<Expression>,
    },
    GuardLet {
        pattern: Pattern,
        initializer: Expression,
        failure: ElseContinuation,
    },
    FallibleCall {
        callee: String,
        arguments: Vec<Expression>,
        failure: ElseContinuation,
    },
    Assign {
        name: String,
        initializer: Expression,
    },
    IndexAssign {
        base: Expression,
        index: Expression,
        value: Expression,
    },
    Expression(Expression),
    ValueMatch {
        name: Option<String>,
        value: Expression,
        arms: Vec<ValueMatchArm>,
        fallback: ValueMatchFallback,
    },
    ScalarMatch {
        value: Expression,
        arms: Vec<ScalarMatchArm>,
        fallback: Block,
    },
    Guard {
        condition: Expression,
        failure: ElseContinuation,
    },
    Continuation {
        name: String,
        temperature: BlockTemperature,
        parameters: Vec<Parameter>,
        body: Block,
    },
    ContinuationJump {
        target: String,
        arguments: Vec<Expression>,
    },
    If {
        name: Option<String>,
        ty: Option<Type>,
        condition: Expression,
        temperature: BlockTemperature,
        then_body: Block,
        else_body: Option<Block>,
    },
    While {
        condition: Expression,
        body: Block,
        post_tested: bool,
    },
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct ScalarMatchArm {
    pub(crate) pattern: Pattern,
    pub(crate) temperature: BlockTemperature,
    pub(crate) body: Block,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct ValueMatchArm {
    pub(crate) representation: String,
    pub(crate) binding: Option<String>,
    pub(crate) temperature: BlockTemperature,
    pub(crate) body: Block,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct ValueMatchFallback {
    pub(crate) body: Block,
    pub(crate) temperature: BlockTemperature,
}
