/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The maplike declarations of the interfaces implemented in Rust, https://webidl.spec.whatwg.org/#idl-maplike

use ak::Utf16String;
use libjs_rust::api::{
    Attribute, ErrorKind, ErrorType, Gc, Map, MapIterator, Object, PropertyAttributes, PropertyKey, PropertyKind,
    RawNativeFunctionPointer, Realm, ThrowCompletionOr, Value, Vm, call,
};

use crate::bindings::CallContext;
use crate::bindings::conversions::{ToJs, to_dom_string};
use crate::bindings::interface::{Interface, define_attribute, define_operation, this_implementation};

/// What the implementation of an interface with a maplike declaration, whose keys are strings, implements.
pub(crate) trait Maplike: Interface {
    // https://webidl.spec.whatwg.org/#dfn-map-entries
    fn map_entries(&self, cx: &CallContext) -> Gc<Map>;

    /// The value of the entry of the key, converted to a JS value, if there is one.
    fn map_value(&self, cx: &CallContext, key: &Utf16String) -> Option<Value> {
        let key = key.clone().to_js(cx).expect("a string converts to a JS value");
        self.map_entries(cx).map_get(key)
    }
}

/// What the implementation of an interface with a read-write maplike declaration implements, for the operations that
/// the interface does not declare itself.
pub(crate) trait WritableMaplike: Maplike {
    /// Sets the entry of the key to a value that the bindings converted to the value type.
    fn map_set(&self, cx: &CallContext, key: Utf16String, value: Value);
    fn map_remove(&self, cx: &CallContext, key: &Utf16String) -> bool;
    fn map_clear(&self, cx: &CallContext);
}

/// What the generated bindings of a maplike declaration with operations of the bindings define.
pub(crate) trait MaplikeDeclaration: Maplike {
    /// Throws unless the JS value converts to the value type of the declaration.
    fn check_value(vm: &Vm, value: Value) -> ThrowCompletionOr<()>;
}

/// Defines the properties that every maplike declaration has, https://webidl.spec.whatwg.org/#es-maplike
#[expect(clippy::too_many_arguments, reason = "these are the functions of the declaration")]
pub(crate) fn define_maplike_properties(
    vm: &Vm,
    realm: Gc<Realm>,
    prototype: Gc<Object>,
    size: RawNativeFunctionPointer,
    entries: RawNativeFunctionPointer,
    keys: RawNativeFunctionPointer,
    values: RawNativeFunctionPointer,
    for_each: RawNativeFunctionPointer,
    get: RawNativeFunctionPointer,
    has: RawNativeFunctionPointer,
) {
    define_attribute(vm, realm, prototype, "size", size, None);
    define_operation(vm, realm, prototype, "entries", entries, 0);
    prototype.define_direct_property(
        vm,
        &PropertyKey::from(vm.well_known_symbols().iterator),
        prototype.get_without_side_effects(vm, &vm.names.entries),
        PropertyAttributes::new(Attribute::CONFIGURABLE | Attribute::WRITABLE),
    );
    define_operation(vm, realm, prototype, "keys", keys, 0);
    define_operation(vm, realm, prototype, "values", values, 0);
    define_operation(vm, realm, prototype, "forEach", for_each, 1);
    define_operation(vm, realm, prototype, "get", get, 1);
    define_operation(vm, realm, prototype, "has", has, 1);
}

/// The call context of the realm that a collection of a wrapper world lives in.
pub(crate) fn context_of_map<'vm>(cx: &CallContext<'vm>, map: Gc<Map>) -> CallContext<'vm> {
    CallContext::new(cx.vm(), map.upcast::<Object>().shape().realm())
}

// https://webidl.spec.whatwg.org/#js-map-size
pub(crate) fn map_size<T: Maplike>(vm: &Vm) -> ThrowCompletionOr<Value> {
    // 1. Let O be the this value, implementation-checked against A with identifier "size" and type "getter".
    let (_, implementation, cx) = this_implementation::<T>(vm)?;

    // 2. Let map be the map entries of the IDL value that represents a reference to O.
    let map = implementation.map_entries(&cx);

    // 3. Return map's size, converted to a JavaScript value.
    Ok(Value::from_f64(map.map_size() as f64))
}

