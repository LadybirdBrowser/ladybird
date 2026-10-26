/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! IDBKeyRange, https://w3c.github.io/IndexedDB/#keyrange

use std::rc::Rc;

use ak::Utf16String;
use libjs_rust::api::{Trace, Value, Vm, is_host_instance_of};

use crate::bindings::interface::{Interface, Ref, Wrappable, allocate, implementation_of};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::indexed_db::internal::algorithms::{convert_a_key_to_a_value, convert_a_value_to_a_key};
use crate::indexed_db::internal::key::Key;

// https://w3c.github.io/IndexedDB/#keyrange
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct IDBKeyRange {
    // A key range has an associated lower bound (null or a key).
    #[gc(untraced)]
    lower_bound: Option<Rc<Key>>,

    // A key range has an associated upper bound (null or a key).
    #[gc(untraced)]
    upper_bound: Option<Rc<Key>>,

    // A key range has an associated lower open flag. Unless otherwise stated it is false.
    lower_open: bool,

    // A key range has an associated upper open flag. Unless otherwise stated it is false.
    upper_open: bool,
}

impl Wrappable for IDBKeyRange {}

impl IDBKeyRange {
    pub(crate) fn create(
        vm: &Vm,
        lower_bound: Option<Rc<Key>>,
        upper_bound: Option<Rc<Key>>,
        lower_open: bool,
        upper_open: bool,
    ) -> Ref<IDBKeyRange> {
        allocate(vm, |_| IDBKeyRange {
            lower_bound,
            upper_bound,
            lower_open,
            upper_open,
        })
    }

    pub(crate) fn is_lower_open(&self) -> bool {
        self.lower_open
    }

    pub(crate) fn is_upper_open(&self) -> bool {
        self.upper_open
    }

    pub(crate) fn lower_key(&self) -> Option<&Rc<Key>> {
        self.lower_bound.as_ref()
    }

    pub(crate) fn upper_key(&self) -> Option<&Rc<Key>> {
        self.upper_bound.as_ref()
    }

    // https://w3c.github.io/IndexedDB/#in
    pub(crate) fn is_in_range(&self, key: &Key) -> bool {
        // A key is in a key range range if both of the following conditions are fulfilled:

        // The range’s lower bound is null, or it is less than key, or it is both equal to key and the range’s lower open flag is false.
        let lower_bound_in_range = match self.lower_key() {
            None => true,
            Some(lower_key) => Key::less_than(lower_key, key) || (Key::equals(key, lower_key) && !self.is_lower_open()),
        };

        // The range’s upper bound is null, or it is greater than key, or it is both equal to key and the range’s upper open flag is false.
        let upper_bound_in_range = match self.upper_key() {
            None => true,
            Some(upper_key) => {
                Key::greater_than(upper_key, key) || (Key::equals(key, upper_key) && !self.is_upper_open())
            }
        };

        lower_bound_in_range && upper_bound_in_range
    }

    pub(crate) fn key_range_lower(&self, cx: &mut CallContext) -> WebIdlResult<Value> {
        // The lower getter steps are to return the result of converting a key to a
        // value with this's lower bound if it is not null, or undefined otherwise.
        if let Some(lower_key) = self.lower_key() {
            return Ok(convert_a_key_to_a_value(cx, lower_key));
        }

        Ok(Value::UNDEFINED)
    }

    pub(crate) fn key_range_upper(&self, cx: &mut CallContext) -> WebIdlResult<Value> {
        // The upper getter steps are to return the result of converting a key to a
        // value with this's upper bound if it is not null, or undefined otherwise.
        if let Some(upper_key) = self.upper_key() {
            return Ok(convert_a_key_to_a_value(cx, upper_key));
        }

        Ok(Value::UNDEFINED)
    }

    // https://w3c.github.io/IndexedDB/#dom-idbkeyrange-loweropen
    pub(crate) fn lower_open(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        // The lowerOpen getter steps are to return this’s lower open flag.
        Ok(self.lower_open)
    }

    // https://w3c.github.io/IndexedDB/#dom-idbkeyrange-upperopen
    pub(crate) fn upper_open(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        // The upperOpen getter steps are to return this’s upper open flag.
        Ok(self.upper_open)
    }

    // https://w3c.github.io/IndexedDB/#dom-idbkeyrange-only
    pub(crate) fn only(cx: &mut CallContext, value: Value) -> WebIdlResult<Ref<IDBKeyRange>> {
        // 1. Let key be the result of converting a value to a key with value. Rethrow any exceptions.
        let key = convert_a_value_to_a_key(cx, value, None)?;

        // 2. If key is "invalid value" or "invalid type", throw a "DataError" DOMException.
        if key.is_invalid() {
            return Err(data_error("Value is invalid"));
        }

        // 3. Create and return a new key range containing only key.
        Ok(IDBKeyRange::create(cx.vm(), Some(key.clone()), Some(key), false, false))
    }

