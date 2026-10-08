/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The property gets and puts that consult and fill the inline caches the interpreter's fast paths read.

use ak::Utf16FlyString;
use libjs_abi::PutKind;

use crate::bytecode::executable::{
    KeyedPropertyLookupCache, KeyedPropertyLookupCacheEntry, PropertyLookupCache, PropertyLookupCacheEntryType,
};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::call_function_object;
use crate::runtime::class_field_definition::ClassElementName;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::ecmascript_function_object::as_ecmascript_function_object;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::object::{
    CacheableGetPropertyMetadata, CacheableGetPropertyMetadataType, CacheableSetPropertyMetadata,
    CacheableSetPropertyMetadataType, Object, PropertyLookupPhase,
};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::utf16::Utf16Display;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum GetByIdMode {
    Normal,
    Length,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CachePropertyAbsence {
    No,
    Yes,
}

/// Whether the code doing a property access is strict mode code.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Strict {
    No,
    Yes,
}

pub fn get_cached_property_value(vm: &Vm, value: Value, this_value: Value) -> ThrowCompletionOr<Value> {
    if !value.is_accessor() {
        return Ok(value);
    }

    // https://tc39.es/ecma262/#sec-ordinaryget
    // If _getter_ is *undefined*, return *undefined*.
    let Some(getter) = value.as_accessor().getter() else {
        return Ok(Value::UNDEFINED);
    };
    call_function_object(vm, getter, this_value, &[])
}

pub fn object_can_cache_property_additions(object: &Object) -> bool {
    !object.may_interfere_with_indexed_property_access() && !object.requires_slow_add_own_property()
}

pub fn property_addition_is_cacheable(vm: &Vm, object: &Object, property_key: &PropertyKey) -> bool {
    if !property_key.is_string() {
        return object_can_cache_property_additions(object);
    }
    object_can_cache_property_additions(object)
        && !(object.has_magical_length_property() && property_key.as_string() == vm.names.length.as_string())
}

/// Remembers a keyed lookup that the VM's keyed property lookup cache describes in the per-site cache of the
/// instruction that made it, keyed by the identity of the key Value (see PropertyLookupCacheEntry::key).
fn remember_keyed_lookup(site_cache: &PropertyLookupCache, site_cache_key: u64, entry: &KeyedPropertyLookupCacheEntry) {
    site_cache.update(entry.entry_type, |site_entry| {
        site_entry.key = site_cache_key;
        site_entry.property_offset = entry.property_offset;
        site_entry.shape_dictionary_generation = entry.shape_dictionary_generation;
        site_entry.shape = entry.shape;
        site_entry.prototype = entry.prototype;
        site_entry.prototype_chain_validity = entry.prototype_chain_validity;
    });
}

