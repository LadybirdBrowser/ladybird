/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Maps and Sets.
//!
//! The functions here follow the contract object.rs states for the embedding module. A Map or Set crosses as its
//! JSObject, and the functions that take one abort for any other object, as the C++ as<JS::Map>() does. Like the C++
//! Map and Set, the operations compare keys with SameValue, so -0 and +0 are different keys; only the JavaScript
//! methods turn a -0 key into +0.

#![allow(
    clippy::missing_safety_doc,
    reason = "object.rs states the contract every exported function shares"
)]

use core::ffi::c_void;

use crate::embedding::abi_types::{
    JSRealm, cell_from_abi, completion_from_abi, completion_into_abi, object_into_abi, throw_completion_into_abi,
    vm_from_abi,
};
use crate::layout::cell::Gc;
use crate::layout::host_class::{JSCompletion, JSObject, JSVM, JSValue};
use crate::layout::value::Value;
use crate::runtime::map::Map;
use crate::runtime::map_iterator::MapIterator;
use crate::runtime::object::PropertyKind;
use crate::runtime::set::Set;
use crate::runtime::set_iterator::SetIterator;

/// What a Map or Set iterator yields, or what EnumerableOwnProperties collects, in the order of the C++
/// JS::Object::PropertyKind: keys, values, or [key, value] arrays.
pub type JSPropertyKind = u8;

pub const JS_PROPERTY_KIND_KEY: JSPropertyKind = 0;
pub const JS_PROPERTY_KIND_VALUE: JSPropertyKind = 1;
pub const JS_PROPERTY_KIND_KEY_AND_VALUE: JSPropertyKind = 2;

/// Receives one entry of a Map with the context it came with. A throw completion stops the iteration, and the payload
/// of a normal one is ignored.
pub type JSMapEntryCallback =
    Option<unsafe extern "C" fn(context: *mut c_void, key: JSValue, value: JSValue) -> JSCompletion>;

/// Receives one value of a Set with the context it came with. A throw completion stops the iteration, and the payload
/// of a normal one is ignored.
pub type JSSetValueCallback = Option<unsafe extern "C" fn(context: *mut c_void, value: JSValue) -> JSCompletion>;

pub(crate) fn property_kind_from_abi(kind: JSPropertyKind) -> PropertyKind {
    match kind {
        JS_PROPERTY_KIND_KEY => PropertyKind::Key,
        JS_PROPERTY_KIND_VALUE => PropertyKind::Value,
        JS_PROPERTY_KIND_KEY_AND_VALUE => PropertyKind::KeyAndValue,
        kind => panic!("the embedder passed an unknown property kind {kind}"),
    }
}

/// # Safety
///
/// `map` must be a live Map.
unsafe fn map_from_abi(map: *mut JSObject) -> Gc<Map> {
    // SAFETY: The caller passes a live object.
    unsafe { cell_from_abi::<JSObject>(map) }
        .downcast::<Map>()
        .expect("the object is a Map")
}

/// # Safety
///
/// `set` must be a live Set.
unsafe fn set_from_abi(set: *mut JSObject) -> Gc<Set> {
    // SAFETY: The caller passes a live object.
    unsafe { cell_from_abi::<JSObject>(set) }
        .downcast::<Set>()
        .expect("the object is a Set")
}

// Maps

/// Map::create(realm): an empty Map whose prototype is the realm's %Map.prototype%. Returns an unrooted map. Main
/// thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_collections_map_create(vm: *mut JSVM, realm: *mut JSRealm) -> *mut JSObject {
    // SAFETY: See the module documentation.
    let (vm, realm) = unsafe { (vm_from_abi(vm), cell_from_abi::<JSRealm>(realm)) };
    object_into_abi(Map::create(vm, realm))
}

/// The number of entries of the map. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_collections_map_size(map: *mut JSObject) -> usize {
    // SAFETY: See the module documentation.
    unsafe { map_from_abi(map) }.map_size()
}

/// Sets the value of the key's entry, which a new key appends after the others. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_collections_map_set(map: *mut JSObject, key: JSValue, value: JSValue) {
    // SAFETY: See the module documentation.
    unsafe { map_from_abi(map) }.map_set(Value(key), Value(value));
}

/// Removes the key's entry, and returns whether there was one. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_collections_map_remove(map: *mut JSObject, key: JSValue) -> bool {
    // SAFETY: See the module documentation.
    unsafe { map_from_abi(map) }.map_remove(Value(key))
}

/// Removes every entry. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_collections_map_clear(map: *mut JSObject) {
    // SAFETY: See the module documentation.
    unsafe { map_from_abi(map) }.map_clear();
}

