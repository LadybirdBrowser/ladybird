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
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;

/// %AsyncIteratorPrototype%.
#[repr(C)]
#[derive(Trace)]
pub struct AsyncIteratorPrototype {
    base: Object,
}

define_object_class!(AsyncIteratorPrototype, extends: [Object], methods: {
    initialize: AsyncIteratorPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

impl AsyncIteratorPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<AsyncIteratorPrototype> {
        realm.create_object(
            vm,
            AsyncIteratorPrototype {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.object_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &PropertyKey::from(vm.well_known_symbols().async_iterator),
            raw_native!(AsyncIteratorPrototype::symbol_async_iterator),
            0,
            attr,
            None,
        );
    }

    // 27.1.3.1 %AsyncIteratorPrototype% [ @@asyncIterator ] ( ), https://tc39.es/ecma262/#sec-asynciteratorprototype-asynciterator
    #[allow(clippy::unnecessary_wraps, reason = "native functions return a completion")]
    fn symbol_async_iterator(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Return the this value.
        Ok(vm.this_value())
    }
}
