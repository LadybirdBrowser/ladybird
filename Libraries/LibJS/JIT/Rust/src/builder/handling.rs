/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! How the graph builder handles each bytecode opcode.
//!
//! The match below is exhaustive on purpose: a new opcode does not compile
//! until someone decides how the JIT handles it.

use crate::bytecode::OpCode;

/// How the graph builder handles one opcode.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Handling {
    /// Built from native IR nodes.
    Native,
    /// Built as a `Generic` node that runs the interpreter's slow path for
    /// the opcode.
    Generic(GenericInfo),
    /// Built as IR for the common cases, with cold blocks that run the
    /// slow path of a `Generic` node for the others (`Op::CallSlowPath`).
    Expanded(GenericInfo),
    /// Compilation fails with the given reason.
    Unsupported(&'static str),
}

impl Handling {
    pub fn generic(self) -> Option<GenericInfo> {
        match self {
            Handling::Generic(info) | Handling::Expanded(info) => Some(info),
            _ => None,
        }
    }
}

/// What the graph builder and later passes need to know about the slow path
/// of a generic opcode.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct GenericInfo {
    pub can_throw: bool,
    /// Calls a JS function with call semantics (the runtime's call helper).
    pub is_js_call: bool,
    /// Reads frame slots that are not among its operands (the arguments, or
    /// any slot through direct eval or the debugger), so every slot must be
    /// written back to the frame before it, not just the live ones.
    pub reads_whole_frame: bool,
}

const GENERIC: Handling = Handling::Generic(GenericInfo {
    can_throw: true,
    is_js_call: false,
    reads_whole_frame: false,
});

const GENERIC_NO_THROW: Handling = Handling::Generic(GenericInfo {
    can_throw: false,
    is_js_call: false,
    reads_whole_frame: false,
});

const GENERIC_CALL: Handling = Handling::Generic(GenericInfo {
    can_throw: true,
    is_js_call: true,
    reads_whole_frame: false,
});

const GENERIC_READS_WHOLE_FRAME: Handling = Handling::Generic(GenericInfo {
    can_throw: true,
    is_js_call: false,
    reads_whole_frame: true,
});

const GENERIC_CALL_READS_WHOLE_FRAME: Handling = Handling::Generic(GenericInfo {
    can_throw: true,
    is_js_call: true,
    reads_whole_frame: true,
});

const EXPANDED: Handling = Handling::Expanded(GenericInfo {
    can_throw: true,
    is_js_call: false,
    reads_whole_frame: false,
});

const SUSPENDS: Handling =
    Handling::Unsupported("suspends a generator or async function; JIT code is only entered at function entry");

