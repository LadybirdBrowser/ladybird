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

/// An object with an [[IsRawJSON]] internal slot, which JSON.rawJSON creates.
#[repr(C)]
#[derive(Trace)]
pub struct RawJSONObject {
    base: Object,
}

define_object_class!(RawJSONObject, extends: [Object], methods: {
    ..ORDINARY_OBJECT_METHODS
});

impl RawJSONObject {
    pub fn create(vm: &Vm, realm: Gc<Realm>, prototype: Option<Gc<Object>>) -> Gc<RawJSONObject> {
        let Some(prototype) = prototype else {
            return realm.create_object(
                vm,
                RawJSONObject {
                    base: Object::new_with_shape(
                        Self::CLASS,
                        realm.empty_object_shape(),
                        MayInterfereWithIndexedPropertyAccess::No,
                    ),
                },
            );
        };

        realm.create_object(
            vm,
            RawJSONObject {
                base: Object::new_with_prototype(vm, Self::CLASS, prototype, MayInterfereWithIndexedPropertyAccess::No),
            },
        )
    }
}

impl Object {
    pub fn is_raw_json_object(&self) -> bool {
        self.is::<RawJSONObject>()
    }
}