/// Calls the callback with the key and value of each entry in insertion order, the way Map.prototype.forEach visits
/// them: the callback may change the map, entries it adds are visited, and entries it removes before they are reached
/// are not. Returns the first throw completion of the callback, or a normal completion once every entry was visited.
/// Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_collections_map_for_each_entry(
    map: *mut JSObject,
    callback: JSMapEntryCallback,
    context: *mut c_void,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let map = unsafe { map_from_abi(map) };
    let callback = callback.expect("the embedder passes a callback");
    let iterator = map.begin();
    while !iterator.is_end() {
        let entry = iterator.current();
        // SAFETY: The embedder's callback takes the entry with the context it came with. The map's storage is not
        //         borrowed while it runs, as it may change the map or run JavaScript.
        if let Err(throw) = completion_from_abi(unsafe { callback(context, entry.key.0, entry.value.0) }) {
            return throw_completion_into_abi(throw);
        }
        iterator.advance();
    }
    completion_into_abi(Ok(()))
}

/// CreateMapIterator ( map, kind ) with kind one of JS_PROPERTY_KIND_*, an iterator of the realm's
/// %MapIteratorPrototype% that sees the map's entries as they are when it steps. Returns an unrooted iterator. Main
/// thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_collections_map_iterator_create(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    map: *mut JSObject,
    kind: JSPropertyKind,
) -> *mut JSObject {
    // SAFETY: See the module documentation.
    let (vm, realm, map) = unsafe { (vm_from_abi(vm), cell_from_abi::<JSRealm>(realm), map_from_abi(map)) };
    object_into_abi(MapIterator::create(vm, realm, map, property_kind_from_abi(kind)))
}

// Sets

/// Set::create(realm): an empty Set whose prototype is the realm's %Set.prototype%. Returns an unrooted set. Main
/// thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_collections_set_create(vm: *mut JSVM, realm: *mut JSRealm) -> *mut JSObject {
    // SAFETY: See the module documentation.
    let (vm, realm) = unsafe { (vm_from_abi(vm), cell_from_abi::<JSRealm>(realm)) };
    object_into_abi(Set::create(vm, realm))
}

/// The number of values of the set. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_collections_set_size(set: *mut JSObject) -> usize {
    // SAFETY: See the module documentation.
    unsafe { set_from_abi(set) }.set_size()
}

/// Appends the value unless the set has it already. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_collections_set_add(set: *mut JSObject, value: JSValue) {
    // SAFETY: See the module documentation.
    unsafe { set_from_abi(set) }.set_add(Value(value));
}

/// Removes the value, and returns whether the set had it. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_collections_set_remove(set: *mut JSObject, value: JSValue) -> bool {
    // SAFETY: See the module documentation.
    unsafe { set_from_abi(set) }.set_remove(Value(value))
}

/// Removes every value. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_collections_set_clear(set: *mut JSObject) {
    // SAFETY: See the module documentation.
    unsafe { set_from_abi(set) }.set_clear();
}

/// Calls the callback with each value in insertion order, the way Set.prototype.forEach visits them: the callback may
/// change the set, values it adds are visited, and values it removes before they are reached are not. Returns the
/// first throw completion of the callback, or a normal completion once every value was visited. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_collections_set_for_each_value(
    set: *mut JSObject,
    callback: JSSetValueCallback,
    context: *mut c_void,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let set = unsafe { set_from_abi(set) };
    let callback = callback.expect("the embedder passes a callback");
    let iterator = set.begin();
    while !iterator.is_end() {
        let value = iterator.current();
        // SAFETY: The embedder's callback takes the value with the context it came with. The set's storage is not
        //         borrowed while it runs, as it may change the set or run JavaScript.
        if let Err(throw) = completion_from_abi(unsafe { callback(context, value.0) }) {
            return throw_completion_into_abi(throw);
        }
        iterator.advance();
    }
    completion_into_abi(Ok(()))
}

/// CreateSetIterator ( set, kind ) with kind one of JS_PROPERTY_KIND_*, an iterator of the realm's
/// %SetIteratorPrototype% that sees the set's values as they are when it steps. Returns an unrooted iterator. Main
/// thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_collections_set_iterator_create(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    set: *mut JSObject,
    kind: JSPropertyKind,
) -> *mut JSObject {
    // SAFETY: See the module documentation.
    let (vm, realm, set) = unsafe { (vm_from_abi(vm), cell_from_abi::<JSRealm>(realm), set_from_abi(set)) };
    object_into_abi(SetIterator::create(vm, realm, set, property_kind_from_abi(kind)))
}
