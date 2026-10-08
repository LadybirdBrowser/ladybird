/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::call;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::keyed_collections::canonicalize_keyed_collection_key;
use crate::runtime::map::Map;
use crate::runtime::map_iterator::MapIterator;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{
    MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, PropertyKind, define_object_class,
};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::typed_this_object;
use crate::runtime::realm::Realm;

/// %Map.prototype%.
#[repr(C)]
#[derive(Trace)]
pub struct MapPrototype {
    base: Object,
    entries_function: Cell<Option<Gc<FunctionObject>>>,
}

define_object_class!(MapPrototype, extends: [Object], methods: {
    initialize: MapPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

fn typed_this_map(vm: &Vm) -> ThrowCompletionOr<Gc<Map>> {
    typed_this_object::<Map>(vm, "Map")
}

impl MapPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<MapPrototype> {
        realm.create_object(
            vm,
            MapPrototype {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.object_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
                entries_function: Cell::new(None),
            },
        )
    }

    /// The original Map.prototype.entries, which is also the original Map.prototype[%Symbol.iterator%].
    pub fn entries_function(&self) -> Gc<FunctionObject> {
        self.entries_function
            .get()
            .expect("%Map.prototype% defines entries when it is initialized")
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;
        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        let define_native_function = |name: &PropertyKey, function, length| {
            object.define_native_function(vm, realm, name, function, length, attr, None);
        };

        define_native_function(&names.clear, raw_native!(MapPrototype::clear), 0);
        define_native_function(&names.delete_, raw_native!(MapPrototype::delete_), 1);
        define_native_function(&names.entries, raw_native!(MapPrototype::entries), 0);
        let entries_function = object.get_without_side_effects(vm, &names.entries).as_function();
        object
            .as_gc()
            .downcast::<MapPrototype>()
            .expect("initializing %Map.prototype%")
            .entries_function
            .set(Some(entries_function));
        define_native_function(&names.forEach, raw_native!(MapPrototype::for_each), 1);
        define_native_function(&names.get, raw_native!(MapPrototype::get), 1);
        define_native_function(&names.getOrInsert, raw_native!(MapPrototype::get_or_insert), 2);
        define_native_function(
            &names.getOrInsertComputed,
            raw_native!(MapPrototype::get_or_insert_computed),
            2,
        );
        define_native_function(&names.has, raw_native!(MapPrototype::has), 1);
        define_native_function(&names.keys, raw_native!(MapPrototype::keys), 0);
        define_native_function(&names.set, raw_native!(MapPrototype::set), 2);
        define_native_function(&names.values, raw_native!(MapPrototype::values), 0);

        object.define_native_accessor(
            vm,
            realm,
            &names.size,
            raw_native!(MapPrototype::size_getter),
            None,
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().iterator),
            Value::from_object(entries_function),
            attr,
        );
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_fly_string(vm, names.Map.as_string())),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 24.1.3.1 Map.prototype.clear ( ), https://tc39.es/ecma262/#sec-map.prototype.clear
    fn clear(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let M be the this value.
        // 2. Perform ? RequireInternalSlot(M, [[MapData]]).
        let map = typed_this_map(vm)?;

        // 3. For each Record { [[Key]], [[Value]] } p of M.[[MapData]], do
        //     a. Set p.[[Key]] to empty.
        //     b. Set p.[[Value]] to empty.
        map.map_clear(vm);

        // 4. Return undefined.
        Ok(Value::UNDEFINED)
    }

    // 24.1.3.3 Map.prototype.delete ( key ), https://tc39.es/ecma262/#sec-map.prototype.delete
    fn delete_(vm: &Vm) -> ThrowCompletionOr<Value> {
        let key = vm.argument(0);

        // 1. Let M be the this value.
        // 2. Perform ? RequireInternalSlot(M, [[MapData]]).
        let map = typed_this_map(vm)?;

        // 3. Set key to CanonicalizeKeyedCollectionKey(key).
        let key = canonicalize_keyed_collection_key(key);

        // 3. For each Record { [[Key]], [[Value]] } p of M.[[MapData]], do
        //     a. If p.[[Key]] is not empty and SameValue(p.[[Key]], key) is true, then
        //         i. Set p.[[Key]] to empty.
        //         ii. Set p.[[Value]] to empty.
        //         iii. Return true.
        // 4. Return false.
        Ok(Value::from_bool(map.map_remove(vm, key)))
    }

    // 24.1.3.4 Map.prototype.entries ( ), https://tc39.es/ecma262/#sec-map.prototype.entries
    fn entries(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        // 1. Let M be the this value.
        let map = typed_this_map(vm)?;

        // 2. Return ? CreateMapIterator(M, key+value).
        Ok(Value::from_object(MapIterator::create(
            vm,
            realm,
            map,
            PropertyKind::KeyAndValue,
        )))
    }

    // 24.1.3.5 Map.prototype.forEach ( callbackfn [ , thisArg ] ), https://tc39.es/ecma262/#sec-map.prototype.foreach
    fn for_each(vm: &Vm) -> ThrowCompletionOr<Value> {
        let callbackfn = vm.argument(0);
        let this_arg = vm.argument(1);

        // 1. Let M be the this value.
        // 2. Perform ? RequireInternalSlot(M, [[MapData]]).
        let map = typed_this_map(vm)?;

        // 3. If IsCallable(callbackfn) is false, throw a TypeError exception.
        if !callbackfn.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&callbackfn]);
        }

        // 4. Let entries be M.[[MapData]].
        // 5. Let numEntries be the number of elements in entries.
        // 6. Let index be 0.
        // 7. Repeat, while index < numEntries,
        let iterator = map.begin();
        while !iterator.is_end() {
            let entry = iterator.current();

            // i. Let e be entries[index].
            // b. Set index to index + 1.
            // c. If e.[[Key]] is not empty, then
            // NOTE: This is handled by Map's IteratorImpl.

            // i. Perform ? Call(callbackfn, thisArg, « e.[[Value]], e.[[Key]], M »).
            call(
                vm,
                callbackfn,
                this_arg,
                &[entry.value, entry.key, Value::from_object(map)],
            )?;

            // ii. NOTE: The number of elements in entries may have increased during execution of callbackfn.
            // iii. Set numEntries to the number of elements in entries.
            iterator.advance();
        }

        // 8. Return undefined.
        Ok(Value::UNDEFINED)
    }

    // 24.1.3.6 Map.prototype.get ( key ), https://tc39.es/ecma262/#sec-map.prototype.get
    fn get(vm: &Vm) -> ThrowCompletionOr<Value> {
        let key = vm.argument(0);

        // 1. Let M be the this value.
        // 2. Perform ? RequireInternalSlot(M, [[MapData]]).
        let map = typed_this_map(vm)?;

        // 3. Set key to CanonicalizeKeyedCollectionKey(key).
        let key = canonicalize_keyed_collection_key(key);

        // 3. For each Record { [[Key]], [[Value]] } p of M.[[MapData]], do
        //    a. If p.[[Key]] is not empty and SameValue(p.[[Key]], key) is true, return p.[[Value]].
        if let Some(result) = map.map_get(key) {
            return Ok(result);
        }

        // 4. Return undefined.
        Ok(Value::UNDEFINED)
    }

    // 1 Map.prototype.getOrInsert ( key, value ), https://tc39.es/proposal-upsert/#sec-map.prototype.getOrInsert
    fn get_or_insert(vm: &Vm) -> ThrowCompletionOr<Value> {
        let key = vm.argument(0);
        let value = vm.argument(1);

        // 1. Let M be the this value.
        // 2. Perform ? RequireInternalSlot(M, [[MapData]]).
        let map = typed_this_map(vm)?;

        // 3. Set key to CanonicalizeKeyedCollectionKey(key).
        let key = canonicalize_keyed_collection_key(key);

        // 4. For each Record { [[Key]], [[Value]] } p of M.[[MapData]], do
        if let Some(result) = map.map_get(key) {
            // a. If p.[[Key]] is not empty and SameValue(p.[[Key]], key) is true, return p.[[Value]].
            return Ok(result);
        }

        // 5. Let p be the Record { [[Key]]: key, [[Value]]: value }.
        // 6. Append p to M.[[MapData]].
        map.map_set(vm, key, value);

        // 7. Return value.
        Ok(value)
    }

    // 2 Map.prototype.getOrInsertComputed ( key, callback ), https://tc39.es/proposal-upsert/#sec-map.prototype.getOrInsertComputed
    fn get_or_insert_computed(vm: &Vm) -> ThrowCompletionOr<Value> {
        let key = vm.argument(0);
        let callback = vm.argument(1);

        // 1. Let M be the this value.
        // 2. Perform ? RequireInternalSlot(M, [[MapData]]).
        let map = typed_this_map(vm)?;

        // 3. If IsCallable(callback) is false, throw a TypeError exception.
        if !callback.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&callback]);
        }

        // 4. Set key to CanonicalizeKeyedCollectionKey(key).
        let key = canonicalize_keyed_collection_key(key);

        // 5. For each Record { [[Key]], [[Value]] } p of M.[[MapData]], do
        if let Some(result) = map.map_get(key) {
            // a. If p.[[Key]] is not empty and SameValue(p.[[Key]], key) is true, return p.[[Value]].
            return Ok(result);
        }

        // 6. Let value be ? Call(callback, undefined, « key »).
        let value = call(vm, callback, Value::UNDEFINED, &[key])?;

        // 7. NOTE: The Map may have been modified during execution of callback.

        // 8. For each Record { [[Key]], [[Value]] } p of M.[[MapData]], do
        //     a. If p.[[Key]] is not empty and SameValue(p.[[Key]], key) is true, then
        //         i. Set p.[[Value]] to value.
        //         ii. Return value.
        // 9. Let p be the Record { [[Key]]: key, [[Value]]: value }.
        // 10. Append p to M.[[MapData]].
        map.map_set(vm, key, value);

        // 11. Return value.
        Ok(value)
    }

    // 24.1.3.7 Map.prototype.has ( key ), https://tc39.es/ecma262/#sec-map.prototype.has
    fn has(vm: &Vm) -> ThrowCompletionOr<Value> {
        let key = vm.argument(0);

        // 1. Let M be the this value.
        // 2. Perform ? RequireInternalSlot(M, [[MapData]]).
        let map = typed_this_map(vm)?;

        // 3. Set key to CanonicalizeKeyedCollectionKey(key).
        let key = canonicalize_keyed_collection_key(key);

        // 3. For each Record { [[Key]], [[Value]] } p of M.[[MapData]], do
        //    a. If p.[[Key]] is not empty and SameValue(p.[[Key]], key) is true, return true.
        // 4. Return false.
        Ok(Value::from_bool(map.map_has(key)))
    }

    // 24.1.3.8 Map.prototype.keys ( ), https://tc39.es/ecma262/#sec-map.prototype.keys
    fn keys(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        // 1. Let M be the this value.
        let map = typed_this_map(vm)?;

        // 2. Return ? CreateMapIterator(M, key).
        Ok(Value::from_object(MapIterator::create(
            vm,
            realm,
            map,
            PropertyKind::Key,
        )))
    }

    // 24.1.3.9 Map.prototype.set ( key, value ), https://tc39.es/ecma262/#sec-map.prototype.set
    fn set(vm: &Vm) -> ThrowCompletionOr<Value> {
        let key = vm.argument(0);
        let value = vm.argument(1);

        // 1. Let M be the this value.
        // 2. Perform ? RequireInternalSlot(M, [[MapData]]).
        let map = typed_this_map(vm)?;

        // 3. Set key to CanonicalizeKeyedCollectionKey(key).
        let key = canonicalize_keyed_collection_key(key);

        // 4. For each Record { [[Key]], [[Value]] } p of M.[[MapData]], do
        //     a. If p.[[Key]] is not empty and SameValue(p.[[Key]], key) is true, then
        //         i. Set p.[[Value]] to value.
        //         ii. Return M.
        // 5. Let p be the Record { [[Key]]: key, [[Value]]: value }.
        // 6. Append p to M.[[MapData]].
        map.map_set(vm, key, value);

        // 7. Return M.
        Ok(Value::from_object(map))
    }

    // 24.1.3.10 get Map.prototype.size, https://tc39.es/ecma262/#sec-get-map.prototype.size
    fn size_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let M be the this value.
        // 2. Perform ? RequireInternalSlot(M, [[MapData]]).
        let map = typed_this_map(vm)?;

        // 3. Let count be 0.
        // 4. For each Record { [[Key]], [[Value]] } p of M.[[MapData]], do
        //    a. If p.[[Key]] is not empty, set count to count + 1.
        let count = map.map_size();

        // 5. Return 𝔽(count).
        Ok(Value::from_f64(count as f64))
    }

    // 24.1.3.11 Map.prototype.values ( ), https://tc39.es/ecma262/#sec-map.prototype.values
    fn values(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        // 1. Let M be the this value.
        let map = typed_this_map(vm)?;

        // 2. Return ? CreateMapIterator(M, value).
        Ok(Value::from_object(MapIterator::create(
            vm,
            realm,
            map,
            PropertyKind::Value,
        )))
    }
}
