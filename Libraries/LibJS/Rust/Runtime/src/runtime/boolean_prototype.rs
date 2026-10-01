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
use crate::runtime::boolean_object::BooleanObject;
use crate::runtime::object::{ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::realm::Realm;

/// %Boolean.prototype%, which is a Boolean object whose [[BooleanData]] is false.
#[repr(C)]
#[derive(Trace)]
pub struct BooleanPrototype {
    base: BooleanObject,
}

define_object_class!(BooleanPrototype, extends: [BooleanObject, Object], methods: {
    initialize: BooleanPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

impl BooleanPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<BooleanPrototype> {
        realm.create_object(
            vm,
            BooleanPrototype {
                base: BooleanObject::new(vm, Self::CLASS, false, realm.object_prototype()),
            },
        )
    }

    // NB: Boolean.prototype.toString and Boolean.prototype.valueOf come with the Boolean builtins.
    fn initialize(_object: &Object, _vm: &Vm, _realm: Gc<Realm>) {}
}
