/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16FlyString;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;

/// %AsyncFunction.prototype%.
#[repr(C)]
#[derive(Trace)]
pub struct AsyncFunctionPrototype {
    base: Object,
}

define_object_class!(AsyncFunctionPrototype, extends: [Object], methods: {
    initialize: AsyncFunctionPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

impl AsyncFunctionPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<AsyncFunctionPrototype> {
        realm.create_object(
            vm,
            AsyncFunctionPrototype {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.function_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, _realm: Gc<Realm>) {
        // 27.7.3.2 AsyncFunction.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-async-function-prototype-properties-toStringTag
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_fly_string(
                vm,
                &Utf16FlyString::from_utf8("AsyncFunction"),
            )),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }
}
