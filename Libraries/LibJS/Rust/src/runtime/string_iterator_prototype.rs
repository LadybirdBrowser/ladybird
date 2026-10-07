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
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::typed_this_value;
use crate::runtime::realm::Realm;
use crate::runtime::string_iterator::StringIterator;

/// %StringIteratorPrototype%.
#[repr(C)]
#[derive(Trace)]
pub struct StringIteratorPrototype {
    base: Object,
}

define_object_class!(StringIteratorPrototype, extends: [Object], methods: {
    initialize: StringIteratorPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

impl StringIteratorPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<StringIteratorPrototype> {
        realm.create_object(
            vm,
            StringIteratorPrototype {
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
            raw_native!(StringIteratorPrototype::next),
            0,
            PropertyAttributes::new(Attribute::CONFIGURABLE | Attribute::WRITABLE),
            Some(Builtin::StringIteratorPrototypeNext),
        );

        // 22.1.5.1.2 %StringIteratorPrototype% [ @@toStringTag ], https://tc39.es/ecma262/#sec-%stringiteratorprototype%-@@tostringtag
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_fly_string(
                vm,
                &Utf16FlyString::from_utf8("String Iterator"),
            )),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 22.1.5.1.1 %StringIteratorPrototype%.next ( ), https://tc39.es/ecma262/#sec-%stringiteratorprototype%.next
    fn next(vm: &Vm) -> ThrowCompletionOr<Value> {
        let iterator = typed_this_value::<StringIterator>(vm, "StringIterator")?;

        let mut value = Value::UNDEFINED;
        let mut done = false;
        iterator.next(vm, &mut done, &mut value)?;

        let realm = vm.current_realm().expect("a native function runs in a realm");
        Ok(Value::from_object(create_iterator_result_object(
            vm, realm, value, done,
        )))
    }
}
