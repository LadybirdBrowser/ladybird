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
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::typed_this_object;
use crate::runtime::realm::Realm;
use crate::runtime::weak_ref::WeakRef;

/// %WeakRef.prototype%.
#[repr(C)]
#[derive(Trace)]
pub struct WeakRefPrototype {
    base: Object,
}

define_object_class!(WeakRefPrototype, extends: [Object], methods: {
    initialize: WeakRefPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

impl WeakRefPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<WeakRefPrototype> {
        realm.create_object(
            vm,
            WeakRefPrototype {
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
        object.define_native_function(
            vm,
            realm,
            &vm.names.deref,
            raw_native!(WeakRefPrototype::deref),
            0,
            PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE),
            None,
        );

        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_fly_string(
                vm,
                vm.names.WeakRef.as_string(),
            )),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 26.1.3.2 WeakRef.prototype.deref ( ), https://tc39.es/ecma262/#sec-weak-ref.prototype.deref
    fn deref(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let weakRef be the this value.
        // 2. Perform ? RequireInternalSlot(weakRef, [[WeakRefTarget]]).
        let weak_ref = typed_this_object::<WeakRef>(vm, "WeakRef")?;

        // 3. Return WeakRefDeref(weakRef).
        weak_ref.update_execution_generation(vm);
        let value = weak_ref.value();
        if value.is_empty() {
            return Ok(Value::UNDEFINED);
        }
        Ok(value)
    }
}
