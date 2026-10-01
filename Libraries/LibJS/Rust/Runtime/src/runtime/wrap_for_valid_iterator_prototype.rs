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
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::realm::Realm;

/// %WrapForValidIteratorPrototype%.
#[repr(C)]
#[derive(Trace)]
pub struct WrapForValidIteratorPrototype {
    base: Object,
}

define_object_class!(WrapForValidIteratorPrototype, extends: [Object], methods: {
    initialize: WrapForValidIteratorPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

impl WrapForValidIteratorPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<WrapForValidIteratorPrototype> {
        realm.create_object(
            vm,
            WrapForValidIteratorPrototype {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.intrinsics().iterator_prototype(vm),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            },
        )
    }

    fn initialize(_object: &Object, _vm: &Vm, _realm: Gc<Realm>) {
        // NB: %WrapForValidIteratorPrototype%.next and .return come with the Iterator builtins.
    }
}
