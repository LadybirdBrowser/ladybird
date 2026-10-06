/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ops::{ControlFlow, Deref};

use libjs_runtime_macros::Trace;

use crate::gc::class::{Class, GcCell, define_cell};
use crate::gc::root::MarkedVec;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{
    CanonicalIndexMode, canonical_numeric_index_string, is_compatible_property_descriptor,
};
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, ObjectMethods};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::PropertyAttributes;
use crate::runtime::property_descriptor::PropertyDescriptor;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;

/// A String exotic object, whose [[StringData]] is `string`.
#[repr(C)]
#[derive(Trace)]
pub struct StringObject {
    base: Object,
    string: Gc<PrimitiveString>,
}

pub static STRING_OBJECT_METHODS: ObjectMethods = ObjectMethods {
    initialize: StringObject::initialize,
    internal_get_own_property: StringObject::internal_get_own_property,
    is_cacheable_for_property_absence: |_| false,
    internal_define_own_property: StringObject::internal_define_own_property,
    internal_own_property_keys: StringObject::internal_own_property_keys,
    eligible_for_own_property_enumeration_fast_path: |_| false,
    ..ORDINARY_OBJECT_METHODS
};

define_cell!(StringObject, Object, extends: [Object], methods: STRING_OBJECT_METHODS);

impl Deref for StringObject {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

/// The String object an internal method of a String object was called on.
fn as_string_object(object: &Object) -> &StringObject {
    assert!(object.is::<StringObject>());
    // SAFETY: The object is a StringObject, which starts with its Object.
    unsafe { &*core::ptr::from_ref(object).cast::<StringObject>() }
}

impl StringObject {
    /// A StringObject with `prototype`, for `class`, which is StringObject or a class that extends it.
    pub fn new(vm: &Vm, class: &'static Class, string: Gc<PrimitiveString>, prototype: Gc<Object>) -> StringObject {
        StringObject {
            base: Object::new_with_prototype(vm, class, prototype, MayInterfereWithIndexedPropertyAccess::Yes),
            string,
        }
    }

    // 10.4.3.4 StringCreate ( value, prototype ), https://tc39.es/ecma262/#sec-stringcreate
    pub fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        primitive_string: Gc<PrimitiveString>,
        prototype: Gc<Object>,
    ) -> Gc<StringObject> {
        // 1. Let S be MakeBasicObject(« [[Prototype]], [[Extensible]], [[StringData]] »).
        // 2. Set S.[[Prototype]] to prototype.
        // 3. Set S.[[StringData]] to value.
        // 4. Set S.[[GetOwnProperty]] as specified in 10.4.3.1.
        // 5. Set S.[[DefineOwnProperty]] as specified in 10.4.3.2.
        // 6. Set S.[[OwnPropertyKeys]] as specified in 10.4.3.3.
        // 7. Let length be the length of value.
        // 8. Perform ! DefinePropertyOrThrow(S, "length", PropertyDescriptor { [[Value]]: 𝔽(length), [[Writable]]: false, [[Enumerable]]: false, [[Configurable]]: false }).
        // 9. Return S.
        realm.create_object(vm, StringObject::new(vm, Self::CLASS, primitive_string, prototype))
    }

    pub fn primitive_string(&self) -> Gc<PrimitiveString> {
        self.string
    }