/// Gets a property by a key that is not known statically. Lookups of string keys are cached in the VM's keyed property
/// lookup cache, and also in `site_cache` under its key if one is given, so that the instruction can find them again
/// by the identity of its key Value.
pub fn get_by_value_with_keyed_cache(
    vm: &Vm,
    base_object: Gc<Object>,
    this_value: Value,
    property_key: &PropertyKey,
    site_cache: Option<(&PropertyLookupCache, u64)>,
) -> ThrowCompletionOr<Value> {
    if !property_key.is_string() {
        return base_object.internal_get(vm, property_key, this_value, None, PropertyLookupPhase::OwnProperty);
    }

    let property_name = property_key.as_string();
    let shape = base_object.shape();
    let keyed_property_lookup_cache = vm.keyed_property_lookup_cache();
    let entry_index = KeyedPropertyLookupCache::entry_index_for(shape, property_name);
    let entry = keyed_property_lookup_cache.entry(entry_index);
    let remember = |entry: &KeyedPropertyLookupCacheEntry| {
        if let Some((site_cache, site_cache_key)) = site_cache {
            remember_keyed_lookup(site_cache, site_cache_key, entry);
        }
    };
    if entry.shape == Some(shape)
        && entry.property_name.as_ref() == Some(property_name)
        && (!shape.is_dictionary() || shape.dictionary_generation() == entry.shape_dictionary_generation)
    {
        let prototype_chain_validity_is_valid = entry
            .prototype_chain_validity
            .is_some_and(|validity| validity.is_valid());
        match entry.entry_type {
            PropertyLookupCacheEntryType::GetOwnProperty => {
                remember(&entry);
                return get_cached_property_value(vm, base_object.get_direct(entry.property_offset), this_value);
            }
            PropertyLookupCacheEntryType::GetPropertyInPrototypeChain => {
                if prototype_chain_validity_is_valid {
                    remember(&entry);
                    let prototype = entry.prototype.expect("an inherited property has a holder");
                    return get_cached_property_value(vm, prototype.get_direct(entry.property_offset), this_value);
                }
            }
            PropertyLookupCacheEntryType::GetMissingProperty
                if base_object.is_cacheable_for_property_absence()
                    && (shape.prototype().is_none() || prototype_chain_validity_is_valid) =>
            {
                remember(&entry);
                return Ok(Value::UNDEFINED);
            }
            _ => {}
        }
    }

    let prototype_chain_validity = shape
        .prototype()
        .and_then(|prototype| prototype.shape().prototype_chain_validity());

    let dictionary_generation = shape.dictionary_generation();
    let mut cacheable_metadata = CacheableGetPropertyMetadata {
        property_absence_is_cacheable: base_object.is_cacheable_for_absence_of(vm, property_key),
        ..Default::default()
    };
    let value = base_object.internal_get(
        vm,
        property_key,
        this_value,
        Some(&mut cacheable_metadata),
        PropertyLookupPhase::OwnProperty,
    )?;

    // A getter may have changed the object's shape or the property storage of a dictionary shape, which
    // leaves the metadata describing a lookup that no longer applies.
    if shape != base_object.shape()
        || shape.dictionary_generation() != dictionary_generation
        || cacheable_metadata.r#type == CacheableGetPropertyMetadataType::NotCacheable
    {
        return Ok(value);
    }

    let mut entry = KeyedPropertyLookupCacheEntry {
        shape: Some(shape),
        property_name: Some(property_name.clone()),
        ..Default::default()
    };
    if shape.is_dictionary() {
        entry.shape_dictionary_generation = shape.dictionary_generation();
    }
    match cacheable_metadata.r#type {
        CacheableGetPropertyMetadataType::GetOwnProperty => {
            entry.entry_type = PropertyLookupCacheEntryType::GetOwnProperty;
            entry.property_offset = cacheable_metadata
                .property_offset
                .expect("cacheable metadata has an offset");
        }
        CacheableGetPropertyMetadataType::GetPropertyInPrototypeChain => {
            entry.entry_type = PropertyLookupCacheEntryType::GetPropertyInPrototypeChain;
            entry.property_offset = cacheable_metadata
                .property_offset
                .expect("cacheable metadata has an offset");
            entry.prototype = cacheable_metadata.prototype;
            entry.prototype_chain_validity = prototype_chain_validity;
        }
        CacheableGetPropertyMetadataType::GetMissingProperty => {
            entry.entry_type = PropertyLookupCacheEntryType::GetMissingProperty;
            entry.prototype_chain_validity = prototype_chain_validity;
        }
        CacheableGetPropertyMetadataType::NotCacheable => unreachable!("an uncacheable lookup returned above"),
    }
    remember(&entry);
    keyed_property_lookup_cache.set_entry(entry_index, entry);
    Ok(value)
}

// Non-standard
pub fn get_own_property_without_side_effects(
    object: &Object,
    property_key: &PropertyKey,
    cache: &PropertyLookupCache,
) -> Value {
    let shape = object.shape();

    if let Some(cache_entry) = cache.first_entry()
        && (cache_entry.entry_type == PropertyLookupCacheEntryType::GetOwnProperty
            || cache_entry.entry_type == PropertyLookupCacheEntryType::GetMissingProperty)
        && Some(shape) == cache_entry.shape
        && (!shape.is_dictionary() || shape.dictionary_generation() == cache_entry.shape_dictionary_generation)
    {
        if cache_entry.entry_type == PropertyLookupCacheEntryType::GetMissingProperty {
            return Value::EMPTY;
        }
        return object.get_direct(cache_entry.property_offset);
    }

    let metadata = shape.lookup(property_key);
    let cache_type = if metadata.is_some() {
        PropertyLookupCacheEntryType::GetOwnProperty
    } else {
        PropertyLookupCacheEntryType::GetMissingProperty
    };
    cache.update(cache_type, |entry| {
        entry.shape = Some(shape);
        if let Some(metadata) = metadata {
            entry.property_offset = metadata.offset;
        }
        if shape.is_dictionary() {
            entry.shape_dictionary_generation = shape.dictionary_generation();
        }
    });

    let Some(metadata) = metadata else {
        return Value::EMPTY;
    };
    object.get_direct(metadata.offset)
}

