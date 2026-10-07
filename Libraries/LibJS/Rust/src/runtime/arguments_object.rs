/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::Deref;

use ak::Utf16FlyString;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::environment::Environment;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::object::{
    CacheableGetPropertyMetadata, CacheableSetPropertyMetadata, MayInterfereWithIndexedPropertyAccess,
    ORDINARY_OBJECT_METHODS, ObjectMethods, PropertyLookupPhase, allocate_object,
};
use crate::runtime::property_descriptor::PropertyDescriptor;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::value::same_value;

/// The arguments exotic object of a function with a simple parameter list in sloppy mode code, whose indexed
/// properties alias the bindings of the parameters they were passed for.
#[repr(C)]
#[derive(Trace)]
pub struct ArgumentsObject {
    base: Object,
    environment: Cell<Gc<Environment>>,
    mapped_names: GcRefCell<Vec<Utf16FlyString>>,
}

pub static ARGUMENTS_OBJECT_METHODS: ObjectMethods = ObjectMethods {
    internal_get_own_property: ArgumentsObject::internal_get_own_property,
    internal_define_own_property: ArgumentsObject::internal_define_own_property,
    internal_get: ArgumentsObject::internal_get,
    is_cacheable_for_property_absence: |_| false,
    internal_set: ArgumentsObject::internal_set,
    internal_delete: ArgumentsObject::internal_delete,
    ..ORDINARY_OBJECT_METHODS
};

define_cell!(ArgumentsObject, Object, extends: [Object], methods: ARGUMENTS_OBJECT_METHODS);

impl Deref for ArgumentsObject {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

/// The arguments object an internal method of an arguments object was called on.
fn as_arguments_object(object: &Object) -> &ArgumentsObject {
    assert!(object.is::<ArgumentsObject>());
    // SAFETY: The object is an ArgumentsObject, which starts with its Object.
    unsafe { &*core::ptr::from_ref(object).cast::<ArgumentsObject>() }
}

impl ArgumentsObject {
    pub fn create(vm: &Vm, realm: Gc<Realm>, environment: Gc<Environment>, parameter_list_is_empty: bool) -> Gc<Self> {
        allocate_object(
            vm,
            ArgumentsObject {
                base: Object::new_with_shape(
                    Self::CLASS,
                    realm.mapped_arguments_object_shape(),
                    if parameter_list_is_empty {
                        MayInterfereWithIndexedPropertyAccess::No
                    } else {
                        MayInterfereWithIndexedPropertyAccess::Yes
                    },
                ),
                environment: Cell::new(environment),
                mapped_names: GcRefCell::new(Vec::new()),
            },
        )
    }

    pub fn set_mapped_names(&self, mapped_names: Vec<Utf16FlyString>) {
        self.mapped_names.replace(mapped_names);
    }

    fn parameter_map_has(&self, property_key: &PropertyKey) -> bool {
        if !property_key.is_number() {
            return false;
        }
        let mapped_names = self.mapped_names.borrow();
        let index = property_key.as_number() as usize;
        index < mapped_names.len() && !mapped_names[index].is_empty()
    }

    /// The name of the parameter `property_key` is mapped to, copied out since using it calls into the environment.
    fn mapped_name(&self, property_key: &PropertyKey) -> Utf16FlyString {
        self.mapped_names.borrow()[property_key.as_number() as usize].clone()
    }

    // 10.4.4.3 [[Get]] ( P, Receiver ), https://tc39.es/ecma262/#sec-arguments-exotic-objects-get-p-receiver
    fn internal_get(
        object: &Object,
        vm: &Vm,
        property_key: &PropertyKey,
        receiver: Value,
        cacheable_metadata: Option<&mut CacheableGetPropertyMetadata>,
        phase: PropertyLookupPhase,
    ) -> ThrowCompletionOr<Value> {
        let arguments_object = as_arguments_object(object);

        // 1. Let map be args.[[ParameterMap]].
        // 2. Let isMapped be ! HasOwnProperty(map, P).
        let is_mapped = arguments_object.parameter_map_has(property_key);

        // 3. If isMapped is false, then
        if !is_mapped {
            // a. Return ? OrdinaryGet(args, P, Receiver).
            return object.ordinary_get(vm, property_key, receiver, cacheable_metadata, phase);
        }

        // a. Assert: map contains a formal parameter mapping for P.
        // b. Return ! Get(map, P).
        Ok(arguments_object.get_from_parameter_map(vm, property_key))
    }

    // 10.4.4.4 [[Set]] ( P, V, Receiver ), https://tc39.es/ecma262/#sec-arguments-exotic-objects-set-p-v-receiver
    fn internal_set(
        object: &Object,
        vm: &Vm,
        property_key: &PropertyKey,
        value: Value,
        receiver: Value,
        _cacheable_metadata: Option<&mut CacheableSetPropertyMetadata>,
        _phase: PropertyLookupPhase,
    ) -> ThrowCompletionOr<bool> {
        let arguments_object = as_arguments_object(object);

        // 1. If SameValue(args, Receiver) is false, then
        let is_mapped = if !same_value(Value::from_object(object.as_gc()), receiver) {
            // a. Let isMapped be false.
            false
        } else {
            // a. Let map be args.[[ParameterMap]].
            // b. Let isMapped be ! HasOwnProperty(map, P).
            arguments_object.parameter_map_has(property_key)
        };

        // 3. If isMapped is true, then
        if is_mapped {
            // a. Assert: The following Set will succeed, since formal parameters mapped by arguments objects are always writable.
            // b. Perform ! Set(map, P, V, false).
            arguments_object.set_in_parameter_map(vm, property_key, value);
        }

        // 4. Return ? OrdinarySet(args, P, V, Receiver).
        object.ordinary_set(
            vm,
            property_key,
            value,
            receiver,
            None,
            PropertyLookupPhase::OwnProperty,
        )
    }

