/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::{Utf16FlyString, Utf16String};
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;

#[repr(C)]
#[derive(Trace)]
pub struct AggregateErrorPrototype {
    base: Object,
}

define_object_class!(AggregateErrorPrototype, extends: [Object], methods: {
    initialize: AggregateErrorPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

impl AggregateErrorPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<AggregateErrorPrototype> {
        realm.create_object(
            vm,
            AggregateErrorPrototype {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.intrinsics().error_prototype(vm),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, _realm: Gc<Realm>) {
        let attributes = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_direct_property(
            vm,
            &vm.names.name,
            Value::from_string(PrimitiveString::create_from_fly_string(
                vm,
                &Utf16FlyString::from_utf8("AggregateError"),
            )),
            attributes,
        );
        object.define_direct_property(
            vm,
            &vm.names.message,
            Value::from_string(PrimitiveString::create(vm, Utf16String::default())),
            attributes,
        );
    }
}