pub fn base_object_for_get_impl(vm: &Vm, base_value: Value) -> Option<Gc<Object>> {
    if base_value.is_object() {
        return Some(base_value.as_object());
    }

    // OPTIMIZATION: For various primitives we can avoid actually creating a new object for them.
    if base_value.is_nullish() {
        return None;
    }
    let realm = vm
        .current_realm()
        .expect("there is a current realm to find the prototype of a primitive in");
    if base_value.is_string() {
        return Some(realm.string_prototype(vm));
    }
    if base_value.is_number() {
        return Some(realm.number_prototype(vm));
    }
    if base_value.is_boolean() {
        return Some(realm.boolean_prototype(vm));
    }
    if base_value.is_bigint() {
        return Some(realm.bigint_prototype(vm));
    }
    if base_value.is_symbol() {
        return Some(realm.symbol_prototype(vm));
    }

    None
}

#[cold]
fn throw_null_or_undefined_property_get<T>(
    vm: &Vm,
    base_value: Value,
    get_base_identifier: impl FnOnce() -> Option<Utf16FlyString>,
    property_name: &dyn Utf16Display,
) -> ThrowCompletionOr<T> {
    assert!(base_value.is_nullish());

    if let Some(base_identifier) = get_base_identifier() {
        return vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::ToObjectNullOrUndefinedWithPropertyAndName,
            &[property_name, &base_value, &base_identifier],
        );
    }
    vm.throw_completion(
        ErrorKind::TypeError,
        ErrorType::ToObjectNullOrUndefinedWithProperty,
        &[property_name, &base_value],
    )
}

/// The object a property get on `base_value` looks the property up in. `property_name` is the property as the error
/// for a null or undefined base names it: a key, or the value a key has yet to be computed from.
pub fn base_object_for_get(
    vm: &Vm,
    base_value: Value,
    get_base_identifier: impl FnOnce() -> Option<Utf16FlyString>,
    property_name: &dyn Utf16Display,
) -> ThrowCompletionOr<Gc<Object>> {
    if let Some(base_object) = base_object_for_get_impl(vm, base_value) {
        return Ok(base_object);
    }

    // NOTE: At this point this is guaranteed to throw (null or undefined).
    throw_null_or_undefined_property_get(vm, base_value, get_base_identifier, property_name)
}

#[allow(clippy::too_many_arguments)]
pub fn get_by_id(
    vm: &Vm,
    mode: GetByIdMode,
    get_base_identifier: impl FnOnce() -> Option<Utf16FlyString>,
    property_name: &PropertyKey,
    base_value: Value,
    this_value: Value,
    cache: &PropertyLookupCache,
    cache_property_absence: CachePropertyAbsence,
) -> ThrowCompletionOr<Value> {
    if mode == GetByIdMode::Length && base_value.is_string() {
        return Ok(Value::from_f64(
            base_value.as_string().length_in_utf16_code_units() as f64
        ));
    }

    if base_value.is_string() {
        // https://tc39.es/ecma262/#sec-stringgetownproperty
        // String exotic objects expose virtual own properties for canonical string indexes.
        let string_value = base_value.as_string().get(vm, property_name)?;
        if let Some(string_value) = string_value {
            return Ok(string_value);
        }
    }

    let base_obj = base_object_for_get(vm, base_value, get_base_identifier, property_name)?;

    // OPTIMIZATION: Fast path for the magical "length" property on Array objects.
    if mode == GetByIdMode::Length && base_obj.has_magical_length_property() {
        return Ok(Value::from_f64(f64::from(base_obj.indexed_array_like_size())));
    }

    get_with_property_lookup_cache(
        vm,
        base_obj,
        property_name,
        this_value,
        cache,
        cache_property_absence,
        0,
    )
}

