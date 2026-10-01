/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::fmt;

use libjs_runtime_macros::Trace;

use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::function_object::FunctionObject;
use crate::layout::value::Value;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::object::Object;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};

// 6.2.5.4 FromPropertyDescriptor ( Desc ), https://tc39.es/ecma262/#sec-frompropertydescriptor
pub fn from_property_descriptor(vm: &Vm, property_descriptor: &Option<PropertyDescriptor>) -> Value {
    let Some(property_descriptor) = property_descriptor else {
        return Value::UNDEFINED;
    };
    let realm = vm
        .current_realm()
        .expect("there is a current realm to create the descriptor object in");
    let object = Object::create(vm, realm, Some(realm.object_prototype()));
    let function_or_undefined =
        |function: Option<Gc<FunctionObject>>| function.map_or(Value::UNDEFINED, Value::from_object);
    if let Some(value) = property_descriptor.value {
        object.create_data_property_or_throw(vm, &vm.names.value, value).must();
    }
    if let Some(writable) = property_descriptor.writable {
        object
            .create_data_property_or_throw(vm, &vm.names.writable, Value::from_bool(writable))
            .must();
    }
    if let Some(get) = property_descriptor.get {
        object
            .create_data_property_or_throw(vm, &vm.names.get, function_or_undefined(get))
            .must();
    }
    if let Some(set) = property_descriptor.set {
        object
            .create_data_property_or_throw(vm, &vm.names.set, function_or_undefined(set))
            .must();
    }
    if let Some(enumerable) = property_descriptor.enumerable {
        object
            .create_data_property_or_throw(vm, &vm.names.enumerable, Value::from_bool(enumerable))
            .must();
    }
    if let Some(configurable) = property_descriptor.configurable {
        object
            .create_data_property_or_throw(vm, &vm.names.configurable, Value::from_bool(configurable))
            .must();
    }
    Value::from_object(object)
}

// 6.2.5.5 ToPropertyDescriptor ( Obj ), https://tc39.es/ecma262/#sec-topropertydescriptor
pub fn to_property_descriptor(vm: &Vm, argument: Value) -> ThrowCompletionOr<PropertyDescriptor> {
    // 1. If Type(Obj) is not Object, throw a TypeError exception.
    if !argument.is_object() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObject, &[&argument]);
    }

    let object = argument.as_object();

    // 2. Let desc be a new Property Descriptor that initially has no fields.
    let mut descriptor = PropertyDescriptor::default();

    // 3. Let hasEnumerable be ? HasProperty(Obj, "enumerable").
    let has_enumerable = object.has_property(vm, &vm.names.enumerable)?;

    // 4. If hasEnumerable is true, then
    if has_enumerable {
        // a. Let enumerable be ToBoolean(? Get(Obj, "enumerable")).
        let enumerable = object.get(vm, &vm.names.enumerable)?.to_boolean();

        // b. Set desc.[[Enumerable]] to enumerable.
        descriptor.enumerable = Some(enumerable);
    }

    // 5. Let hasConfigurable be ? HasProperty(Obj, "configurable").
    let has_configurable = object.has_property(vm, &vm.names.configurable)?;

    // 6. If hasConfigurable is true, then
    if has_configurable {
        // a. Let configurable be ToBoolean(? Get(Obj, "configurable")).
        let configurable = object.get(vm, &vm.names.configurable)?.to_boolean();

        // b. Set desc.[[Configurable]] to configurable.
        descriptor.configurable = Some(configurable);
    }

    // 7. Let hasValue be ? HasProperty(Obj, "value").
    let has_value = object.has_property(vm, &vm.names.value)?;

    // 8. If hasValue is true, then
    if has_value {
        // a. Let value be ? Get(Obj, "value").
        let value = object.get(vm, &vm.names.value)?;

        // b. Set desc.[[Value]] to value.
        descriptor.value = Some(value);
    }

    // 9. Let hasWritable be ? HasProperty(Obj, "writable").
    let has_writable = object.has_property(vm, &vm.names.writable)?;

    // 10. If hasWritable is true, then
    if has_writable {
        // a. Let writable be ToBoolean(? Get(Obj, "writable")).
        let writable = object.get(vm, &vm.names.writable)?.to_boolean();

        // b. Set desc.[[Writable]] to writable.
        descriptor.writable = Some(writable);
    }

    // 11. Let hasGet be ? HasProperty(Obj, "get").
    let has_get = object.has_property(vm, &vm.names.get)?;

    // 12. If hasGet is true, then
    if has_get {
        // a. Let getter be ? Get(Obj, "get").
        let getter = object.get(vm, &vm.names.get)?;

        // b. If IsCallable(getter) is false and getter is not undefined, throw a TypeError exception.
        if !getter.is_function() && !getter.is_undefined() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::AccessorBadField, &[&"get"]);
        }

        // c. Set desc.[[Get]] to getter.
        descriptor.get = Some(getter.is_function().then(|| getter.as_function()));
    }

    // 13. Let hasSet be ? HasProperty(Obj, "set").
    let has_set = object.has_property(vm, &vm.names.set)?;

    // 14. If hasSet is true, then
    if has_set {
        // a. Let setter be ? Get(Obj, "set").
        let setter = object.get(vm, &vm.names.set)?;

        // b. If IsCallable(setter) is false and setter is not undefined, throw a TypeError exception.
        if !setter.is_function() && !setter.is_undefined() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::AccessorBadField, &[&"set"]);
        }

        // c. Set desc.[[Set]] to setter.
        descriptor.set = Some(setter.is_function().then(|| setter.as_function()));
    }

    // 15. If desc has a [[Get]] field or desc has a [[Set]] field, then
    if descriptor.get.is_some() || descriptor.set.is_some() {
        // a. If desc has a [[Value]] field or desc has a [[Writable]] field, throw a TypeError exception.
        if descriptor.value.is_some() || descriptor.writable.is_some() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::AccessorValueOrWritable, &[]);
        }
    }

    // 16. Return desc.
    Ok(descriptor)
}

