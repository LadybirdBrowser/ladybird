/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The slow paths for literals, iterators, generators and control flow, and their helpers.

use crate::bytecode::op;
use crate::interpreter::runtime_functions::{SlowPathControl, handle_asm_exception};
use crate::interpreter::vm::Vm;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;

/// Throws a new error and hands it to the interpreter.
fn throw_error(
    vm: &Vm,
    pc: u32,
    kind: ErrorKind,
    error_type: ErrorType,
    arguments: &[&dyn core::fmt::Display],
) -> SlowPathControl {
    match vm.throw_completion::<()>(kind, error_type, arguments) {
        Err(throw) => handle_asm_exception(vm, pc, throw.value()),
        Ok(()) => unreachable!("throw_completion always throws"),
    }
}

pub fn throw_if_tdz(vm: &Vm, pc: u32, values: &op::ThrowIfTDZValues) -> SlowPathControl {
    let value = values.src;
    if value.is_empty() {
        return throw_error(
            vm,
            pc,
            ErrorKind::ReferenceError,
            ErrorType::BindingNotInitialized,
            &[&value],
        );
    }
    SlowPathControl::continue_at(pc + op::ThrowIfTDZ::LENGTH)
}

pub fn throw_if_not_object(vm: &Vm, pc: u32, values: &op::ThrowIfNotObjectValues) -> SlowPathControl {
    let src = values.src;
    if !src.is_object() {
        return throw_error(vm, pc, ErrorKind::TypeError, ErrorType::NotAnObject, &[&src]);
    }
    SlowPathControl::continue_at(pc + op::ThrowIfNotObject::LENGTH)
}

pub fn throw_if_nullish(vm: &Vm, pc: u32, values: &op::ThrowIfNullishValues) -> SlowPathControl {
    let value = values.src;
    if value.is_nullish() {
        return throw_error(vm, pc, ErrorKind::TypeError, ErrorType::NotObjectCoercible, &[&value]);
    }
    SlowPathControl::continue_at(pc + op::ThrowIfNullish::LENGTH)
}

pub fn throw_const_assignment(vm: &Vm, pc: u32) -> SlowPathControl {
    throw_error(vm, pc, ErrorKind::TypeError, ErrorType::InvalidAssignToConst, &[])
}
