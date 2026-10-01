/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::fmt;

use libjs_runtime_macros::Trace;

use crate::layout::cell::Gc;
use crate::layout::function_object::FunctionObject;
use crate::layout::value::Value;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};

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
