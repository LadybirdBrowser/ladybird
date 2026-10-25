/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The runtime helpers of the intrinsics JIT code calls without a native frame (`libjs_jit::snapshot::IntrinsicHelpers`).
//! Each returns a value, or the empty value, having done nothing observable, for anything it does not handle; the
//! code then exits to the interpreter, which runs the instruction itself.

use ak::Utf16FlyString;

use crate::bytecode::executable::{KeyedPropertyLookup, KeyedPropertyLookupCache, PropertyLookupCacheEntryType};
use crate::gc::class::GcCell;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::array::Array;
use crate::runtime::module_namespace_object::ModuleNamespaceObject;
use crate::runtime::property_key::{PropertyKey, StringMayBeNumber};
use crate::runtime::proxy_object::ProxyObject;
use crate::runtime::realm_watchpoints;
use crate::runtime::string_object::StringObject;
use crate::runtime::typed_array::{ContentType, TypedArrayBase};
use crate::utf16::{has_fly_string_storage, to_utf16_fly_string};

/// The property key of a string or symbol, which converting runs no code for.
fn string_or_symbol_key(vm: &Vm, key: Value) -> Option<PropertyKey> {
    if !key.is_string() && !key.is_symbol() {
        return None;
    }
    key.to_property_key(vm).ok()
}

/// Whether looking up properties of the object runs no code and cannot throw. Proxies run traps, the bindings of
/// module namespaces may be uninitialized, and platform objects may check their embedder's rules.
fn has_ordinary_lookups(object: Gc<Object>) -> bool {
    // NB: Neither class has subclasses.
    let class = object.class();
    !core::ptr::eq(class, ProxyObject::CLASS)
        && !core::ptr::eq(class, ModuleNamespaceObject::CLASS)
        && !object.is_platform_object()
}

/// Whether looking up the property `key` of the object creates nothing.
fn lookup_creates_nothing(vm: &Vm, object: Gc<Object>, key: &PropertyKey) -> bool {
    if object.is_ecmascript_function_object()
        && (*key == vm.names.prototype || *key == vm.names.caller || *key == vm.names.arguments)
    {
        return false;
    }
    if key.is_number() {
        return !object.is::<StringObject>()
            && !object
                .downcast::<TypedArrayBase>()
                .is_some_and(|typed_array| typed_array.content_type() == ContentType::BigInt);
    }
    !key.is_string() || !object.has_intrinsic_accessors()
}

/// Whether looking up the property `key` creates nothing in the object or in its prototypes.
fn prototype_chain_lookup_creates_nothing(vm: &Vm, object: Gc<Object>, key: &PropertyKey) -> bool {
    let mut holder = Some(object);
    while let Some(current) = holder {
        if !lookup_creates_nothing(vm, current, key) {
            return false;
        }
        holder = current.prototype();
    }
    true
}

/// The fly string of a string key, if the string is stored as one already.
fn fly_string_key(key: Value) -> Option<Utf16FlyString> {
    if !key.is_string() {
        return None;
    }
    let string = key.as_string().utf16_string();
    has_fly_string_storage(&string).then(|| to_utf16_fly_string(&string))
}

/// Whether an object whose own string-keyed properties are those of its shape has the own property `name`, from the
/// VM's keyed property lookup cache, whose entries for an own property or its absence this remembers too, or from its
/// shape. Returns `None` for other objects and for array indices.
fn has_own_string_property(vm: &Vm, object: Gc<Object>, name: &Utf16FlyString) -> Option<bool> {
    if !object.own_string_keyed_properties_are_in_shape() {
        return None;
    }
    let shape = object.shape();
    let cache = vm.keyed_property_lookup_cache();
    let index = KeyedPropertyLookupCache::entry_index_for(shape, name);
    if let Some(entry) = cache.lookup(index, shape, name)
        && (!shape.is_dictionary() || shape.dictionary_generation() == entry.shape_dictionary_generation)
    {
        // NB: Only names that are no array indices have entries, since the shape does not hold indexed properties.
        match entry.entry_type {
            PropertyLookupCacheEntryType::GetOwnProperty => return Some(true),
            PropertyLookupCacheEntryType::GetPropertyInPrototypeChain
            | PropertyLookupCacheEntryType::GetMissingProperty
            | PropertyLookupCacheEntryType::MissingOwnProperty => return Some(false),
            _ => {}
        }
    }
    // NB: Array indices are indexed properties, which the shape does not hold.
    let key = PropertyKey::from_fly_string(name.clone(), StringMayBeNumber::Yes);
    if key.is_number() {
        return None;
    }
    let metadata = shape.lookup(&key);
    cache.set_entry(
        index,
        KeyedPropertyLookup {
            entry_type: if metadata.is_some() {
                PropertyLookupCacheEntryType::GetOwnProperty
            } else {
                PropertyLookupCacheEntryType::MissingOwnProperty
            },
            property_offset: metadata.map_or(0, |metadata| metadata.offset),
            shape_dictionary_generation: shape.dictionary_generation(),
            shape: Some(shape),
            prototype: None,
            prototype_chain_validity: None,
            new_shape: None,
        },
        name,
    );
    Some(metadata.is_some())
}

