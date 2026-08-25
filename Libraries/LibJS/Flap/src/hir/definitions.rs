/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Typed high-level intermediate representation definitions.

use crate::frontend::ast::ParameterMode;
use crate::frontend::diagnostic::SourceSpan;
use crate::frontend::layout::LayoutValue;
use crate::hash::HashSet;
use crate::identity::{ExternalSymbol, InlineFunctionId};
use crate::intrinsic::{ComparisonRelation, FieldAccess, Intrinsic, OperationValue, ValueOperation};
use crate::types::{BlockTemperature, InterpreterRegister, RegisterReference, Type};
use std::fmt;

pub(crate) type VariableId = usize;

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct Value {
    pub(crate) kind: ValueKind,
    pub(crate) ty: Type,
    pub(crate) span: SourceSpan,
}

impl Value {
    pub(crate) fn new(kind: ValueKind, ty: Type, span: SourceSpan) -> Self {
        Self { kind, ty, span }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum ValueKind {
    Variable(VariableId),
    Integer(i64),
    LayoutValue(LayoutValue),
    Constant(String),
    Label(String),
    SlowPath(ExternalSymbol),
    FunctionSymbol(ExternalSymbol),
    Operation(OperationValue),
    MachineRegister(RegisterReference),
    Memory(Vec<Value>),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct Variable {
    pub(crate) name: String,
    pub(crate) ty: Type,
    pub(crate) parameter_mode: Option<ParameterMode>,
    pub(crate) mutable: bool,
    pub(crate) span: SourceSpan,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum CallTarget {
    Intrinsic(Intrinsic),
    InlineFunction(InlineFunctionId),
    OperationParameter(VariableId),
    FieldAccess(FieldAccess),
}

impl fmt::Display for CallTarget {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Intrinsic(intrinsic) => formatter.write_str(intrinsic.name()),
            Self::InlineFunction(id) => write!(formatter, "inline function #{}", id.index()),
            Self::OperationParameter(_) => formatter.write_str("operation parameter"),
            Self::FieldAccess(_) => formatter.write_str("field access"),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct Call {
    pub(crate) target: CallTarget,
    pub(crate) arguments: Vec<Value>,
    pub(crate) effects: Vec<ParameterMode>,
    pub(crate) return_type: Option<Type>,
    pub(crate) terminal: bool,
    pub(crate) branch_target: Option<String>,
    pub(crate) failure: Option<CallFailure>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct CallFailure {
    pub(crate) captures: Vec<VariableId>,
    pub(crate) body: Vec<Statement>,
}

impl Call {
    pub(crate) fn new(
        target: CallTarget,
        arguments: Vec<Value>,
        effects: Vec<ParameterMode>,
        return_type: Option<Type>,
    ) -> Self {
        Self {
            target,
            arguments,
            effects,
            return_type,
            terminal: false,
            branch_target: None,
            failure: None,
        }
    }

    pub(crate) fn with_control_flow(
        target: CallTarget,
        arguments: Vec<Value>,
        effects: Vec<ParameterMode>,
        return_type: Option<Type>,
        terminal: bool,
        branch_target: Option<String>,
    ) -> Self {
        Self {
            target,
            arguments,
            effects,
            return_type,
            terminal,
            branch_target,
            failure: None,
        }
    }

    pub(crate) fn display_name(&self) -> &CallTarget {
        &self.target
    }

    pub(crate) fn intrinsic(&self) -> Option<Intrinsic> {
        let CallTarget::Intrinsic(intrinsic) = self.target else {
            return None;
        };
        Some(intrinsic)
    }

    pub(crate) fn field_access(&self) -> Option<FieldAccess> {
        let CallTarget::FieldAccess(field_access) = self.target else {
            return None;
        };
        Some(field_access)
    }

    pub(crate) fn is_load(&self) -> bool {
        match self.target {
            CallTarget::Intrinsic(Intrinsic::Bytecode(_)) => true,
            CallTarget::Intrinsic(Intrinsic::LowLevel(
                crate::intrinsic::LowLevelOperation::LoadLabel | crate::intrinsic::LowLevelOperation::LoadVm,
            )) => true,
            CallTarget::Intrinsic(Intrinsic::Memory(operation)) => !operation.writes(),
            CallTarget::Intrinsic(Intrinsic::Operand(
                crate::intrinsic::OperandOperation::Load(_) | crate::intrinsic::OperandOperation::LoadPair,
            )) => true,
            CallTarget::FieldAccess(access) => !access.kind.writes(),
            _ => false,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum Condition {
    LogicalNot(Box<Condition>),
    ScalarEquality {
        lhs: Value,
        rhs: Value,
        equal: bool,
    },
    ScalarRelational {
        lhs: Value,
        rhs: Value,
        relation: ComparisonRelation,
        signed: bool,
    },
    BitTest {
        value: Value,
        mask: Value,
        set: bool,
    },
    ValueTag {
        value: Value,
        tag: VariableId,
        source_tag: Option<VariableId>,
        mask: Option<TagMask>,
        expected_tag: String,
        matches_when_equal: bool,
        matches: bool,
    },
    ValueBits {
        value: Value,
        expected: VariableId,
        expected_bits: String,
        matches: bool,
    },
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum TagMask {
    Immediate(i64),
    Constant(String),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct Statement {
    pub(crate) kind: StatementKindIr,
    pub(crate) span: SourceSpan,
}

impl Statement {
    pub(crate) fn new(kind: StatementKindIr, span: SourceSpan) -> Self {
        Self { kind, span }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum ValueMatchArmKind {
    Int32,
    Double,
    Boolean,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct ValueMatchArm {
    pub(crate) kind: ValueMatchArmKind,
    pub(crate) binding: Option<VariableId>,
    pub(crate) layout: BlockTemperature,
    pub(crate) body: Vec<Statement>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct ScalarMatchArmIr {
    pub(crate) pattern: Vec<Value>,
    pub(crate) layout: BlockTemperature,
    pub(crate) body: Vec<Statement>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum StatementKindIr {
    Call(Vec<VariableId>, Call),
    MachineAssign {
        destination: InterpreterRegister,
        intrinsic: Intrinsic,
        arguments: Vec<Value>,
    },
    ValueRefinement {
        binding: VariableId,
        value: Value,
        tag: VariableId,
        expected_tag: Option<String>,
        payload_operation: ValueOperation,
        failure: String,
    },
    ValueMatch {
        destination: Option<VariableId>,
        value: Value,
        tag: Option<VariableId>,
        captures: Vec<VariableId>,
        arms: Vec<ValueMatchArm>,
        fallback: ValueMatchFallbackIr,
    },
    ScalarMatch {
        value: Value,
        captures: Vec<VariableId>,
        arms: Vec<ScalarMatchArmIr>,
        fallback: Vec<Statement>,
    },
    Guard {
        condition: Condition,
        failure: String,
    },
    Continuation {
        name: String,
        temperature: BlockTemperature,
        parameters: Vec<VariableId>,
        captures: Vec<VariableId>,
        body: Vec<Statement>,
    },
    If {
        destination: Option<VariableId>,
        condition: Condition,
        temperature: BlockTemperature,
        terminal: bool,
        captures: Vec<VariableId>,
        then_body: Vec<Statement>,
        else_body: Option<Vec<Statement>>,
    },
    While {
        condition_setup: Vec<Statement>,
        condition: Condition,
        captures: Vec<VariableId>,
        body: Vec<Statement>,
        post_tested: bool,
    },
}

impl StatementKindIr {
    pub(crate) fn call(destinations: impl IntoIterator<Item = VariableId>, call: Call) -> Self {
        Self::Call(destinations.into_iter().collect(), call)
    }
}

macro_rules! visit_nested_bodies {
    ($statement:expr, $visit:expr) => {
        match $statement {
            Self::Call(_, call) => {
                if let Some(failure) = &call.failure {
                    ($visit)(&failure.body);
                }
            }
            Self::If {
                then_body, else_body, ..
            } => {
                ($visit)(then_body);
                if let Some(else_body) = else_body {
                    ($visit)(else_body);
                }
            }
            Self::ValueMatch { arms, fallback, .. } => {
                for arm in arms {
                    ($visit)(&arm.body);
                }
                ($visit)(&fallback.body);
            }
            Self::ScalarMatch { arms, fallback, .. } => {
                for arm in arms {
                    ($visit)(&arm.body);
                }
                ($visit)(fallback);
            }
            Self::Continuation { body, .. } => {
                ($visit)(body);
            }
            Self::While {
                condition_setup, body, ..
            } => {
                ($visit)(condition_setup);
                ($visit)(body);
            }
            _ => {}
        }
    };
}

impl StatementKindIr {
    pub(crate) fn for_each_nested_body(&self, mut visit: impl FnMut(&[Statement])) {
        visit_nested_bodies!(self, visit);
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct ValueMatchFallbackIr {
    pub(crate) body: Vec<Statement>,
    pub(crate) temperature: BlockTemperature,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct Body {
    pub(crate) variables: Vec<Variable>,
    pub(crate) statements: Vec<Statement>,
    pub(crate) bytecode_fields: HashSet<VariableId>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct InlineFunction {
    pub(crate) id: InlineFunctionId,
    pub(crate) name: String,
    pub(crate) span: SourceSpan,
    pub(crate) parameters: Vec<VariableId>,
    pub(crate) return_type: Option<Type>,
    pub(crate) body: Body,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct Handler {
    pub(crate) name: String,
    pub(crate) parameters: Vec<VariableId>,
    pub(crate) temperature: BlockTemperature,
    pub(crate) body: Body,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct Program {
    pub(crate) inline_functions: Vec<InlineFunction>,
    pub(crate) handlers: Vec<Handler>,
}
