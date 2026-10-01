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
use crate::layout::value::Value;
use crate::runtime::array::{ARRAY_OBJECT_METHODS, Array};
use crate::runtime::completion::Must;
use crate::runtime::object::define_object_class;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;

/// %Array.prototype%, which is an Array exotic object.
#[repr(C)]
#[derive(Trace)]
pub struct ArrayPrototype {
    base: Array,
}

define_object_class!(ArrayPrototype, extends: [Array, Object], methods: {
    initialize: ArrayPrototype::initialize,
    ..ARRAY_OBJECT_METHODS
});

impl ArrayPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<ArrayPrototype> {
        realm.create_object(
            vm,
            ArrayPrototype {
                base: Array::new_with_class(vm, Self::CLASS, realm, realm.object_prototype()),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;

        // NB: The Array.prototype methods come with the Array builtins, and so does Array.prototype [ @@iterator ],
        //     which is %Array.prototype.values% and comes before @@unscopables.

        // 23.1.3.41 Array.prototype [ @@unscopables ], https://tc39.es/ecma262/#sec-array.prototype-@@unscopables
        let unscopable_list = Object::create(vm, realm, None);
        for name in [
            &names.at,
            &names.copyWithin,
            &names.entries,
            &names.fill,
            &names.find,
            &names.findIndex,
            &names.findLast,
            &names.findLastIndex,
            &names.flat,
            &names.flatMap,
            &names.includes,
            &names.keys,
            &names.toReversed,
            &names.toSorted,
            &names.toSpliced,
            &names.values,
        ] {
            unscopable_list
                .create_data_property_or_throw(vm, name, Value::TRUE)
                .must();
        }

        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().unscopables),
            Value::from_object(unscopable_list),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }
}
