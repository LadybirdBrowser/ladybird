/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::fmt;

use super::completion::ThrowCompletionOr;
use super::error_types::ErrorType;
use crate::interpreter::runtime_functions::unimplemented_runtime_function;
use crate::interpreter::vm::Vm;

/// The constructors of the errors the runtime throws: %Error% and the NativeError constructors, as the C++ Error
/// subclasses.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ErrorKind {
    Error,
    EvalError,
    InternalError,
    RangeError,
    ReferenceError,
    SyntaxError,
    TypeError,
    URIError,
}

impl Vm {
    /// 5.2.3.2 Throw an Exception, https://tc39.es/ecma262/#sec-throw-an-exception
    #[cold]
    pub fn throw_completion<T>(
        &self,
        kind: ErrorKind,
        error_type: ErrorType,
        arguments: &[&dyn fmt::Display],
    ) -> ThrowCompletionOr<T> {
        self.throw_completion_with_message(kind, error_type.message(arguments))
    }

    /// Throws a new error of `kind` with `message`.
    #[cold]
    pub fn throw_completion_with_message<T>(&self, kind: ErrorKind, message: String) -> ThrowCompletionOr<T> {
        unimplemented_runtime_function(&format!("creating a {kind:?} with the message \"{message}\""), 0)
    }
}
