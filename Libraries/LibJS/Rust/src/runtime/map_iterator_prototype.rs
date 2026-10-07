/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16FlyString;
use libjs_abi::Builtin;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::iterator::create_iterator_result_object;
use crate::runtime::map_iterator::MapIterator;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::typed_this_value;
use crate::runtime::realm::Realm;

/// %MapIteratorPrototype%.
#[repr(C)]
#[derive(Trace)]
pub struct MapIteratorPrototype {
    base: Object,
}

define_object_class!(MapIteratorPrototype, extends: [Object], methods: {
    initialize: MapIteratorPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

impl MapIteratorPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<MapIteratorPrototype> {
        realm.create_object(
            vm,
            MapIteratorPrototype {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.intrinsics().iterator_prototype(vm),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        object.define_native_function(
            vm,
            realm,
            &vm.names.next,
            raw_native!(MapIteratorPrototype::next),
            0,
            PropertyAttributes::new(Attribute::CONFIGURABLE | Attribute::WRITABLE),
            Some(Builtin::MapIteratorPrototypeNext),
        );

        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_fly_string(
                vm,
                &Utf16FlyString::from_utf8("Map Iterator"),
            )),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 24.1.5.2.1 %MapIteratorPrototype%.next ( ), https://tc39.es/ecma262/#sec-%mapiteratorprototype%.next
    fn next(vm: &Vm) -> ThrowCompletionOr<Value> {
        let iterator = typed_this_value::<MapIterator>(vm, "MapIterator")?;

        let mut value = Value::UNDEFINED;
        let mut done = false;
        iterator.next(vm, &mut done, &mut value)?;

        let realm = vm.current_realm().expect("a builtin runs in a realm");
        Ok(Value::from_object(create_iterator_result_object(
            vm, realm, value, done,
        )))
    }
}
