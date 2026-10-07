/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ops::Deref;

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::runtime::error::Error;
use crate::runtime::realm::Realm;

#[repr(C)]
#[derive(Trace)]
pub struct SuppressedError {
    base: Error,
}

define_cell!(SuppressedError, Object, extends: [Error, Object]);

impl Deref for SuppressedError {
    type Target = Error;

    fn deref(&self) -> &Error {
        &self.base
    }
}

impl SuppressedError {
    pub fn new(vm: &Vm, prototype: Gc<Object>) -> SuppressedError {
        SuppressedError {
            base: Error::new(vm, Self::CLASS, prototype),
        }
    }

    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<SuppressedError> {
        realm.create_object(
            vm,
            SuppressedError::new(vm, realm.intrinsics().suppressed_error_prototype(vm)),
        )
    }
}
