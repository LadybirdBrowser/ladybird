/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The special property cache of a shape: what Get of the properties that conversions of objects to primitives and
//! JSON.stringify look up gives on the objects of the shape, for as long as that holds.
//!
//! The cache only knows what Get gives without running code: that no object on the prototype chain has the property,
//! or that a prototype has it as a data property with a value. It relies on watchpoint sets for that to stay true. The
//! shape of the prototype of the objects has one that fires when that prototype or one above it changes its
//! properties or its prototype (see `Shape::prototype_chain_validity`), and the prototype that has the property has
//! one that fires when the value of the property changes (see `Shape::watch_slot_for_replacements()`). The cache
//! forgets everything it knows when one of them fires, and learns it again from the next Get.

use core::cell::Cell;

use crate::bytecode::executable::StaticPropertyLookupCacheSite;
use crate::gc::class::{Class, GcCell, define_cell};
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::object_prototype::ObjectPrototype;
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::value::PreferredType;
use crate::runtime::watchpoint_set::WatchpointSet;

/// The most prototypes the cache looks through for a property.
const MAX_PROTOTYPE_CHAIN_LENGTH: usize = 8;

/// The properties the cache knows Get of.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(u8)]
pub enum SpecialPropertyKey {
    ToPrimitive,
    ToString,
    ValueOf,
    ToStringTag,
    ToJson,
    Then,
    Constructor,
}

const SPECIAL_PROPERTY_KEY_COUNT: usize = 7;

impl SpecialPropertyKey {
    fn property_key(self, vm: &Vm) -> PropertyKey {
        match self {
            Self::ToPrimitive => PropertyKey::from(vm.well_known_symbols().to_primitive),
            Self::ToString => vm.names.toString.clone(),
            Self::ValueOf => vm.names.valueOf.clone(),
            Self::ToStringTag => PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Self::ToJson => vm.names.toJSON.clone(),
            Self::Then => vm.names.then.clone(),
            Self::Constructor => vm.names.constructor.clone(),
        }
    }

    fn static_property_lookup_cache_site(self) -> StaticPropertyLookupCacheSite {
        match self {
            Self::ToPrimitive => StaticPropertyLookupCacheSite::ValueToPrimitive,
            Self::ToString => StaticPropertyLookupCacheSite::ObjectOrdinaryToPrimitiveToString,
            Self::ValueOf => StaticPropertyLookupCacheSite::ObjectOrdinaryToPrimitiveValueOf,
            Self::ToStringTag => StaticPropertyLookupCacheSite::ObjectPrototypeToStringToStringTag,
            Self::ToJson => StaticPropertyLookupCacheSite::JSONSerializeToJSON,
            Self::Then => StaticPropertyLookupCacheSite::PromiseResolveFunctionThen,
            Self::Constructor => StaticPropertyLookupCacheSite::PromiseResolveConstructor,
        }
    }
}

/// Whether ToPrimitive of the objects with one kind of preferred type gives what %Object.prototype.toString% gives for
/// them without running code, as far as the cache knows.
#[derive(Clone, Copy, PartialEq, Eq)]
enum DefaultConversion {
    Unknown,
    Yes,
    No,
}

#[repr(C)]
pub struct SpecialPropertyCache {
    header: CellHeader,
    /// The class of the objects the cache knows of, since objects of different classes can have the same shape.
    class: Cell<Option<&'static Class>>,
    /// Whether objects of more than one class had the shape, which the cache gives up on.
    gave_up: Cell<bool>,
    /// How many times the cache forgot what it knew.
    generation: Cell<u32>,
    /// The validity of the shape of the prototype of the objects that the cache watches in this generation, which it
    /// only watches once.
    watched_prototype_chain_validity: Cell<Option<Gc<WatchpointSet>>>,
    /// What Get of each key gives, or empty while that is not known.
    values: [Cell<Value>; SPECIAL_PROPERTY_KEY_COUNT],
    /// What %Object.prototype.toString% gives for the objects, while that is known.
    object_to_string: Cell<Option<Gc<PrimitiveString>>>,
    /// Whether ToPrimitive gives object_to_string with the preferred type string, and with the others.
    default_conversions: [Cell<DefaultConversion>; 2],
}

define_cell!(SpecialPropertyCache, Other);

// SAFETY: Visits the values and the string the cache knows, and the validity it watches.
unsafe impl Trace for SpecialPropertyCache {
    fn trace(&self, visitor: &mut Visitor) {
        self.watched_prototype_chain_validity.trace(visitor);
        for value in &self.values {
            value.trace(visitor);
        }
        self.object_to_string.trace(visitor);
    }
}

