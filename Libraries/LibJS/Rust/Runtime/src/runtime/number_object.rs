/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::Deref;

use libjs_runtime_macros::Trace;

use crate::gc::class::{Class, GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;
use crate::runtime::realm::Realm;

/// A Number object, whose [[NumberData]] is `value`.
#[repr(C)]
#[derive(Trace)]
pub struct NumberObject {
    base: Object,
    value: Cell<f64>,
}

define_cell!(NumberObject, Object, extends: [Object]);

impl Deref for NumberObject {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl NumberObject {
    /// NumberObject(double, Object& prototype), for `class`, which is NumberObject or a class that extends it.
    pub fn new(vm: &Vm, class: &'static Class, value: f64, prototype: Gc<Object>) -> NumberObject {
        NumberObject {
            base: Object::new_with_prototype(vm, class, prototype, MayInterfereWithIndexedPropertyAccess::No),
            value: Cell::new(value),
        }
    }

    pub fn create(vm: &Vm, realm: Gc<Realm>, value: f64) -> Gc<NumberObject> {
        realm.create_object(
            vm,
            NumberObject::new(vm, Self::CLASS, value, realm.intrinsics().number_prototype(vm)),
        )
    }

    pub fn number(&self) -> f64 {
        self.value.get()
    }
}
