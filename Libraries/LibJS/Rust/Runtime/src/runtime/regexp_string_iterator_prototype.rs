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

/// %RegExpStringIteratorPrototype%.
#[repr(C)]
#[derive(Trace)]
pub struct RegExpStringIteratorPrototype {
    base: Object,
}

define_object_class!(RegExpStringIteratorPrototype, extends: [Object], methods: {
    initialize: RegExpStringIteratorPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

impl RegExpStringIteratorPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<RegExpStringIteratorPrototype> {
        realm.create_object(
            vm,
            RegExpStringIteratorPrototype {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.intrinsics().iterator_prototype(vm),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, _realm: Gc<Realm>) {
        // NB: %RegExpStringIteratorPrototype%.next comes with the RegExp builtins.

        // 22.2.9.2.2 %RegExpStringIteratorPrototype% [ @@toStringTag ], https://tc39.es/ecma262/#sec-%regexpstringiteratorprototype%-@@tostringtag
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_fly_string(
                vm,
                &Utf16FlyString::from_utf8("RegExp String Iterator"),
            )),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }
}
