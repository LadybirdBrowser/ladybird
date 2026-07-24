/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Semantic definitions for built-in Flap operations.

use crate::frontend::ast::ParameterMode;
use crate::intrinsic::{
    BranchOperation, CheckedIntegerOperation, FloatingPointOperation, IntegerSignedness,
    Intrinsic, IntrinsicCallSignature, IntrinsicResultType, IntrinsicValueType,
};
use crate::types::Type;

#[derive(Clone, PartialEq, Eq)]
pub(super) struct Signature {
    pub(super) parameters: Vec<(ParameterMode, Type)>,
    pub(super) has_else_parameter: bool,
    pub(super) return_type: Option<Type>,
    pub(super) terminal: bool,
}

pub(super) struct ResolvedIntrinsic {
    pub(super) intrinsic: Intrinsic,
    pub(super) signature: Signature,
}

pub(super) fn signature(parameters: &[(ParameterMode, Type)], return_type: Option<Type>) -> Signature {
    Signature {
        parameters: parameters.to_vec(),
        has_else_parameter: false,
        return_type,
        terminal: false,
    }
}

fn signature_with_else(parameters: &[(ParameterMode, Type)], return_type: Option<Type>) -> Signature {
    let mut parameters = parameters.to_vec();
    parameters.push((ParameterMode::In, Type::label()));
    Signature {
        parameters,
        has_else_parameter: true,
        return_type,
        terminal: false,
    }
}

pub(super) fn operation_signature(ty: &Type) -> Option<Signature> {
    use ParameterMode::{In, InOut};
    match ty {
        Type::BinaryOperation(inner) => Some(signature(&[(InOut, *inner.clone()), (In, *inner.clone())], None)),
        Type::CheckedBinaryOperation(inner) => Some(signature_with_else(
            &[(InOut, *inner.clone()), (In, *inner.clone())],
            None,
        )),
        Type::IntegerCondition => Some(signature(
            &[(In, Type::I32), (In, Type::I32), (In, Type::label())],
            None,
        )),
        Type::FloatCondition => Some(signature(
            &[(In, Type::F64), (In, Type::F64), (In, Type::label())],
            None,
        )),
        Type::EqualityOperation => Some(signature_with_else(
            &[
                (In, Type::Value),
                (In, Type::Value),
                (In, Type::label()),
                (In, Type::label()),
            ],
            None,
        )),
        _ => None,
    }
}

pub(super) fn resolve_operation(name: &str, ty: &Type) -> Option<Intrinsic> {
    let intrinsic = Intrinsic::from_name(name)?;
    match (ty, intrinsic) {
        (Type::BinaryOperation(inner), Intrinsic::IntegerBinary(_))
            if **inner == Type::I32 => Some(intrinsic),
        (
            Type::CheckedBinaryOperation(inner),
            Intrinsic::CheckedInteger(
                CheckedIntegerOperation::Add
                | CheckedIntegerOperation::Subtract
                | CheckedIntegerOperation::Multiply,
            ),
        ) if **inner == Type::I32 => Some(intrinsic),
        (
            Type::BinaryOperation(inner),
            Intrinsic::FloatingPoint(FloatingPointOperation::Binary(_)),
        ) if **inner == Type::F64 => Some(intrinsic),
        (
            Type::IntegerCondition,
            Intrinsic::Branch(BranchOperation::Ordered {
                signedness: IntegerSignedness::Signed,
                ..
            }),
        )
        | (Type::FloatCondition, Intrinsic::Branch(BranchOperation::Float(_))) => {
            Some(intrinsic)
        }
        _ => None,
    }
}

pub(super) fn signatures_match(lhs: &Signature, rhs: &Signature) -> bool {
    lhs.parameters == rhs.parameters
        && lhs.has_else_parameter == rhs.has_else_parameter
        && lhs.return_type == rhs.return_type
}

fn intrinsic_value_type(ty: IntrinsicValueType) -> Type {
    match ty {
        IntrinsicValueType::AnyGpr => Type::AnyGpr,
        IntrinsicValueType::BytecodeOffset => Type::BytecodeOffset,
        IntrinsicValueType::F32 => Type::F32,
        IntrinsicValueType::F64 => Type::F64,
        IntrinsicValueType::FunctionSymbol => Type::FunctionSymbol,
        IntrinsicValueType::I32 => Type::I32,
        IntrinsicValueType::Label => Type::label(),
        IntrinsicValueType::Memory => Type::Memory,
        IntrinsicValueType::Operand => Type::Operand,
        IntrinsicValueType::SlowPath => Type::SlowPath,
        IntrinsicValueType::U8 => Type::U8,
        IntrinsicValueType::U16 => Type::U16,
        IntrinsicValueType::U32 => Type::U32,
        IntrinsicValueType::U64 => Type::U64,
        IntrinsicValueType::Value => Type::Value,
        IntrinsicValueType::ValueTag => Type::ValueTag,
        IntrinsicValueType::Vm => Type::VM,
    }
}

fn semantic_signature(signature: IntrinsicCallSignature) -> Signature {
    let mut parameters = signature
        .parameters()
        .iter()
        .map(|parameter| (parameter.mode, intrinsic_value_type(parameter.ty)))
        .collect::<Vec<_>>();
    if signature.has_else_parameter {
        parameters.push((ParameterMode::In, Type::label()));
    }
    let return_type = signature.result.map(|result| match result {
        IntrinsicResultType::Single(ty) => intrinsic_value_type(ty),
        IntrinsicResultType::Pair(lhs, rhs) => Type::Tuple(vec![intrinsic_value_type(lhs), intrinsic_value_type(rhs)]),
    });
    Signature {
        parameters,
        has_else_parameter: signature.has_else_parameter,
        return_type,
        terminal: false,
    }
}

pub(super) fn resolve_intrinsic(name: &str, argument_count: usize) -> Option<ResolvedIntrinsic> {
    let intrinsic = Intrinsic::from_name(name)?;
    Some(ResolvedIntrinsic {
        intrinsic,
        signature: semantic_signature(
            intrinsic.call_signature(argument_count)?,
        ),
    })
}

pub(super) fn intrinsic_is_terminal(name: &str) -> bool {
    Intrinsic::from_name(name).is_some_and(Intrinsic::is_terminal)
}
