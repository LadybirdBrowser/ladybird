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

/// %BigInt.prototype%.
#[repr(C)]
#[derive(Trace)]
pub struct BigIntPrototype {
    base: Object,
}

define_object_class!(BigIntPrototype, extends: [Object], methods: {
    initialize: BigIntPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

impl BigIntPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<BigIntPrototype> {
        realm.create_object(
            vm,
            BigIntPrototype {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.object_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, _realm: Gc<Realm>) {
        // NB: BigInt.prototype.toString, .toLocaleString and .valueOf come with the BigInt builtins.

        // 21.2.3.5 BigInt.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-bigint.prototype-@@tostringtag
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_fly_string(
                vm,
                &Utf16FlyString::from_utf8("BigInt"),
            )),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }
}
