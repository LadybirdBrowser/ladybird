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
use crate::runtime::abstract_operations::can_be_held_weakly;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::typed_this_object;
use crate::runtime::realm::Realm;
use crate::runtime::weak_set::WeakSet;

/// %WeakSet.prototype%.
#[repr(C)]
#[derive(Trace)]
pub struct WeakSetPrototype {
    base: Object,
}

define_object_class!(WeakSetPrototype, extends: [Object], methods: {
    initialize: WeakSetPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

fn typed_this_weak_set(vm: &Vm) -> ThrowCompletionOr<Gc<WeakSet>> {
    typed_this_object::<WeakSet>(vm, "WeakSet")
}

impl WeakSetPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<WeakSetPrototype> {
        realm.create_object(
            vm,
            WeakSetPrototype {
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
        let names = &vm.names;
        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        let define_native_function = |name: &PropertyKey, function, length| {
            object.define_native_function(vm, realm, name, function, length, attr, None);
        };

        define_native_function(&names.add, raw_native!(WeakSetPrototype::add), 1);
        define_native_function(&names.delete_, raw_native!(WeakSetPrototype::delete_), 1);
        define_native_function(&names.has, raw_native!(WeakSetPrototype::has), 1);

        // 24.4.3.5 WeakSet.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-weakset.prototype-@@tostringtag
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_fly_string(vm, names.WeakSet.as_string())),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 24.4.3.1 WeakSet.prototype.add ( value ), https://tc39.es/ecma262/#sec-weakset.prototype.add
    fn add(vm: &Vm) -> ThrowCompletionOr<Value> {
        let value = vm.argument(0);

        // 1. Let S be the this value.
        // 2. Perform ? RequireInternalSlot(S, [[WeakSetData]]).
        let weak_set = typed_this_weak_set(vm)?;

        // 3. If CanBeHeldWeakly(value) is false, throw a TypeError exception.
        if !can_be_held_weakly(value) {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::CannotBeHeldWeakly, &[&value]);
        }

        // 4. For each element e of S.[[WeakSetData]], do
        //     a. If e is not empty and SameValue(e, value) is true, then
        //         i. Return S.
        // 5. Append value to S.[[WeakSetData]].
        weak_set.weak_set_add(vm, value.as_cell());

        // 6. Return S.
        Ok(Value::from_object(weak_set))
    }

    // 24.4.3.3 WeakSet.prototype.delete ( value ), https://tc39.es/ecma262/#sec-weakset.prototype.delete
    fn delete_(vm: &Vm) -> ThrowCompletionOr<Value> {
        let value = vm.argument(0);

        // 1. Let S be the this value.
        // 2. Perform ? RequireInternalSlot(S, [[WeakSetData]]).
        let weak_set = typed_this_weak_set(vm)?;

        // 3. If CanBeHeldWeakly(value) is false, return false.
        if !can_be_held_weakly(value) {
            return Ok(Value::FALSE);
        }

        // 4. For each element e of S.[[WeakSetData]], do
        //     a. If e is not empty and SameValue(e, value) is true, then
        //         i. Replace the element of S.[[WeakSetData]] whose value is e with an element whose value is empty.
        //         ii. Return true.
        // 5. Return false.
        Ok(Value::from_bool(weak_set.weak_set_remove(value.as_cell())))
    }

    // 24.4.3.4 WeakSet.prototype.has ( value ), https://tc39.es/ecma262/#sec-weakset.prototype.has
    fn has(vm: &Vm) -> ThrowCompletionOr<Value> {
        let value = vm.argument(0);

        // 1. Let S be the this value.
        // 2. Perform ? RequireInternalSlot(S, [[WeakSetData]]).
        let weak_set = typed_this_weak_set(vm)?;

        // 3. If CanBeHeldWeakly(value) is false, return false.
        if !can_be_held_weakly(value) {
            return Ok(Value::FALSE);
        }

        // 4. For each element e of S.[[WeakSetData]], do
        //     a. If e is not empty and SameValue(e, value) is true, return true.
        if weak_set.weak_set_has(value.as_cell()) {
            return Ok(Value::TRUE);
        }

        // 5. Return false.
        Ok(Value::FALSE)
    }
}