// 6.2.5 The Property Descriptor Specification Type, https://tc39.es/ecma262/#sec-property-descriptor-specification-type
#[derive(Clone, Copy, Default, Trace)]
pub struct PropertyDescriptor {
    pub value: Option<Value>,
    pub get: Option<Option<Gc<FunctionObject>>>,
    pub set: Option<Option<Gc<FunctionObject>>>,
    pub writable: Option<bool>,
    pub enumerable: Option<bool>,
    pub configurable: Option<bool>,
    pub property_offset: Option<u32>,
}

impl PropertyDescriptor {
    // 6.2.5.1 IsAccessorDescriptor ( Desc ), https://tc39.es/ecma262/#sec-isaccessordescriptor
    pub fn is_accessor_descriptor(&self) -> bool {
        // 1. If Desc is undefined, return false.

        // 2. If Desc has a [[Get]] field, return true.
        if self.get.is_some() {
            return true;
        }

        // 3. If Desc has a [[Set]] field, return true.
        if self.set.is_some() {
            return true;
        }

        // 4. Return false.
        false
    }

    // 6.2.5.2 IsDataDescriptor ( Desc ), https://tc39.es/ecma262/#sec-isdatadescriptor
    pub fn is_data_descriptor(&self) -> bool {
        // 1. If Desc is undefined, return false.

        // 2. If Desc has a [[Value]] field, return true.
        if self.value.is_some() {
            return true;
        }

        // 3. If Desc has a [[Writable]] field, return true.
        if self.writable.is_some() {
            return true;
        }

        // 4. Return false.
        false
    }

    // 6.2.5.3 IsGenericDescriptor ( Desc ), https://tc39.es/ecma262/#sec-isgenericdescriptor
    pub fn is_generic_descriptor(&self) -> bool {
        // 1. If Desc is undefined, return false.

        // 2. If IsAccessorDescriptor(Desc) is true, return false.
        if self.is_accessor_descriptor() {
            return false;
        }

        // 3. If IsDataDescriptor(Desc) is true, return false.
        if self.is_data_descriptor() {
            return false;
        }

        // 4. Return true.
        true
    }

    // 6.2.5.6 CompletePropertyDescriptor ( Desc ), https://tc39.es/ecma262/#sec-completepropertydescriptor
    pub fn complete(&mut self) {
        if self.is_generic_descriptor() || self.is_data_descriptor() {
            self.value.get_or_insert(Value::UNDEFINED);
            self.writable.get_or_insert(false);
        } else {
            self.get.get_or_insert(None);
            self.set.get_or_insert(None);
        }
        self.enumerable.get_or_insert(false);
        self.configurable.get_or_insert(false);
    }

    // Non-standard, just a convenient way to get from three Optional<bool> to PropertyAttributes.
    pub fn attributes(&self) -> PropertyAttributes {
        let mut attributes = 0;
        if self.writable.unwrap_or(false) {
            attributes |= Attribute::WRITABLE;
        }
        if self.enumerable.unwrap_or(false) {
            attributes |= Attribute::ENUMERABLE;
        }
        if self.configurable.unwrap_or(false) {
            attributes |= Attribute::CONFIGURABLE;
        }
        PropertyAttributes::new(attributes)
    }

    // Not a standard abstract operation, but "If every field in Desc is absent".
    pub fn is_empty(&self) -> bool {
        self.value.is_none()
            && self.get.is_none()
            && self.set.is_none()
            && self.writable.is_none()
            && self.enumerable.is_none()
            && self.configurable.is_none()
    }
}

impl fmt::Debug for PropertyDescriptor {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        let function_address =
            |function: Option<Gc<FunctionObject>>| function.map_or(core::ptr::null_mut(), |function| function.as_ptr());
        let mut parts = Vec::new();
        if let Some(value) = self.value {
            parts.push(format!("[[Value]]: {value:?}"));
        }
        if let Some(get) = self.get {
            parts.push(format!("[[Get]]: JS::Function* @ {:p}", function_address(get)));
        }
        if let Some(set) = self.set {
            parts.push(format!("[[Set]]: JS::Function* @ {:p}", function_address(set)));
        }
        if let Some(writable) = self.writable {
            parts.push(format!("[[Writable]]: {writable}"));
        }
        if let Some(enumerable) = self.enumerable {
            parts.push(format!("[[Enumerable]]: {enumerable}"));
        }
        if let Some(configurable) = self.configurable {
            parts.push(format!("[[Configurable]]: {configurable}"));
        }
        write!(formatter, "PropertyDescriptor {{ {} }}", parts.join(", "))
    }
}