fn create_map_iterator<T: Maplike>(vm: &Vm, kind: PropertyKind) -> ThrowCompletionOr<Value> {
    let (_, implementation, cx) = this_implementation::<T>(vm)?;
    let map = implementation.map_entries(&cx);
    Ok(Value::from_object(
        MapIterator::create(vm, cx.realm(), map, kind).upcast::<Object>(),
    ))
}

// https://webidl.spec.whatwg.org/#js-map-entries
pub(crate) fn map_iterator_entries<T: Maplike>(vm: &Vm) -> ThrowCompletionOr<Value> {
    // 1. Let O be the this value, implementation-checked against A with identifier "entries" and type "method".
    // 2. Let map be the map entries of the IDL value that represents a reference to O.
    // 3. Return the result of creating a map iterator from map with kind "key+value".
    create_map_iterator::<T>(vm, PropertyKind::KeyAndValue)
}

// https://webidl.spec.whatwg.org/#js-map-keys
pub(crate) fn map_iterator_keys<T: Maplike>(vm: &Vm) -> ThrowCompletionOr<Value> {
    // 1. Let O be the this value, implementation-checked against A with identifier "keys" and type "method".
    // 2. Let map be the map entries of the IDL value that represents a reference to O.
    // 3. Return the result of creating a map iterator from map with kind "key".
    create_map_iterator::<T>(vm, PropertyKind::Key)
}

// https://webidl.spec.whatwg.org/#js-map-values
pub(crate) fn map_iterator_values<T: Maplike>(vm: &Vm) -> ThrowCompletionOr<Value> {
    // 1. Let O be the this value, implementation-checked against A with identifier "values" and type "method".
    // 2. Let map be the map entries of the IDL value that represents a reference to O.
    // 3. Return the result of creating a map iterator from map with kind "value".
    create_map_iterator::<T>(vm, PropertyKind::Value)
}

// https://webidl.spec.whatwg.org/#js-map-forEach
pub(crate) fn map_for_each<T: Maplike>(vm: &Vm) -> ThrowCompletionOr<Value> {
    // 1. Let O be the this value, implementation-checked against A with identifier "forEach" and type "method".
    let (wrapper, implementation, cx) = this_implementation::<T>(vm)?;

    // 2. Let map be the map entries of the IDL value that represents a reference to O.
    let map = implementation.map_entries(&cx);

    // 3. Let callbackFn be the first argument passed to the function, or undefined if not supplied.
    // 4. If IsCallable(callbackFn) is false, throw a TypeError.
    let callback = vm.argument(0);
    if !callback.is_function() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&callback]);
    }

    // 5. Let thisArg be the second argument passed to the function, or undefined if not supplied.
    let this_arg = vm.argument(1);

    // 6. For each key → value of map:
    let iterator = map.begin();
    while !iterator.is_end() {
        // 1. Let jsKey be key converted to a JavaScript value.
        // 2. Let jsValue be value converted to a JavaScript value.
        let entry = iterator.current();

        // 3. Perform ? Call(callbackFn, thisArg, « jsValue, jsKey, O »).
        call(
            vm,
            callback,
            this_arg,
            &[entry.value, entry.key, Value::from_object(wrapper)],
        )?;
        iterator.advance();
    }

    // 7. Return undefined.
    Ok(Value::UNDEFINED)
}

/// The map entries of the this value, and the key argument converted to the key type and back to a JS value.
fn entries_and_key<T: Maplike>(vm: &Vm) -> ThrowCompletionOr<(Gc<Map>, Value)> {
    let (_, implementation, cx) = this_implementation::<T>(vm)?;
    let map = implementation.map_entries(&cx);
    let key = to_dom_string(vm, vm.argument(0))?;
    Ok((map, key.to_js(&cx)?))
}