/// `Object.prototype.hasOwnProperty` called on `object` with `key`.
///
/// JIT code calls this with its VM.
pub extern "C" fn libjs_jit_has_own_property(vm: &Vm, object: u64, key: u64) -> u64 {
    let (object, key) = (Value::from_bits(object), Value::from_bits(key));
    if !object.is_object() || !has_ordinary_lookups(object.as_object()) {
        return Value::EMPTY.0;
    }
    if let Some(name) = fly_string_key(key)
        && let Some(result) = has_own_string_property(vm, object.as_object(), &name)
    {
        return Value::from_bool(result).0;
    }
    let Some(key) = string_or_symbol_key(vm, key).filter(|key| lookup_creates_nothing(vm, object.as_object(), key))
    else {
        return Value::EMPTY.0;
    };
    object
        .as_object()
        .has_own_property(vm, &key)
        .map_or(Value::EMPTY, Value::from_bool)
        .0
}

/// `key in object`.
///
/// JIT code calls this with its VM.
pub extern "C" fn libjs_jit_has_property(vm: &Vm, key: u64, object: u64) -> u64 {
    let (key_value, object) = (Value::from_bits(key), Value::from_bits(object));
    let index_key = key_value.is_int32() && key_value.as_i32() >= 0;
    if !object.is_object() || !(key_value.is_string() || key_value.is_symbol() || index_key) {
        return Value::EMPTY.0;
    }
    // NB: [[HasProperty]] of ordinary objects asks their prototypes, so every object of the chain must look up its
    //     properties without running code.
    let mut holder = Some(object.as_object());
    while let Some(current) = holder {
        if !has_ordinary_lookups(current) {
            return Value::EMPTY.0;
        }
        holder = current.prototype();
    }
    if index_key {
        let key = PropertyKey::from(key_value.as_i32().cast_unsigned());
        if !prototype_chain_lookup_creates_nothing(vm, object.as_object(), &key) {
            return Value::EMPTY.0;
        }
        return object
            .as_object()
            .has_property(vm, &key)
            .map_or(Value::EMPTY, Value::from_bool)
            .0;
    }
    let name = fly_string_key(key_value);
    let mut holder = Some(object.as_object());
    while let Some(current) = holder {
        match name
            .as_ref()
            .and_then(|name| has_own_string_property(vm, current, name))
        {
            Some(true) => return Value::TRUE.0,
            Some(false) => holder = current.prototype(),
            None => {
                let Some(key) = string_or_symbol_key(vm, key_value)
                    .filter(|key| prototype_chain_lookup_creates_nothing(vm, current, key))
                else {
                    return Value::EMPTY.0;
                };
                return current.has_property(vm, &key).map_or(Value::EMPTY, Value::from_bool).0;
            }
        }
    }
    Value::FALSE.0
}

/// `GetIterator(iterable, sync)` in fast array mode (see `get_iterator()` in the interpreter's slow paths): the
/// iterable, if it is an array whose iterator method is `%Array.prototype.values%` of the realm of JIT code and whose
/// iterators have the builtin `next` method, so that nothing can observe the iterator GetIterator would make. The
/// empty value otherwise.
///
/// JIT code calls this with its VM.
pub extern "C" fn libjs_jit_open_array_iteration(vm: &Vm, iterable: u64) -> u64 {
    let iterable = Value::from_bits(iterable);
    if !iterable.is_object() {
        return Value::EMPTY.0;
    }
    let array = iterable.as_object();
    if array.downcast::<Array>().is_none() {
        return Value::EMPTY.0;
    }
    let realm = vm.current_realm().expect("JIT code runs in a realm");
    if !realm_watchpoints::array_is_iterated_unobservably(vm, realm, &array) {
        return Value::EMPTY.0;
    }
    iterable.0
}