    pub(crate) fn initialize(object: &Object, vm: &Vm, _realm: Gc<Realm>) {
        let string_object = as_string_object(object);
        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_f64(string_object.string.length_in_utf16_code_units() as f64),
            PropertyAttributes::new(0),
        );
    }

    // 10.4.3.5 StringGetOwnProperty ( S, P ), https://tc39.es/ecma262/#sec-stringgetownproperty
    fn string_get_own_property(&self, vm: &Vm, property_key: &PropertyKey) -> Option<PropertyDescriptor> {
        // 1. If P is not a String, return undefined.
        // NOTE: The spec only uses string and symbol keys, and later coerces to numbers -
        // this is not the case for PropertyKey, so '!property_key.is_string()' would be wrong.
        if property_key.is_symbol() {
            return None;
        }

        // 2. Let index be CanonicalNumericIndexString(P).
        let index = canonical_numeric_index_string(property_key, CanonicalIndexMode::IgnoreNumericRoundtrip);

        // 3. If index is undefined, return undefined.
        // 4. If index is not an integral Number, return undefined.
        // 5. If index is -0𝔽, return undefined.
        if !index.is_index() {
            return None;
        }

        // 6. Let str be S.[[StringData]].
        // 7. Assert: Type(str) is String.
        let primitive_string = self.string;

        // 8. Let len be the length of str.
        let length = primitive_string.length_in_utf16_code_units();

        // 9. If ℝ(index) < 0 or len ≤ ℝ(index), return undefined.
        let index = index.as_index() as usize;
        if length <= index {
            return None;
        }

        // 10. Let resultStr be the substring of str from ℝ(index) to ℝ(index) + 1.
        let result_str = PrimitiveString::create_from_substring(vm, primitive_string, index, 1);

        // 11. Return the PropertyDescriptor { [[Value]]: resultStr, [[Writable]]: false, [[Enumerable]]: true, [[Configurable]]: false }.
        Some(PropertyDescriptor {
            value: Some(Value::from_string(result_str)),
            writable: Some(false),
            enumerable: Some(true),
            configurable: Some(false),
            ..Default::default()
        })
    }

    // 10.4.3.1 [[GetOwnProperty]] ( P ), https://tc39.es/ecma262/#sec-string-exotic-objects-getownproperty-p
    #[allow(clippy::unnecessary_wraps, reason = "[[GetOwnProperty]] can throw for other objects")]
    fn internal_get_own_property(
        object: &Object,
        vm: &Vm,
        property_key: &PropertyKey,
    ) -> ThrowCompletionOr<Option<PropertyDescriptor>> {
        // 1. Let desc be OrdinaryGetOwnProperty(S, P).
        let descriptor = object.ordinary_get_own_property(vm, property_key).must();

        // 2. If desc is not undefined, return desc.
        if descriptor.is_some() {
            return Ok(descriptor);
        }

        // 3. Return StringGetOwnProperty(S, P).
        Ok(as_string_object(object).string_get_own_property(vm, property_key))
    }

    // 10.4.3.2 [[DefineOwnProperty]] ( P, Desc ), https://tc39.es/ecma262/#sec-string-exotic-objects-defineownproperty-p-desc
    fn internal_define_own_property(
        object: &Object,
        vm: &Vm,
        property_key: &PropertyKey,
        property_descriptor: &mut PropertyDescriptor,
        precomputed_get_own_property: Option<&Option<PropertyDescriptor>>,
    ) -> ThrowCompletionOr<bool> {
        // 1. Let stringDesc be StringGetOwnProperty(S, P).
        let string_descriptor = as_string_object(object).string_get_own_property(vm, property_key);

        // 2. If stringDesc is not undefined, then
        if string_descriptor.is_some() {
            // a. Let extensible be S.[[Extensible]].
            let extensible = object.extensible();

            // b. Return IsCompatiblePropertyDescriptor(extensible, Desc, stringDesc).
            return Ok(is_compatible_property_descriptor(
                vm,
                extensible,
                property_descriptor,
                &string_descriptor,
            ));
        }

        // 3. Return ! OrdinaryDefineOwnProperty(S, P, Desc).
        object.ordinary_define_own_property(vm, property_key, property_descriptor, precomputed_get_own_property)
    }

    // 10.4.3.3 [[OwnPropertyKeys]] ( ), https://tc39.es/ecma262/#sec-string-exotic-objects-ownpropertykeys
    #[allow(
        clippy::unnecessary_wraps,
        reason = "[[OwnPropertyKeys]] can throw for other objects"
    )]
    fn internal_own_property_keys<'vm>(object: &Object, vm: &'vm Vm) -> ThrowCompletionOr<MarkedVec<'vm, Value>> {
        // 1. Let keys be a new empty List.
        let keys = MarkedVec::new(vm);

        // 2. Let str be O.[[StringData]].
        // 3. Assert: str is a String.
        // 4. Let len be the length of str.
        let length = as_string_object(object).string.length_in_utf16_code_units();

        // 5. For each integer i starting with 0 such that i < len, in ascending order, do
        for i in 0..length {
            // a. Add ! ToString(𝔽(i)) as the last element of keys.
            keys.push(Value::from_string(PrimitiveString::create_from_unsigned_integer(
                vm, i as u64,
            )));
        }

        // 6. For each own property key P of O such that P is an array index and ! ToIntegerOrInfinity(P) ≥ len, in ascending numeric index order, do
        {
            let indices = object.indexed_indices();
            for index in indices {
                if index as usize >= length {
                    // a. Add P as the last element of keys.
                    keys.push(Value::from_string(PrimitiveString::create_from_unsigned_integer(
                        vm,
                        u64::from(index),
                    )));
                }
            }
        }

        // The keys are copied out first, since turning them into values allocates.
        let string_keys = MarkedVec::new(vm);
        let symbol_keys = MarkedVec::new(vm);
        object.shape().for_each_property_in_insertion_order(|property_key, _| {
            if property_key.is_string() {
                string_keys.push(property_key.clone());
            } else if property_key.is_symbol() && !property_key.is_private() {
                symbol_keys.push(property_key.clone());
            }
            ControlFlow::Continue(())
        });

        // 7. For each own property key P of O such that P is a String and P is not an array index, in ascending chronological order of property creation, do
        for index in 0..string_keys.len() {
            let property_key: PropertyKey = string_keys.get(index).expect("the index is in bounds");
            // a. Add P as the last element of keys.
            keys.push(property_key.to_value(vm));
        }

        // 8. For each own property key P of O such that P is a Symbol, in ascending chronological order of property creation, do
        for index in 0..symbol_keys.len() {
            let property_key: PropertyKey = symbol_keys.get(index).expect("the index is in bounds");
            // a. Add P as the last element of keys.
            keys.push(property_key.to_value(vm));
        }

        // 9. Return keys.
        Ok(keys)
    }
}
