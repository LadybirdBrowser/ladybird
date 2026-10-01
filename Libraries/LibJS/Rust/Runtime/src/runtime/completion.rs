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