impl SpecialPropertyCache {
    fn create(vm: &Vm) -> Gc<SpecialPropertyCache> {
        vm.heap().allocate(|_| SpecialPropertyCache {
            header: CellHeader::for_class(Self::CLASS),
            class: Cell::new(None),
            gave_up: Cell::new(false),
            generation: Cell::new(0),
            watched_prototype_chain_validity: Cell::new(None),
            values: core::array::from_fn(|_| Cell::new(Value::EMPTY)),
            object_to_string: Cell::new(None),
            default_conversions: core::array::from_fn(|_| Cell::new(DefaultConversion::Unknown)),
        })
    }

    pub(crate) fn generation(&self) -> u32 {
        self.generation.get()
    }

    /// Forgets what the cache knows, if it still knows what it knew in its generation `generation`.
    pub(crate) fn forget(&self, generation: u32) {
        if generation != self.generation.get() {
            return;
        }
        self.generation.set(generation.wrapping_add(1));
        self.class.set(None);
        self.watched_prototype_chain_validity.set(None);
        for value in &self.values {
            value.set(Value::EMPTY);
        }
        self.object_to_string.set(None);
        for default_conversion in &self.default_conversions {
            default_conversion.set(DefaultConversion::Unknown);
        }
    }

    /// Whether ToPrimitive of the objects gives object_to_string, if that is known yet.
    fn default_conversion(&self, vm: &Vm, preferred_type: PreferredType) -> DefaultConversion {
        let index = usize::from(preferred_type != PreferredType::String);
        let known = self.default_conversions[index].get();
        if known != DefaultConversion::Unknown {
            return known;
        }

        // NB: ToPrimitive calls neither @@toPrimitive if it is undefined or null, nor valueOf if the preferred type is
        //     string. It skips %Object.prototype.valueOf%, which gives the object, and %Object.prototype.toString%
        //     gives object_to_string.
        let value = |key: SpecialPropertyKey| self.values[key as usize].get();
        let is_function_with = |value: Value, is_function: fn(&Vm, Gc<FunctionObject>) -> bool| {
            value.is_function() && is_function(vm, value.as_function())
        };
        let to_primitive = value(SpecialPropertyKey::ToPrimitive);
        let to_string = value(SpecialPropertyKey::ToString);
        let value_of = value(SpecialPropertyKey::ValueOf);
        let checks_value_of = preferred_type != PreferredType::String;
        let definitely_not = (!to_primitive.is_empty() && !to_primitive.is_nullish())
            || (!to_string.is_empty() && !is_function_with(to_string, ObjectPrototype::is_to_string_function))
            || (checks_value_of
                && !value_of.is_empty()
                && !is_function_with(value_of, ObjectPrototype::is_value_of_function));
        let known = if definitely_not {
            DefaultConversion::No
        } else if to_primitive.is_empty()
            || to_string.is_empty()
            || (checks_value_of && value_of.is_empty())
            || self.object_to_string.get().is_none()
        {
            return DefaultConversion::Unknown;
        } else {
            DefaultConversion::Yes
        };
        self.default_conversions[index].set(known);
        known
    }
}

/// The cache of the shape of `object`, if it knows of objects like it.
#[inline]
fn cache_of(object: &Object) -> Option<Gc<SpecialPropertyCache>> {
    let cache = object.shape().special_property_cache()?;
    let class = cache.class.get()?;
    // NB: The cache only knows of objects with ordinary property lookup, which only an object with unimplemented
    //     properties has without its class saying so.
    (core::ptr::eq(class, object.class()) && !object.has_unimplemented_properties()).then_some(cache)
}

/// OPTIMIZATION: ToPrimitive(object, preferred_type) when it gives what %Object.prototype.toString% gives for the
///               object without running code.
#[inline]
pub fn to_primitive_without_running_code(
    vm: &Vm,
    object: &Object,
    preferred_type: PreferredType,
) -> Option<Gc<PrimitiveString>> {
    let cache = cache_of(object)?;
    if cache.default_conversion(vm, preferred_type) != DefaultConversion::Yes {
        return None;
    }
    cache.object_to_string.get()
}

/// What %Object.prototype.toString% gives for `object`, if the cache knows.
pub fn object_to_string(object: &Object) -> Option<Gc<PrimitiveString>> {
    cache_of(object)?.object_to_string.get()
}

