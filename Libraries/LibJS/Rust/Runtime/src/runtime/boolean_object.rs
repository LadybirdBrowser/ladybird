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

/// A Boolean object, whose [[BooleanData]] is `value`.
#[repr(C)]
#[derive(Trace)]
pub struct BooleanObject {
    base: Object,
    value: Cell<bool>,
}

define_cell!(BooleanObject, Object, extends: [Object]);

impl Deref for BooleanObject {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl BooleanObject {
    /// BooleanObject(bool, Object& prototype), for `class`, which is BooleanObject or a class that extends it.
    pub fn new(vm: &Vm, class: &'static Class, value: bool, prototype: Gc<Object>) -> BooleanObject {
        BooleanObject {
            base: Object::new_with_prototype(vm, class, prototype, MayInterfereWithIndexedPropertyAccess::No),
            value: Cell::new(value),
        }
    }

    pub fn create(vm: &Vm, realm: Gc<Realm>, value: bool) -> Gc<BooleanObject> {
        realm.create_object(
            vm,
            BooleanObject::new(vm, Self::CLASS, value, realm.intrinsics().boolean_prototype(vm)),
        )
    }

    pub fn boolean(&self) -> bool {
        self.value.get()
    }
}