    // 10.4.4.5 [[Delete]] ( P ), https://tc39.es/ecma262/#sec-arguments-exotic-objects-delete-p
    fn internal_delete(object: &Object, vm: &Vm, property_key: &PropertyKey) -> ThrowCompletionOr<bool> {
        let arguments_object = as_arguments_object(object);

        // 1. Let map be args.[[ParameterMap]].
        // 2. Let isMapped be ! HasOwnProperty(map, P).
        let is_mapped = arguments_object.parameter_map_has(property_key);

        // 3. Let result be ? OrdinaryDelete(args, P).
        let result = object.ordinary_delete(vm, property_key)?;

        // 4. If result is true and isMapped is true, then
        if result && is_mapped {
            // a. Perform ! map.[[Delete]](P).
            arguments_object.delete_from_parameter_map(property_key);
        }

        // 5. Return result.
        Ok(result)
    }

    // 10.4.4.1 [[GetOwnProperty]] ( P ), https://tc39.es/ecma262/#sec-arguments-exotic-objects-getownproperty-p
    #[allow(
        clippy::unnecessary_wraps,
        reason = "the internal method's type lets other objects throw"
    )]
    fn internal_get_own_property(
        object: &Object,
        vm: &Vm,
        property_key: &PropertyKey,
    ) -> ThrowCompletionOr<Option<PropertyDescriptor>> {
        let arguments_object = as_arguments_object(object);

        // 1. Let desc be OrdinaryGetOwnProperty(args, P).
        let descriptor = object.ordinary_get_own_property(vm, property_key).must();

        // 2. If desc is undefined, return desc.
        let Some(mut descriptor) = descriptor else {
            return Ok(None);
        };

        // 3. Let map be args.[[ParameterMap]].
        // 4. Let isMapped be ! HasOwnProperty(map, P).
        let is_mapped = arguments_object.parameter_map_has(property_key);

        // 5. If isMapped is true, then
        if is_mapped {
            // a. Set desc.[[Value]] to ! Get(map, P).
            descriptor.value = Some(arguments_object.get_from_parameter_map(vm, property_key));
        }

        // 6. Return desc.
        Ok(Some(descriptor))
    }

    // 10.4.4.2 [[DefineOwnProperty]] ( P, Desc ), https://tc39.es/ecma262/#sec-arguments-exotic-objects-defineownproperty-p-desc
    #[allow(
        clippy::unnecessary_wraps,
        reason = "the internal method's type lets other objects throw"
    )]
    fn internal_define_own_property(
        object: &Object,
        vm: &Vm,
        property_key: &PropertyKey,
        descriptor: &mut PropertyDescriptor,
        precomputed_get_own_property: Option<&Option<PropertyDescriptor>>,
    ) -> ThrowCompletionOr<bool> {
        let arguments_object = as_arguments_object(object);

        // 1. Let map be args.[[ParameterMap]].
        // 2. Let isMapped be ! HasOwnProperty(map, P).
        let is_mapped = arguments_object.parameter_map_has(property_key);

        // 3. Let newArgDesc be Desc.
        let mut new_arg_desc = *descriptor;

        // 4. If isMapped is true and IsDataDescriptor(Desc) is true, then
        if is_mapped && descriptor.is_data_descriptor() {
            // a. If Desc does not have a [[Value]] field and Desc has a [[Writable]] field, and Desc.[[Writable]] is false, then
            if descriptor.value.is_none() && descriptor.writable == Some(false) {
                // i. Set newArgDesc to a copy of Desc.
                new_arg_desc = *descriptor;
                // ii. Set newArgDesc.[[Value]] to ! Get(map, P).
                new_arg_desc.value = Some(arguments_object.get_from_parameter_map(vm, property_key));
            }
        }

        // 5. Let allowed be ! OrdinaryDefineOwnProperty(args, P, newArgDesc).
        let allowed = object
            .ordinary_define_own_property(vm, property_key, &mut new_arg_desc, precomputed_get_own_property)
            .must();

        // 6. If allowed is false, return false.
        if !allowed {
            return Ok(false);
        }

        // 7. If isMapped is true, then
        if is_mapped {
            // a. If IsAccessorDescriptor(Desc) is true, then
            if descriptor.is_accessor_descriptor() {
                // i. Perform ! map.[[Delete]](P).
                arguments_object.delete_from_parameter_map(property_key);
            } else {
                // i. If Desc has a [[Value]] field, then
                if let Some(value) = descriptor.value {
                    // 1. Assert: The following Set will succeed, since formal parameters mapped by arguments objects are always writable.

                    // 2. Perform ! Set(map, P, Desc.[[Value]], false).
                    arguments_object.set_in_parameter_map(vm, property_key, value);
                }
                // ii. If Desc has a [[Writable]] field and Desc.[[Writable]] is false, then
                if descriptor.writable == Some(false) {
                    // 1. Perform ! map.[[Delete]](P).
                    arguments_object.delete_from_parameter_map(property_key);
                }
            }
        }

        // 8. Return true.
        Ok(true)
    }

    fn delete_from_parameter_map(&self, property_key: &PropertyKey) {
        self.mapped_names.borrow_mut()[property_key.as_number() as usize] = Utf16FlyString::default();
    }

    fn get_from_parameter_map(&self, vm: &Vm, property_key: &PropertyKey) -> Value {
        self.environment
            .get()
            .get_binding_value(vm, &self.mapped_name(property_key), false)
            .must()
    }

    fn set_in_parameter_map(&self, vm: &Vm, property_key: &PropertyKey, value: Value) {
        self.environment
            .get()
            .set_mutable_binding(vm, &self.mapped_name(property_key), value, false)
            .must();
    }
}
