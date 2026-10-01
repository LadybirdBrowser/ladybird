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
use crate::runtime::object::define_object_class;
use crate::runtime::realm::Realm;
use crate::runtime::string_object::{STRING_OBJECT_METHODS, StringObject};

/// %String.prototype%, which is a String exotic object whose [[StringData]] is the empty String.
#[repr(C)]
#[derive(Trace)]
pub struct StringPrototype {
    base: StringObject,
}

define_object_class!(StringPrototype, extends: [StringObject, Object], methods: {
    initialize: StringPrototype::initialize,
    ..STRING_OBJECT_METHODS
});

impl StringPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<StringPrototype> {
        realm.create_object(
            vm,
            StringPrototype {
                base: StringObject::new(vm, Self::CLASS, vm.empty_string(), realm.object_prototype()),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        StringObject::initialize(object, vm, realm);

        // 22.1.3 Properties of the String Prototype Object, https://tc39.es/ecma262/#sec-properties-of-the-string-prototype-object
        // NB: The String.prototype methods come with the String builtins, and so do the Annex B methods, trimLeft and
        //     trimRight, which are the trimStart and trimEnd functions.
    }
}