/// How the graph builder handles `opcode`.
pub fn handling(opcode: OpCode) -> Handling {
    use OpCode as O;
    match opcode {
        // Control flow, moves and frame setup.
        O::Enter
        | O::Mov
        | O::Mov2
        | O::Mov3
        | O::MovSrcUndefined
        | O::MovUndefined2
        | O::MovUndefined3
        | O::Jump
        | O::JumpIf
        | O::JumpTrue
        | O::JumpFalse
        | O::JumpLoop
        | O::JumpIfLoop
        | O::JumpTrueLoop
        | O::JumpFalseLoop
        | O::JumpNullish
        | O::JumpUndefined
        | O::Return
        | O::End => Handling::Native,

        // Truthiness, built from a truthiness branch and a phi.
        O::Not | O::ToBoolean => Handling::Native,

        // Trivial frame field accesses and tag checks without a slow path.
        O::GetLexicalEnvironment
        | O::SetLexicalEnvironment
        | O::LeavePrivateEnvironment
        | O::IsCallable
        | O::GetArgumentCount => Handling::Native,

        O::Catch => Handling::Unsupported("starts an exception handler; handler blocks are not built"),
        O::Await | O::Yield | O::YieldIteratorResult => SUSPENDS,

        // Calls.
        O::Call
        | O::CallConstruct
        | O::CallWithArgumentArray
        | O::CallConstructWithArgumentArray
        | O::SuperCallWithArgumentArray => GENERIC_CALL,
        O::CallDirectEval | O::CallDirectEvalWithArgumentArray => GENERIC_CALL_READS_WHOLE_FRAME,
        // The builtin slow paths call the builtin directly, and any other
        // callee through the generic call machinery, without leaving the
        // caller's frame.
        O::CallBuiltinMathExp
        | O::CallBuiltinMathLog
        | O::CallBuiltinMathPow
        | O::CallBuiltinMathImul
        | O::CallBuiltinMathRandom
        | O::CallBuiltinMathSin
        | O::CallBuiltinMathCos
        | O::CallBuiltinMathTan
        | O::CallBuiltinRegExpPrototypeExec
        | O::CallBuiltinRegExpPrototypeReplace
        | O::CallBuiltinRegExpPrototypeSplit
        | O::CallBuiltinOrdinaryHasInstance
        | O::CallBuiltinArrayIteratorPrototypeNext
        | O::CallBuiltinMapIteratorPrototypeNext
        | O::CallBuiltinSetIteratorPrototypeNext
        | O::CallBuiltinStringIteratorPrototypeNext
        | O::CallBuiltinStringFromCharCode => GENERIC,
        // Reading a character of a string, built as IR for strings.
        O::CallBuiltinStringPrototypeCharCodeAt | O::CallBuiltinStringPrototypeCharAt => EXPANDED,
        // Math functions of one number, built as IR for numbers.
        O::CallBuiltinMathAbs
        | O::CallBuiltinMathFloor
        | O::CallBuiltinMathCeil
        | O::CallBuiltinMathRound
        | O::CallBuiltinMathSqrt => EXPANDED,

        O::CreateArguments | O::CreateRestParams | O::Debugger => GENERIC_READS_WHOLE_FRAME,

        // Slow paths that only allocate or inspect values.
        O::NewObject
        | O::NewObjectWithNoPrototype
        | O::NewArray
        | O::NewPrimitiveArray
        | O::NewFunction
        | O::NewReferenceError
        | O::NewTypeError
        | O::CreateLexicalEnvironment
        | O::CreateVariableEnvironment
        | O::CreatePrivateEnvironment
        | O::Typeof
        | O::IsConstructor
        | O::GetNewTarget => GENERIC_NO_THROW,

        // Comparisons and conditional jumps, built as IR (see `speculation`).
        // The slow paths of the jumps return the next pc.
        O::LessThan
        | O::LessThanEquals
        | O::GreaterThan
        | O::GreaterThanEquals
        | O::StrictlyEquals
        | O::StrictlyInequals
        | O::LooselyEquals
        | O::LooselyInequals
        | O::LessThanRhsInt32
        | O::LessThanEqualsRhsInt32
        | O::GreaterThanRhsInt32
        | O::GreaterThanEqualsRhsInt32
        | O::StrictlyEqualsRhsInt32
        | O::StrictlyInequalsRhsInt32
        | O::LooselyEqualsRhsInt32
        | O::LooselyInequalsRhsInt32
        | O::JumpLessThan
        | O::JumpGreaterThan
        | O::JumpLessThanEquals
        | O::JumpGreaterThanEquals
        | O::JumpLooselyEquals
        | O::JumpLooselyInequals
        | O::JumpStrictlyEquals
        | O::JumpStrictlyInequals
        | O::JumpLessThanRhsInt32
        | O::JumpLessThanEqualsRhsInt32
        | O::JumpGreaterThanRhsInt32
        | O::JumpGreaterThanEqualsRhsInt32
        | O::JumpStrictlyEqualsRhsInt32
        | O::JumpStrictlyInequalsRhsInt32
        | O::JumpLooselyEqualsRhsInt32
        | O::JumpLooselyInequalsRhsInt32
        | O::JumpLessThanLoop
        | O::JumpGreaterThanLoop
        | O::JumpLessThanEqualsLoop
        | O::JumpGreaterThanEqualsLoop
        | O::JumpLooselyEqualsLoop
        | O::JumpLooselyInequalsLoop
        | O::JumpStrictlyEqualsLoop
        | O::JumpStrictlyInequalsLoop
        | O::JumpLessThanLoopRhsInt32
        | O::JumpLessThanEqualsLoopRhsInt32
        | O::JumpGreaterThanLoopRhsInt32
        | O::JumpGreaterThanEqualsLoopRhsInt32
        | O::JumpStrictlyEqualsLoopRhsInt32
        | O::JumpStrictlyInequalsLoopRhsInt32
        | O::JumpLooselyEqualsLoopRhsInt32
        | O::JumpLooselyInequalsLoopRhsInt32 => EXPANDED,

        // Arithmetic, built as IR (see `speculation`).
        O::Add
        | O::Sub
        | O::Mul
        | O::Div
        | O::Mod
        | O::Increment
        | O::Decrement
        | O::PostfixIncrement
        | O::PostfixDecrement
        | O::BitwiseXor
        | O::BitwiseAnd
        | O::BitwiseOr
        | O::BitwiseNot
        | O::LeftShift
        | O::RightShift
        | O::UnsignedRightShift
        | O::UnaryPlus
        | O::UnaryMinus
        | O::ToInt32
        | O::AddLhsInt32
        | O::AddRhsInt32
        | O::SubRhsInt32
        | O::MulRhsInt32
        | O::DivRhsInt32
        | O::ModRhsInt32
        | O::BitwiseXorRhsInt32
        | O::BitwiseAndRhsInt32
        | O::BitwiseOrRhsInt32
        | O::LeftShiftRhsInt32
        | O::RightShiftRhsInt32
        | O::UnsignedRightShiftRhsInt32 => EXPANDED,

        // Keyed access and `length` with an inline fast path in the
        // interpreter for arrays, typed arrays and strings.
        O::GetByValue | O::PutByValue | O::GetLength => EXPANDED,

        // Named property access through the interpreter's property lookup
        // caches.
        O::GetById | O::PutById => EXPANDED,

        // Bindings at static environment coordinates.
        O::GetBinding
        | O::GetInitializedBinding
        | O::GetCalleeAndThisFromEnvironment
        | O::SetLexicalBinding
        | O::SetVariableBinding
        | O::InitializeLexicalBinding
        | O::InitializeVariableBinding
        // Bindings that environments of a shape get next.
        | O::CreateVariable => EXPANDED,

        // Global variables through the interpreter's global variable caches.
        O::GetGlobal | O::SetGlobal => EXPANDED,

        // The next key of a for-in loop over a cached key snapshot.
        O::ObjectPropertyIteratorNext => EXPANDED,


        // Conversions that change nothing for objects, and for lengths that
        // are int32 values already.
        O::ToObject | O::ToLength => EXPANDED,

        // Checks that only throw in their slow path.
        O::ThrowIfTDZ | O::ThrowIfNotObject | O::ThrowIfNullish | O::ResolveThisBinding => EXPANDED,

        O::Exp
        | O::ConcatString
        | O::CopyObjectExcludingProperties
        | O::ImportCall
        | O::NewClass
        | O::GetImportMeta
        | O::GetImport
        | O::GetSuperConstructor
        | O::DynamicGetBinding
        | O::DynamicGetInitializedBinding
        | O::DynamicInitializeLexicalBinding
        | O::DynamicInitializeVariableBinding
        | O::DynamicSetLexicalBinding
        | O::DynamicSetVariableBinding
        | O::ResolveBinding
        | O::ResolveGlobalBinding
        | O::ResolveSuperBase
        | O::SetResolvedBinding
        | O::TypeofBinding
        | O::DynamicTypeofBinding
        | O::TypeofGlobal
        | O::VerifyEnvironmentCoordinate
        | O::HasPrivateId
        | O::SetFunctionName
        | O::NewArrayWithLength
        | O::ArrayAppend
        | O::EnterObjectEnvironment
        | O::ThrowConstAssignment
        | O::ThrowNotAFunction
        | O::Throw
        | O::ToString
        | O::ToPrimitiveWithStringHint
        | O::DynamicGetCalleeAndThisFromEnvironment
        | O::GetByIdWithThis
        | O::PutByIdWithThis
        | O::GetByValueWithThis
        | O::PutByValueWithThis
        | O::PutBySpread
        | O::GetLengthWithThis
        | O::GetMethod
        | O::GetIterator
        | O::GetObjectPropertyIterator
        | O::IteratorClose
        | O::IteratorNext
        | O::IteratorNextUnpack
        | O::IteratorToArray
        | O::CacheObjectShape
        | O::InitObjectLiteralProperty
        | O::NewRegExp
        | O::InstanceOf
        | O::In
        | O::AddPrivateName
        | O::CreateAsyncFromSyncIterator
        | O::CreateDataPropertyOrThrow
        | O::CreateImmutableBinding
        | O::CreateMutableBinding
        | O::DeleteById
        | O::DeleteByValue
        | O::DeleteVariable
        | O::GetCompletionFields
        | O::SetCompletionType
        | O::GetTemplateObject
        | O::GetPrivateById
        | O::PutPrivateById
        | O::ExpRhsInt32 => GENERIC,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bytecode::NUM_OPCODES;

    fn all_opcodes() -> impl Iterator<Item = OpCode> {
        (0..NUM_OPCODES).map(|value| OpCode::from_u8(u8::try_from(value).unwrap()).unwrap())
    }

    #[test]
    fn terminators_are_native_generic_jumps_or_unsupported() {
        for opcode in all_opcodes() {
            if !opcode.is_terminator() {
                continue;
            }
            match handling(opcode) {
                Handling::Native | Handling::Unsupported(_) => {}
                Handling::Generic(_) | Handling::Expanded(_) => assert!(
                    opcode.name().starts_with("Jump") || opcode == OpCode::Throw,
                    "{} is a generic terminator",
                    opcode.name()
                ),
            }
        }
    }
}
