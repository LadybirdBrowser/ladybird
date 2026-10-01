/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::runtime::number_object::NumberObject;
use crate::runtime::object::{ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::realm::Realm;

/// %Number.prototype%, which is a Number object whose [[NumberData]] is +0𝔽.
#[repr(C)]
#[derive(Trace)]
pub struct NumberPrototype {
    base: NumberObject,
}

define_object_class!(NumberPrototype, extends: [NumberObject, Object], methods: {
    initialize: NumberPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

impl NumberPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<NumberPrototype> {
        realm.create_object(
            vm,
            NumberPrototype {
                base: NumberObject::new(vm, Self::CLASS, 0.0, realm.object_prototype()),
            },
        )
    }

    // NB: The Number.prototype methods come with the Number builtins.
    fn initialize(_object: &Object, _vm: &Vm, _realm: Gc<Realm>) {}
}