/// Gets the property from `base_obj` through `cache`, filling the cache on a miss. `cache_key` is the key the cache's
/// entries are for (see PropertyLookupCacheEntry::key).
#[inline(always)]
pub fn get_with_property_lookup_cache(
    vm: &Vm,
    base_obj: Gc<Object>,
    property_name: &PropertyKey,
    this_value: Value,
    cache: &PropertyLookupCache,
    cache_property_absence: CachePropertyAbsence,
    cache_key: u64,
) -> ThrowCompletionOr<Value> {
    let shape = base_obj.shape();

    // NB: The entries are read in place, and a getter only runs once the loop is done with them, since it may update
    //     the cache.
    let mut cached_value = None;
    for cache_entry in cache.entry_slots_for_shape(shape, cache_key) {
        // NB: Only the caches of keyed accesses have keys, and named accesses pass a constant 0.
        if cache_key != 0 && cache_entry.key.get() != cache_key {
            continue;
        }
        let entry_type = cache_entry.entry_type.get();
        if entry_type == PropertyLookupCacheEntryType::GetMissingProperty {
            if cache_property_absence == CachePropertyAbsence::No {
                continue;
            }
            if !base_obj.is_cacheable_for_property_absence() {
                continue;
            }
            if Some(shape) != cache_entry.shape.get() {
                continue;
            }
            if shape.is_dictionary() && shape.dictionary_generation() != cache_entry.shape_dictionary_generation.get() {
                continue;
            }
            if shape.prototype().is_some()
                && !cache_entry
                    .prototype_chain_validity
                    .get()
                    .is_some_and(|validity| validity.is_valid())
            {
                continue;
            }
            return Ok(Value::UNDEFINED);
        }

        if entry_type != PropertyLookupCacheEntryType::GetOwnProperty
            && entry_type != PropertyLookupCacheEntryType::GetPropertyInPrototypeChain
        {
            continue;
        }

        if let Some(cached_prototype) = cache_entry.prototype.get() {
            // OPTIMIZATION: If the prototype chain hasn't been mutated in a way that would invalidate the cache, we can use it.
            let can_use_cache = Some(shape) == cache_entry.shape.get()
                && (!shape.is_dictionary()
                    || shape.dictionary_generation() == cache_entry.shape_dictionary_generation.get())
                && cache_entry
                    .prototype_chain_validity
                    .get()
                    .is_some_and(|validity| validity.is_valid());
            if can_use_cache {
                cached_value = Some(cached_prototype.get_direct(cache_entry.property_offset.get()));
                break;
            }
        } else if Some(shape) == cache_entry.shape.get() {
            // OPTIMIZATION: If the shape of the object hasn't changed, we can use the cached property offset.
            let can_use_cache = !shape.is_dictionary()
                || shape.dictionary_generation() == cache_entry.shape_dictionary_generation.get();

            if can_use_cache {
                cached_value = Some(base_obj.get_direct(cache_entry.property_offset.get()));
                break;
            }
        }
    }
    if let Some(value) = cached_value {
        return get_cached_property_value(vm, value, this_value);
    }
    let prototype_chain_validity = shape
        .prototype()
        .and_then(|prototype| prototype.shape().prototype_chain_validity());

    let dictionary_generation = shape.dictionary_generation();
    let mut cacheable_metadata = CacheableGetPropertyMetadata {
        property_absence_is_cacheable: base_obj.is_cacheable_for_absence_of(vm, property_name),
        ..Default::default()
    };
    let value = base_obj.internal_get(
        vm,
        property_name,
        this_value,
        Some(&mut cacheable_metadata),
        PropertyLookupPhase::OwnProperty,
    )?;

    // If internal_get() caused object's shape change, we can no longer be sure
    // that collected metadata is valid, e.g. if getter in prototype chain added
    // property with the same name into the object itself. The same applies when
    // a getter changed the property storage of a dictionary shape.
    if shape == base_obj.shape() && shape.dictionary_generation() == dictionary_generation {
        match cacheable_metadata.r#type {
            CacheableGetPropertyMetadataType::GetOwnProperty => {
                cache.update(PropertyLookupCacheEntryType::GetOwnProperty, |entry| {
                    entry.key = cache_key;
                    entry.shape = Some(shape);
                    entry.property_offset = cacheable_metadata
                        .property_offset
                        .expect("cacheable metadata has an offset");

                    if shape.is_dictionary() {
                        entry.shape_dictionary_generation = shape.dictionary_generation();
                    }
                });
            }
            CacheableGetPropertyMetadataType::GetPropertyInPrototypeChain => {
                cache.update(PropertyLookupCacheEntryType::GetPropertyInPrototypeChain, |entry| {
                    entry.key = cache_key;
                    entry.shape = Some(base_obj.shape());
                    entry.property_offset = cacheable_metadata
                        .property_offset
                        .expect("cacheable metadata has an offset");
                    entry.prototype = cacheable_metadata.prototype;
                    entry.prototype_chain_validity = prototype_chain_validity;

                    if shape.is_dictionary() {
                        entry.shape_dictionary_generation = shape.dictionary_generation();
                    }
                });
            }
            CacheableGetPropertyMetadataType::GetMissingProperty
                if cache_property_absence == CachePropertyAbsence::Yes =>
            {
                cache.update(PropertyLookupCacheEntryType::GetMissingProperty, |entry| {
                    entry.key = cache_key;
                    entry.shape = Some(shape);
                    entry.prototype_chain_validity = prototype_chain_validity;

                    if shape.is_dictionary() {
                        entry.shape_dictionary_generation = shape.dictionary_generation();
                    }
                });
            }
            _ => {}
        }
    }

    Ok(value)
}

