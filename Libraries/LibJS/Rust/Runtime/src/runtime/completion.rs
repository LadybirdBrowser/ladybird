/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::value::Value;

/// A thrown value. Throws are kept distinct from values so that one cannot be mistaken for a normal result.
#[derive(Clone, Copy, Debug)]
pub struct Throw(Value);

impl Throw {
    pub fn new(value: Value) -> Self {
        debug_assert!(!value.is_empty());
        Self(value)
    }

    pub fn value(self) -> Value {
        self.0
    }
}

/// The result of an operation that completes normally with a T or throws.
pub type ThrowCompletionOr<T> = Result<T, Throw>;

/// MUST() of the C++ runtime: the spec's `!`, for an operation that cannot throw here.
pub trait Must<T> {
    fn must(self) -> T;
}

impl<T> Must<T> for ThrowCompletionOr<T> {
    #[track_caller]
    fn must(self) -> T {
        match self {
            Ok(value) => value,
            Err(throw) => panic!("an operation that cannot throw threw {:?}", throw.value()),
        }
    }
}

pub use libjs_abi::CompletionType;

/// The [[Type]] of a completion as the bytecode encodes it, in the operands of IteratorClose and SetCompletionType.
pub fn completion_type_from_bytecode(encoded: u32) -> CompletionType {
    match encoded {
        0 => CompletionType::Empty,
        1 => CompletionType::Normal,
        2 => CompletionType::Break,
        3 => CompletionType::Continue,
        4 => CompletionType::Return,
        5 => CompletionType::Throw,
        _ => unreachable!("the bytecode encodes a completion type"),
    }
}

// 6.2.4 The Completion Record Specification Type, https://tc39.es/ecma262/#sec-completion-record-specification-type
/// A completion record, for the operations that take or return one rather than a ThrowCompletionOr, like
/// IteratorClose. There is no [[Target]], since control flow is handled in bytecode.
#[derive(Clone, Copy, Debug)]
pub struct Completion {
    completion_type: CompletionType, // [[Type]]
    value: Value,                    // [[Value]]
}

impl Completion {
    pub fn new(completion_type: CompletionType, value: Value) -> Self {
        debug_assert!(completion_type != CompletionType::Empty);
        debug_assert!(!value.is_empty());
        Self { completion_type, value }
    }

    // 5.2.3.1 Implicit Completion Values, https://tc39.es/ecma262/#sec-implicit-completion-values
    pub fn normal(value: Value) -> Self {
        Self::new(CompletionType::Normal, value)
    }

    pub fn completion_type(&self) -> CompletionType {
        self.completion_type
    }

    pub fn value(&self) -> Value {
        self.value
    }

    /// "abrupt completion refers to any completion with a [[Type]] value other than normal"
    pub fn is_abrupt(&self) -> bool {
        self.completion_type != CompletionType::Normal
    }

    pub fn is_error(&self) -> bool {
        self.completion_type == CompletionType::Throw
    }

    pub fn release_error(self) -> Throw {
        assert!(self.is_error());
        Throw::new(self.value)
    }

    /// The completion the way TRY() and ASM_TRY() see a C++ Completion: a throw completion is an error, and any other
    /// completion its value.
    pub fn into_throw_completion_or(self) -> ThrowCompletionOr<Value> {
        if self.is_error() {
            return Err(Throw::new(self.value));
        }
        Ok(self.value)
    }
}

impl From<Throw> for Completion {
    fn from(throw: Throw) -> Self {
        Self::new(CompletionType::Throw, throw.value())
    }
}

impl From<ThrowCompletionOr<Value>> for Completion {
    fn from(result: ThrowCompletionOr<Value>) -> Self {
        match result {
            Ok(value) => Self::normal(value),
            Err(throw) => throw.into(),
        }
    }
}
