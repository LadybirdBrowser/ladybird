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

/// %Iterator.prototype%.
#[repr(C)]
#[derive(Trace)]
pub struct IteratorPrototype {
    base: Object,
}

define_object_class!(IteratorPrototype, extends: [Object], methods: {
    initialize: IteratorPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

impl IteratorPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<IteratorPrototype> {
        realm.create_object(
            vm,
            IteratorPrototype {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.object_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            },
        )
    }

    fn initialize(_object: &Object, _vm: &Vm, _realm: Gc<Realm>) {
        // NB: The Iterator.prototype methods, @@iterator, and the accessors of Iterator.prototype.constructor and
        //     Iterator.prototype [ @@toStringTag ] come with the Iterator builtins.
    }
}
