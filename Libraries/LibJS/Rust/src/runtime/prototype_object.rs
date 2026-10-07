/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Helpers for the built-in functions of prototypes, which check the this value they are called with.

use crate::gc::class::{Extends, GcCell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;

pub fn this_object(vm: &Vm) -> ThrowCompletionOr<Gc<Object>> {
    let this_value = vm.this_value();
    if !this_value.is_object() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObject, &[&this_value]);
    }
    Ok(this_value.as_object())
}

/// Use typed_this_object() when the spec coerces |this| value to an object.
pub fn typed_this_object<T: GcCell + Extends<Object>>(vm: &Vm, display_name: &str) -> ThrowCompletionOr<Gc<T>> {
    let this_object = vm.this_value().to_object(vm)?;
    if let Some(typed_object) = this_object.downcast::<T>() {
        return Ok(typed_object);
    }
    vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&display_name])
}

/// Use typed_this_value() when the spec does not coerce |this| value to an object.
pub fn typed_this_value<T: GcCell + Extends<Object>>(vm: &Vm, display_name: &str) -> ThrowCompletionOr<Gc<T>> {
    let this_value = vm.this_value();
    if this_value.is_object()
        && let Some(typed_object) = this_value.as_object().downcast::<T>()
    {
        return Ok(typed_object);
    }
    vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&display_name])
}