// https://webidl.spec.whatwg.org/#js-map-get
pub(crate) fn map_get<T: Maplike>(vm: &Vm) -> ThrowCompletionOr<Value> {
    // 1. Let O be the this value, implementation-checked against A with identifier "get" and type "method".
    let (_, implementation, cx) = this_implementation::<T>(vm)?;

    // 3. Let keyType be the key type specified in the maplike declaration.
    // 4. Let keyArg be the first argument passed to this function, or undefined if not supplied.
    // 5. Let key be keyArg converted to an IDL value of type keyType.
    let key = to_dom_string(vm, vm.argument(0))?;

    // 7. If map[key] exists, then return map[key], converted to a JavaScript value.
    // 8. Return undefined.
    Ok(implementation.map_value(&cx, &key).unwrap_or(Value::UNDEFINED))
}

// https://webidl.spec.whatwg.org/#js-map-has
pub(crate) fn map_has<T: Maplike>(vm: &Vm) -> ThrowCompletionOr<Value> {
    // 1. Let O be the this value, implementation-checked against A with identifier "has" and type "method".
    // 2. Let map be the map entries of the IDL value that represents a reference to O.
    // 3. Let keyType be the key type specified in the maplike declaration.
    // 4. Let keyArg be the first argument passed to this function, or undefined if not supplied.
    // 5. Let key be keyArg converted to an IDL value of type keyType.
    let (map, key) = entries_and_key::<T>(vm)?;

    // 7. If map[key] exists, then return true, otherwise return false.
    Ok(Value::from_bool(map.map_has(key)))
}

// https://webidl.spec.whatwg.org/#js-map-set
pub(crate) fn map_set<T: MaplikeDeclaration + WritableMaplike>(vm: &Vm) -> ThrowCompletionOr<Value> {
    // 1. Let O be the this value, implementation-checked against A with identifier "set" and type "method".
    let (wrapper, implementation, cx) = this_implementation::<T>(vm)?;

    // 3. Let keyType be the key type specified in the maplike declaration, and valueType be the value type.
    // 4. Let keyArg be the first argument passed to this function, or undefined if not supplied.
    // 5. Let key be keyArg converted to an IDL value of type keyType.
    let key = to_dom_string(vm, vm.argument(0))?;

    // 7. Let valueArg be the second argument passed to this function, or undefined if not supplied.
    // 8. Let value be valueArg converted to an IDL value of type valueType.
    let value = vm.argument(1);
    T::check_value(vm, value)?;

    // 9. Set map[key] to value.
    implementation.map_set(&cx, key, value);

    // 10. Return O.
    Ok(Value::from_object(wrapper))
}

// https://webidl.spec.whatwg.org/#js-map-delete
pub(crate) fn map_delete<T: WritableMaplike>(vm: &Vm) -> ThrowCompletionOr<Value> {
    // 1. Let O be the this value, implementation-checked against A with identifier "delete" and type "method".
    let (_, implementation, cx) = this_implementation::<T>(vm)?;

    // 3. Let keyType be the key type specified in the maplike declaration.
    // 4. Let keyArg be the first argument passed to this function, or undefined if not supplied.
    // 5. Let key be keyArg converted to an IDL value of type keyType.
    let key = to_dom_string(vm, vm.argument(0))?;

    // 7. Let retVal be true if map[key] exists, or else false.
    // 8. Remove map[key].
    // 9. Return retVal.
    Ok(Value::from_bool(implementation.map_remove(&cx, &key)))
}

// https://webidl.spec.whatwg.org/#js-map-clear
pub(crate) fn map_clear<T: WritableMaplike>(vm: &Vm) -> ThrowCompletionOr<Value> {
    // 1. Let O be the this value, implementation-checked against A with identifier "clear" and type "method".
    let (_, implementation, cx) = this_implementation::<T>(vm)?;

    // 3. Clear map.
    // NOTE: The map is preserved because there may be existing iterators, currently suspended, iterating over it.
    implementation.map_clear(&cx);

    // 4. Return undefined.
    Ok(Value::UNDEFINED)
}