/// Notes that %Object.prototype.toString% gave `string` for `object`, with `to_string_tag` from Get of @@toStringTag.
pub fn note_object_to_string(object: &Object, to_string_tag: Value, string: Gc<PrimitiveString>) {
    // NB: The string only depends on the class and the shape of the object, and on what Get of @@toStringTag gives,
    //     which the cache must know to be what the string was made from. A getter may have changed it since.
    if let Some(cache) = cache_of(object)
        && known(object, SpecialPropertyKey::ToStringTag) == Some(to_string_tag)
    {
        cache.object_to_string.set(Some(string));
    }
}

/// What Get(object, key) gives, if the cache knows.
#[inline]
pub fn known(object: &Object, key: SpecialPropertyKey) -> Option<Value> {
    let value = cache_of(object)?.values[key as usize].get();
    (!value.is_empty()).then_some(value)
}

/// Get(object, key), from the cache if it knows what that gives.
pub fn get(vm: &Vm, object: &Object, key: SpecialPropertyKey) -> ThrowCompletionOr<Value> {
    if let Some(value) = known(object, key) {
        return Ok(value);
    }
    let property_key = key.property_key(vm);
    let value = object.get_with_cache(
        vm,
        &property_key,
        vm.static_property_lookup_cache(key.static_property_lookup_cache_site()),
    )?;
    learn(vm, object, key, &property_key, value);
    Ok(value)
}

/// Has the cache of the shape of `object` know that Get(object, key) gives `value`, if that holds without running code
/// for as long as the watchpoint sets it relies on do not fire.
fn learn(vm: &Vm, object: &Object, key: SpecialPropertyKey, property_key: &PropertyKey, value: Value) {
    // NB: The objects of a dictionary shape or of a prototype shape change it in place.
    let shape = object.shape();
    if shape.is_dictionary() || shape.is_prototype_shape() || !object.has_ordinary_named_property_lookup() {
        return;
    }

    // NB: The cache gives up on a shape that objects of more than one class have.
    if let Some(cache) = shape.special_property_cache() {
        if cache.gave_up.get() {
            return;
        }
        if cache
            .class
            .get()
            .is_some_and(|class| !core::ptr::eq(class, object.class()))
        {
            cache.forget(cache.generation());
            cache.gave_up.set(true);
            return;
        }
    }

    // NB: An own property is not cached, since other objects that get the shape may have another value in it.
    if shape.lookup(property_key).is_some() {
        return;
    }

    let mut holder = None;
    let first_prototype = shape.prototype();
    let object_prototype = vm.current_realm().map(|realm| realm.object_prototype());
    let mut current = first_prototype;
    let mut chain_length = 0;
    while let Some(prototype) = current {
        chain_length += 1;
        // NB: %Object.prototype% only has an exotic [[SetPrototypeOf]].
        let is_ordinary = Some(prototype) == object_prototype || prototype.has_ordinary_named_property_lookup();
        if chain_length > MAX_PROTOTYPE_CHAIN_LENGTH || !is_ordinary || prototype.has_unimplemented_properties() {
            return;
        }
        if let Some(metadata) = prototype.shape().lookup(property_key) {
            // NB: The value must be the one Get just gave, which also leaves out accessors and intrinsics that have
            //     not computed their value yet.
            let slot_value = prototype.get_direct(metadata.offset);
            if slot_value.is_accessor() || slot_value != value {
                return;
            }
            holder = Some((prototype, metadata.offset));
            break;
        }
        current = prototype.shape().prototype();
    }
    if holder.is_none() && !value.is_undefined() {
        return;
    }

    let prototype_chain_validity = match first_prototype {
        Some(first_prototype) => {
            let Some(validity) = first_prototype.shape().prototype_chain_validity() else {
                return;
            };
            if !validity.is_valid() {
                return;
            }
            Some(validity)
        }
        None => None,
    };

    let cache = match shape.special_property_cache() {
        Some(cache) => cache,
        None => {
            let cache = SpecialPropertyCache::create(vm);
            shape.set_special_property_cache(cache);
            cache
        }
    };
    if cache.class.get().is_none() {
        cache.class.set(Some(object.class()));
    }

    let generation = cache.generation();
    if let Some((holder, offset)) = holder {
        let Some(replacement_watchpoint_set) = holder.shape().watch_slot_for_replacements(vm, offset) else {
            return;
        };
        replacement_watchpoint_set.add_watcher(vm, cache, generation);
    }
    if let Some(prototype_chain_validity) = prototype_chain_validity
        && cache.watched_prototype_chain_validity.get() != Some(prototype_chain_validity)
    {
        prototype_chain_validity.add_watcher(vm, cache, generation);
        cache
            .watched_prototype_chain_validity
            .set(Some(prototype_chain_validity));
    }
    cache.values[key as usize].set(value);
}
