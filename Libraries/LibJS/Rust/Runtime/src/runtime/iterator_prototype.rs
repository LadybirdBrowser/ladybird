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

/// %Iterator.prototype%.
#[repr(C)]
#[derive(Trace)]
pub struct IteratorPrototype {
    base: Object,
}

define_object_class!(IteratorPrototype, extends: [Object], methods: {
    initialize: IteratorPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

impl IteratorPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<IteratorPrototype> {
        realm.create_object(
            vm,
            IteratorPrototype {
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
            &PropertyKey::from(vm.well_known_symbols().iterator),
            raw_native!(IteratorPrototype::symbol_iterator),
            0,
            attr,
            None,
        );
        // NB: The other Iterator.prototype methods, which follow @@iterator in this order: drop, every, filter, find,
        //     flatMap, forEach, map, reduce, some, take and toArray, and then the accessors of
        //     Iterator.prototype.constructor and Iterator.prototype [ @@toStringTag ], come with the Iterator builtins.
    }

    // 27.1.4.15 Iterator.prototype [ %Symbol.iterator% ] ( ), https://tc39.es/ecma262/#sec-iterator.prototype-%symbol.iterator%
    #[allow(clippy::unnecessary_wraps, reason = "native functions can throw")]
    fn symbol_iterator(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Return the this value.
        Ok(vm.this_value())
    }
}
