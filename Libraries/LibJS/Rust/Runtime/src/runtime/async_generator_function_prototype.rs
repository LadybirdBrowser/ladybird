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

/// %AsyncGeneratorFunction.prototype%.
#[repr(C)]
#[derive(Trace)]
pub struct AsyncGeneratorFunctionPrototype {
    base: Object,
}

define_object_class!(AsyncGeneratorFunctionPrototype, extends: [Object], methods: {
    initialize: AsyncGeneratorFunctionPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

impl AsyncGeneratorFunctionPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<AsyncGeneratorFunctionPrototype> {
        realm.create_object(
            vm,
            AsyncGeneratorFunctionPrototype {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.function_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // The constructor cannot be set at this point since it has not been initialized.

        // 27.4.3.2 AsyncGeneratorFunction.prototype.prototype, https://tc39.es/ecma262/#sec-asyncgeneratorfunction-prototype-prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.intrinsics().async_generator_prototype()),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        // 27.4.3.3 AsyncGeneratorFunction.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-asyncgeneratorfunction-prototype-tostringtag
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_fly_string(
                vm,
                &Utf16FlyString::from_utf8("AsyncGeneratorFunction"),
            )),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }
}
