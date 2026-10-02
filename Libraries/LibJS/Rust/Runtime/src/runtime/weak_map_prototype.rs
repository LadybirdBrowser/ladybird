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
use crate::runtime::abstract_operations::{call, can_be_held_weakly};
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
use crate::runtime::weak_map::WeakMap;

/// %WeakMap.prototype%.
#[repr(C)]
#[derive(Trace)]
pub struct WeakMapPrototype {
    base: Object,
}

define_object_class!(WeakMapPrototype, extends: [Object], methods: {
    initialize: WeakMapPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

fn typed_this_weak_map(vm: &Vm) -> ThrowCompletionOr<Gc<WeakMap>> {
    typed_this_object::<WeakMap>(vm, "WeakMap")
}

impl WeakMapPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<WeakMapPrototype> {
        realm.create_object(
            vm,
            WeakMapPrototype {
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

        define_native_function(&names.delete_, raw_native!(WeakMapPrototype::delete_), 1);
        define_native_function(&names.get, raw_native!(WeakMapPrototype::get), 1);
        define_native_function(&names.getOrInsert, raw_native!(WeakMapPrototype::get_or_insert), 2);
        define_native_function(
            &names.getOrInsertComputed,
            raw_native!(WeakMapPrototype::get_or_insert_computed),
            2,
        );
        define_native_function(&names.has, raw_native!(WeakMapPrototype::has), 1);
        define_native_function(&names.set, raw_native!(WeakMapPrototype::set), 2);

        // 24.3.3.6 WeakMap.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-weakmap.prototype-@@tostringtag
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_fly_string(vm, names.WeakMap.as_string())),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 24.3.3.2 WeakMap.prototype.delete ( key ), https://tc39.es/ecma262/#sec-weakmap.prototype.delete
    fn delete_(vm: &Vm) -> ThrowCompletionOr<Value> {
        let key = vm.argument(0);

        // 1. Let M be the this value.
        // 2. Perform ? RequireInternalSlot(M, [[WeakMapData]]).
        let weak_map = typed_this_weak_map(vm)?;

        // 3. If CanBeHeldWeakly(key) is false, return false.
        if !can_be_held_weakly(key) {
            return Ok(Value::FALSE);
        }

        // 4. For each Record { [[Key]], [[Value]] } p of M.[[WeakMapData]], do
        //     a. If p.[[Key]] is not empty and SameValue(p.[[Key]], key) is true, then
        //         i. Set p.[[Key]] to empty.
        //         ii. Set p.[[Value]] to empty.
        //         iii. Return true.
        // 5. Return false.
        Ok(Value::from_bool(weak_map.weak_map_remove(key.as_cell())))
    }

    // 24.3.3.3 WeakMap.prototype.get ( key ), https://tc39.es/ecma262/#sec-weakmap.prototype.get
    fn get(vm: &Vm) -> ThrowCompletionOr<Value> {
        let key = vm.argument(0);

        // 1. Let M be the this value.
        // 2. Perform ? RequireInternalSlot(M, [[WeakMapData]]).
        let weak_map = typed_this_weak_map(vm)?;

        // 3. If CanBeHeldWeakly(key) is false, return undefined.
        if !can_be_held_weakly(key) {
            return Ok(Value::UNDEFINED);
        }

        // 4. For each Record { [[Key]], [[Value]] } p of M.[[WeakMapData]], do
        //     a. If p.[[Key]] is not empty and SameValue(p.[[Key]], key) is true, return p.[[Value]].
        if let Some(result) = weak_map.weak_map_get(key.as_cell()) {
            return Ok(result);
        }

        // 5. Return undefined.
        Ok(Value::UNDEFINED)
    }

    // 3 WeakMap.prototype.getOrInsert ( key, value ), https://tc39.es/proposal-upsert/#sec-weakmap.prototype.getOrInsert
    fn get_or_insert(vm: &Vm) -> ThrowCompletionOr<Value> {
        let key = vm.argument(0);
        let value = vm.argument(1);

        // 1. Let M be the this value.
        // 2. Perform ? RequireInternalSlot(M, [[WeakMapData]]).
        let weak_map = typed_this_weak_map(vm)?;

        // 3. If CanBeHeldWeakly(key) is false, throw a TypeError exception.
        if !can_be_held_weakly(key) {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::CannotBeHeldWeakly, &[&key]);
        }

        // 4. For each Record { [[Key]], [[Value]] } p of M.[[WeakMapData]], do
        if let Some(result) = weak_map.weak_map_get(key.as_cell()) {
            // a. If p.[[Key]] is not empty and SameValue(p.[[Key]], key) is true, return p.[[Value]].
            return Ok(result);
        }

        // 5. Let p be the Record { [[Key]]: key, [[Value]]: value }.
        // 6. Append p to M.[[WeakMapData]].
        weak_map.weak_map_set(key.as_cell(), value);

        // 7. Return value.
        Ok(value)
    }

    // 4 WeakMap.prototype.getOrInsertComputed ( key, callback ), https://tc39.es/proposal-upsert/#sec-weakmap.prototype.getOrInsertComputed
    fn get_or_insert_computed(vm: &Vm) -> ThrowCompletionOr<Value> {
        let key = vm.argument(0);
        let callback = vm.argument(1);

        // 1. Let M be the this value.
        // 2. Perform ? RequireInternalSlot(M, [[WeakMapData]]).
        let weak_map = typed_this_weak_map(vm)?;

        // 3. If CanBeHeldWeakly(key) is false, throw a TypeError exception.
        if !can_be_held_weakly(key) {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::CannotBeHeldWeakly, &[&key]);
        }

        // 4. If IsCallable(callback) is false, throw a TypeError exception.
        if !callback.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&callback]);
        }

        // 5. For each Record { [[Key]], [[Value]] } p of M.[[WeakMapData]], do
        if let Some(result) = weak_map.weak_map_get(key.as_cell()) {
            // a. If p.[[Key]] is not empty and SameValue(p.[[Key]], key) is true, return p.[[Value]].
            return Ok(result);
        }

        // 6. Let value be ? Call(callback, undefined, « key »).
        let value = call(vm, callback, Value::UNDEFINED, &[key])?;

        // 7. NOTE: The WeakMap may have been modified during execution of callback.

        // 8. For each Record { [[Key]], [[Value]] } p of M.[[WeakMapData]], do
        //     a. If p.[[Key]] is not empty and SameValue(p.[[Key]], key) is true, then
        //         i. Set p.[[Value]] to value.
        //         ii. Return value.
        // 9. Let p be the Record { [[Key]]: key, [[Value]]: value }.
        // 10. Append p to M.[[WeakMapData]].
        weak_map.weak_map_set(key.as_cell(), value);

        // 11. Return value.
        Ok(value)
    }

    // 24.3.3.4 WeakMap.prototype.has ( key ), https://tc39.es/ecma262/#sec-weakmap.prototype.has
    fn has(vm: &Vm) -> ThrowCompletionOr<Value> {
        let key = vm.argument(0);

        // 1. Let M be the this value.
        // 2. Perform ? RequireInternalSlot(M, [[WeakMapData]]).
        let weak_map = typed_this_weak_map(vm)?;

        // 3. If CanBeHeldWeakly(key) is false, return false.
        if !can_be_held_weakly(key) {
            return Ok(Value::FALSE);
        }

        // 4. For each Record { [[Key]], [[Value]] } p of M.[[WeakMapData]], do
        //     a. If p.[[Key]] is not empty and SameValue(p.[[Key]], key) is true, return true.
        if weak_map.weak_map_has(key.as_cell()) {
            return Ok(Value::TRUE);
        }

        // 5. Return false.
        Ok(Value::FALSE)
    }

    // 24.3.3.5 WeakMap.prototype.set ( key, value ), https://tc39.es/ecma262/#sec-weakmap.prototype.set
    fn set(vm: &Vm) -> ThrowCompletionOr<Value> {
        let key = vm.argument(0);
        let value = vm.argument(1);

        // 1. Let M be the this value.
        // 2. Perform ? RequireInternalSlot(M, [[WeakMapData]]).
        let weak_map = typed_this_weak_map(vm)?;

        // 3. If CanBeHeldWeakly(key) is false, throw a TypeError exception.
        if !can_be_held_weakly(key) {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::CannotBeHeldWeakly, &[&key]);
        }

        // 4. For each Record { [[Key]], [[Value]] } p of M.[[WeakMapData]], do
        //    a. If p.[[Key]] is not empty and SameValue(p.[[Key]], key) is true, then
        //        i. Set p.[[Value]] to value.
        //        ii. Return M.
        // 5. Let p be the Record { [[Key]]: key, [[Value]]: value }.
        // 6. Append p to M.[[WeakMapData]].
        weak_map.weak_map_set(key.as_cell(), value);

        // 7. Return M.
        Ok(Value::from_object(weak_map))
    }
}