#[cold]
fn throw_null_or_undefined_property_access<T>(
    vm: &Vm,
    base_value: Value,
    base_identifier: Option<Utf16FlyString>,
    property_identifier: &PropertyKey,
) -> ThrowCompletionOr<T> {
    assert!(base_value.is_nullish());

    if let Some(base_identifier) = base_identifier {
        return vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::ToObjectNullOrUndefinedWithPropertyAndName,
            &[property_identifier, &base_value, &base_identifier],
        );
    }
    vm.throw_completion(
        ErrorKind::TypeError,
        ErrorType::ToObjectNullOrUndefinedWithProperty,
        &[property_identifier, &base_value],
    )
}

#[allow(clippy::too_many_arguments)]
#[inline]
pub fn put_by_property_key(
    vm: &Vm,
    base: Value,
    this_value: Value,
    value: Value,
    get_base_identifier: impl FnOnce() -> Option<Utf16FlyString>,
    name: &PropertyKey,
    kind: PutKind,
    strict: Strict,
    caches: Option<&PropertyLookupCache>,
    cache_key: u64,
) -> ThrowCompletionOr<()> {
    // Better error message than to_object would give
    if strict == Strict::Yes && base.is_nullish() {
        return vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::ReferenceNullishSetProperty,
            &[name, &base],
        );
    }

    // a. Let baseObj be ? ToObject(V.[[Base]]).
    if base.is_nullish() {
        return throw_null_or_undefined_property_access(vm, base, get_base_identifier(), name);
    }
    let object = base.to_object(vm)?;

    if kind == PutKind::Getter || kind == PutKind::Setter {
        // The generator should only pass us functions for getters and setters.
        assert!(value.is_function());
    }
    match kind {
        PutKind::Getter | PutKind::Setter => {
            let function = value.as_function();
            if let Some(ecmascript_function) = as_ecmascript_function_object(function)
                && ecmascript_function.name().is_empty()
            {
                let prefix = if kind == PutKind::Getter { "get" } else { "set" };
                ecmascript_function.set_inferred_name(vm, &ClassElementName::PropertyKey(name.clone()), Some(prefix));
            }
            let attributes = PropertyAttributes::new(Attribute::CONFIGURABLE | Attribute::ENUMERABLE);
            if kind == PutKind::Getter {
                object.define_direct_accessor(vm, name, Some(function), None, attributes);
            } else {
                object.define_direct_accessor(vm, name, None, Some(function), attributes);
            }
        }
        PutKind::Normal => {
            let this_value_object = this_value.to_object(vm).must();
            let from_shape = this_value_object.shape();
            let from_shape_dictionary_generation = from_shape.dictionary_generation();
            if let Some(caches) = caches {
                for cache in caches.entries_for_shape(object.shape(), cache_key).as_slice() {
                    // NB: Only the caches of keyed accesses have keys, and named accesses pass a constant 0.
                    if cache_key != 0 && cache.key != cache_key {
                        continue;
                    }
                    match cache.entry_type {
                        PropertyLookupCacheEntryType::Empty => {}
                        PropertyLookupCacheEntryType::ChangePropertyInPrototypeChain => {
                            let Some(cached_prototype) = cache.prototype else {
                                continue;
                            };
                            let Some(cached_shape) = cache.shape else {
                                continue;
                            };
                            // OPTIMIZATION: If the prototype chain hasn't been mutated in a way that would invalidate the cache, we can use it.
                            let can_use_cache = object.shape() == cached_shape
                                && (!cached_shape.is_dictionary()
                                    || object.shape().dictionary_generation() == cache.shape_dictionary_generation)
                                && cache
                                    .prototype_chain_validity
                                    .is_some_and(|validity| validity.is_valid());
                            if can_use_cache {
                                let value_in_prototype = cached_prototype.get_direct(cache.property_offset);
                                if value_in_prototype.is_accessor() {
                                    let Some(setter) = value_in_prototype.as_accessor().setter() else {
                                        continue;
                                    };
                                    call_function_object(vm, setter, this_value, &[value])?;
                                    return Ok(());
                                }
                            }
                        }
                        PropertyLookupCacheEntryType::ChangeOwnProperty => {
                            let Some(cached_shape) = cache.shape else {
                                continue;
                            };
                            if cached_shape != object.shape() {
                                continue;
                            }

                            if cached_shape.is_dictionary()
                                && cached_shape.dictionary_generation() != cache.shape_dictionary_generation
                            {
                                continue;
                            }

                            let value_in_object = object.get_direct(cache.property_offset);
                            if value_in_object.is_accessor() {
                                let Some(setter) = value_in_object.as_accessor().setter() else {
                                    continue;
                                };
                                call_function_object(vm, setter, this_value, &[value])?;
                                return Ok(());
                            }
                            if !cache.writes_data_property {
                                continue;
                            }
                            object.put_direct(cache.property_offset, value);
                            return Ok(());
                        }
                        PropertyLookupCacheEntryType::AddOwnProperty => {
                            // OPTIMIZATION: If the object's shape is the same as the one cached before adding the new property, we can
                            //               reuse the resulting shape from the cache.
                            if cache.from_shape != Some(object.shape()) {
                                continue;
                            }
                            if !property_addition_is_cacheable(vm, &object, name) {
                                continue;
                            }
                            let Some(cached_shape) = cache.shape else {
                                continue;
                            };

                            // Cannot add properties to non-extensible objects (frozen, sealed, or preventExtensions).
                            if !object.internal_is_extensible(vm)? {
                                continue;
                            }

                            if cached_shape.is_dictionary()
                                && object.shape().dictionary_generation() != cache.shape_dictionary_generation
                            {
                                continue;
                            }

                            // The cache is invalid if the prototype chain has been mutated, since such a mutation could have added a setter for the property.
                            if cache
                                .prototype_chain_validity
                                .is_some_and(|validity| !validity.is_valid())
                            {
                                continue;
                            }
                            object.unsafe_set_shape(cached_shape);
                            object.put_direct(cache.property_offset, value);
                            return Ok(());
                        }
                        PropertyLookupCacheEntryType::GetOwnProperty
                        | PropertyLookupCacheEntryType::GetPropertyInPrototypeChain
                        | PropertyLookupCacheEntryType::GetMissingProperty => {}
                    }
                }
            }

            let prototype_chain_validity = object
                .shape()
                .prototype()
                .and_then(|prototype| prototype.shape().prototype_chain_validity());

            let mut cacheable_metadata = CacheableSetPropertyMetadata::default();
            let succeeded = object.internal_set(
                vm,
                name,
                value,
                this_value,
                Some(&mut cacheable_metadata),
                PropertyLookupPhase::OwnProperty,
            )?;

            if let Some(caches) = caches
                && succeeded
                && cacheable_metadata.r#type == CacheableSetPropertyMetadataType::AddOwnProperty
            {
                caches.update(PropertyLookupCacheEntryType::AddOwnProperty, |cache| {
                    cache.key = cache_key;
                    cache.from_shape = Some(from_shape);
                    cache.property_offset = cacheable_metadata
                        .property_offset
                        .expect("cacheable metadata has an offset");
                    cache.shape = Some(object.shape());
                    if let Some(prototype) = cacheable_metadata.prototype {
                        cache.prototype_chain_validity = prototype.shape().prototype_chain_validity();
                    }
                    if object.shape().is_dictionary() {
                        cache.shape_dictionary_generation = object.shape().dictionary_generation();
                    }
                });
            }

            // If internal_set() caused object's shape change, we can no longer be sure
            // that collected metadata is valid, e.g. if setter in prototype chain added
            // property with the same name into the object itself. The same applies when
            // a setter changed the property storage of a dictionary shape.
            if let Some(caches) = caches
                && succeeded
                && from_shape == object.shape()
                && from_shape.dictionary_generation() == from_shape_dictionary_generation
            {
                match cacheable_metadata.r#type {
                    CacheableSetPropertyMetadataType::AddOwnProperty => {
                        // Something went wrong if we ended up here, because cacheable addition of a new property should've changed the shape.
                        unreachable!("a cacheable addition of a property changes the shape");
                    }
                    CacheableSetPropertyMetadataType::ChangeOwnProperty => {
                        caches.update(PropertyLookupCacheEntryType::ChangeOwnProperty, |cache| {
                            cache.key = cache_key;
                            cache.shape = Some(object.shape());
                            cache.property_offset = cacheable_metadata
                                .property_offset
                                .expect("cacheable metadata has an offset");
                            cache.writes_data_property = cacheable_metadata.writes_data_property;

                            if object.shape().is_dictionary() {
                                cache.shape_dictionary_generation = object.shape().dictionary_generation();
                            }
                        });
                    }
                    CacheableSetPropertyMetadataType::ChangePropertyInPrototypeChain => {
                        caches.update(PropertyLookupCacheEntryType::ChangePropertyInPrototypeChain, |cache| {
                            cache.key = cache_key;
                            cache.shape = Some(object.shape());
                            cache.property_offset = cacheable_metadata
                                .property_offset
                                .expect("cacheable metadata has an offset");
                            let prototype = cacheable_metadata
                                .prototype
                                .expect("a property in the prototype chain has a holder");
                            cache.prototype = Some(prototype);
                            cache.prototype_chain_validity = prototype_chain_validity;

                            if object.shape().is_dictionary() {
                                cache.shape_dictionary_generation = object.shape().dictionary_generation();
                            }
                        });
                    }
                    CacheableSetPropertyMetadataType::NotCacheable => {}
                }
            }

            if !succeeded && strict == Strict::Yes {
                if base.is_object() {
                    return vm.throw_completion(
                        ErrorKind::TypeError,
                        ErrorType::ReferenceNullishSetProperty,
                        &[name, &base],
                    );
                }
                return vm.throw_completion(
                    ErrorKind::TypeError,
                    ErrorType::ReferencePrimitiveSetProperty,
                    &[name, &base.typeof_(vm), &base],
                );
            }
        }
        PutKind::Own => {
            if let Some(caches) = caches {
                for cache in caches.entries_for_shape(object.shape(), cache_key).as_slice() {
                    // NB: Only the caches of keyed accesses have keys, and named accesses pass a constant 0.
                    if cache_key != 0 && cache.key != cache_key {
                        continue;
                    }
                    if cache.entry_type == PropertyLookupCacheEntryType::AddOwnProperty {
                        // PutKind::Own is not currently emitted for platform
                        // objects, but keep this aligned with the normal PutById
                        // AddOwnProperty cache hit so a future bytecode path cannot
                        // bypass subclass hooks for objects that require them.
                        if cache.from_shape != Some(object.shape()) {
                            continue;
                        }
                        if !property_addition_is_cacheable(vm, &object, name) {
                            continue;
                        }
                        let Some(cached_shape) = cache.shape else {
                            continue;
                        };
                        if cached_shape.is_dictionary()
                            && object.shape().dictionary_generation() != cache.shape_dictionary_generation
                        {
                            continue;
                        }
                        object.unsafe_set_shape(cached_shape);
                        object.put_direct(cache.property_offset, value);
                        return Ok(());
                    }
                }
            }

            let from_shape = object.shape();
            object.define_direct_property(
                vm,
                name,
                value,
                PropertyAttributes::new(Attribute::ENUMERABLE | Attribute::WRITABLE | Attribute::CONFIGURABLE),
            );

            if let Some(caches) = caches
                && from_shape != object.shape()
            {
                caches.update(PropertyLookupCacheEntryType::AddOwnProperty, |cache| {
                    cache.key = cache_key;
                    cache.from_shape = Some(from_shape);
                    cache.shape = Some(object.shape());
                    cache.property_offset = object.shape().lookup(name).expect("the property was just added").offset;
                    if object.shape().is_dictionary() {
                        cache.shape_dictionary_generation = object.shape().dictionary_generation();
                    }
                });
            }
        }
        PutKind::Prototype => {
            if value.is_object() || value.is_null() {
                object
                    .internal_set_prototype_of(vm, value.is_object().then(|| value.as_object()))
                    .must();
            }
        }
    }

    Ok(())
}
