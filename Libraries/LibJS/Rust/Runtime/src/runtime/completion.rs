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