    // https://w3c.github.io/IndexedDB/#dom-idbkeyrange-lowerbound
    pub(crate) fn lower_bound(cx: &mut CallContext, lower: Value, open: bool) -> WebIdlResult<Ref<IDBKeyRange>> {
        // 1. Let lowerKey be the result of converting a value to a key with lower. Rethrow any exceptions.
        let key = convert_a_value_to_a_key(cx, lower, None)?;

        // 2. If lowerKey is invalid, throw a "DataError" DOMException.
        if key.is_invalid() {
            return Err(data_error("Value is invalid"));
        }

        // 3. Create and return a new key range with lower bound set to lowerKey, lower open flag set to open, upper bound set to null, and upper open flag set to true.
        Ok(IDBKeyRange::create(cx.vm(), Some(key), None, open, true))
    }

    // https://w3c.github.io/IndexedDB/#dom-idbkeyrange-upperbound
    pub(crate) fn upper_bound(cx: &mut CallContext, upper: Value, open: bool) -> WebIdlResult<Ref<IDBKeyRange>> {
        // 1. Let upperKey be the result of converting a value to a key with upper. Rethrow any exceptions.
        let key = convert_a_value_to_a_key(cx, upper, None)?;

        // 2. If upperKey is "invalid value" or "invalid type", throw a "DataError" DOMException.
        if key.is_invalid() {
            return Err(data_error("Value is invalid"));
        }

        // 3. Create and return a new key range with lower bound set to null, lower open flag set to true, upper bound set to upperKey, and upper open flag set to open.
        Ok(IDBKeyRange::create(cx.vm(), None, Some(key), true, open))
    }

    // https://w3c.github.io/IndexedDB/#dom-idbkeyrange-bound
    pub(crate) fn bound(
        cx: &mut CallContext,
        lower: Value,
        upper: Value,
        lower_open: bool,
        upper_open: bool,
    ) -> WebIdlResult<Ref<IDBKeyRange>> {
        // 1. Let lowerKey be the result of converting a value to a key with lower. Rethrow any exceptions.
        let lower_key = convert_a_value_to_a_key(cx, lower, None)?;

        // 2. If lowerKey is "invalid value" or "invalid type", throw a "DataError" DOMException.
        if lower_key.is_invalid() {
            return Err(data_error("Value is invalid"));
        }

        // 3. Let upperKey be the result of converting a value to a key with upper. Rethrow any exceptions.
        let upper_key = convert_a_value_to_a_key(cx, upper, None)?;

        // 4. If upperKey is "invalid value" or "invalid type", throw a "DataError" DOMException.
        if upper_key.is_invalid() {
            return Err(data_error("Value is invalid"));
        }

        // 5. If lowerKey is greater than upperKey, throw a "DataError" DOMException.
        if Key::less_than(&upper_key, &lower_key) {
            return Err(data_error("Lower key is greater than upper key"));
        }

        // 6. Create and return a new key range with lower bound set to lowerKey, lower open flag set to lowerOpen, upper bound set to upperKey and upper open flag set to upperOpen.
        Ok(IDBKeyRange::create(
            cx.vm(),
            Some(lower_key),
            Some(upper_key),
            lower_open,
            upper_open,
        ))
    }

    // https://w3c.github.io/IndexedDB/#dom-idbkeyrange-includes
    pub(crate) fn includes(&self, cx: &mut CallContext, key: Value) -> WebIdlResult<bool> {
        // 1. Let k be the result of converting a value to a key with key. Rethrow any exceptions.
        let converted_key = convert_a_value_to_a_key(cx, key, None)?;

        // 2. If k is "invalid value" or "invalid type", throw a "DataError" DOMException.
        if converted_key.is_invalid() {
            return Err(data_error("Value is invalid"));
        }

        // 3. Return true if k is in this range, and false otherwise.
        Ok(self.is_in_range(&converted_key))
    }
}

fn data_error(message: &str) -> Exception {
    Exception::Dom("DataError", Utf16String::from_utf8(message))
}

pub(crate) fn key_range_from_value(value: Value) -> Option<Ref<IDBKeyRange>> {
    if !value.is_object() {
        return None;
    }
    let object = value.as_object();
    if !is_host_instance_of(&object, IDBKeyRange::wrapper_class()) {
        return None;
    }
    Some(implementation_of(object))
}
