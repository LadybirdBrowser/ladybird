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
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::realm::Realm;

/// %Object.prototype%, an immutable prototype exotic object.
#[repr(C)]
#[derive(Trace)]
pub struct ObjectPrototype {
    base: Object,
}

define_object_class!(ObjectPrototype, extends: [Object], methods: {
    initialize: ObjectPrototype::initialize,
    internal_set_prototype_of: ObjectPrototype::internal_set_prototype_of,
    ..ORDINARY_OBJECT_METHODS
});

impl ObjectPrototype {
    pub fn new(vm: &Vm, realm: Gc<Realm>) -> ObjectPrototype {
        ObjectPrototype {
            base: Object::new_without_prototype(vm, Self::CLASS, realm, MayInterfereWithIndexedPropertyAccess::No),
        }
    }

    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<ObjectPrototype> {
        realm.create_object(vm, Self::new(vm, realm))
    }

    // NB: The Object.prototype methods (hasOwnProperty, toString, toLocaleString, valueOf, propertyIsEnumerable,
    //     isPrototypeOf, the Annex B accessor methods and __proto__) come with the Object builtins, before the
    //     "constructor" property CreateIntrinsics defines.
    fn initialize(_object: &Object, _vm: &Vm, _realm: Gc<Realm>) {}

    // 10.4.7.1 [[SetPrototypeOf]] ( V ), https://tc39.es/ecma262/#sec-immutable-prototype-exotic-objects-setprototypeof-v
    fn internal_set_prototype_of(object: &Object, vm: &Vm, prototype: Option<Gc<Object>>) -> ThrowCompletionOr<bool> {
        // 1. Return ? SetImmutablePrototype(O, V).
        object.set_immutable_prototype(vm, prototype)
    }
}
