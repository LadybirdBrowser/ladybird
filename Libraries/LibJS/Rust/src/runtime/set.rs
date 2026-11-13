/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ops::Deref;

use libjs_runtime_macros::Trace;

use crate::gc::class::{ExternalMemorySize, Finalize, GcCell, define_cell};
use crate::gc::defer_gc::DeferGc;
use crate::gc::gc_ref_cell::GcRefCell;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::keyed_collections::canonicalize_keyed_collection_key;
use crate::runtime::map::{NoValue, Position, Storage};
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;
use crate::runtime::realm::Realm;

#[repr(C)]
#[derive(Trace)]
pub struct Set {
    base: Object,
    storage: GcRefCell<Storage<NoValue>>,
}

define_cell!(
    Set,
    Object,
    extends: [Object],
    finalize: finalize,
    external_memory_size: external_memory_size
);

impl ExternalMemorySize for Set {
    fn external_memory_size(&self) -> usize {
        self.base
            .external_memory_size()
            .saturating_add(self.storage.borrow().external_memory_size())
    }
}

impl Deref for Set {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl Finalize for Set {
    fn finalize(&self) {
        Storage::release(&self.storage);
    }
}

impl Set {
    pub fn new(defer_gc: &DeferGc<'_>, vm: &Vm, realm: Gc<Realm>, prototype: Gc<Object>) -> Set {
        Set {
            base: Object::new_with_prototype(
                vm,
                Self::CLASS,
                realm,
                Some(prototype),
                MayInterfereWithIndexedPropertyAccess::No,
            ),
            storage: GcRefCell::new(Storage::new(defer_gc)),
        }
    }

    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<Set> {
        realm.create_object(vm, |defer_gc| {
            Set::new(defer_gc, vm, realm, realm.intrinsics().set_prototype(vm))
        })
    }

    // NB: Unlike what the spec says, we implement Sets with the storage of a Map that keeps no values, so all the
    //     functions below do not directly implement the operations as defined by the specification.

    pub fn set_clear(&self, vm: &Vm) {
        Storage::modify(&self.storage, vm, Storage::clear);
    }

    pub fn set_remove(&self, vm: &Vm, value: Value) -> bool {
        Storage::modify(&self.storage, vm, |storage| storage.remove(value))
    }

    pub fn set_has(&self, key: Value) -> bool {
        self.storage.borrow().contains(key)
    }

    pub fn set_add(&self, vm: &Vm, key: Value) {
        Storage::modify(&self.storage, vm, |storage| storage.set(key, NoValue));
    }

    pub fn set_size(&self) -> usize {
        self.storage.borrow().len()
    }

    /// Calls the callback with every value, in insertion order. The callback must not modify the set.
    pub fn for_each_value(&self, mut callback: impl FnMut(Value)) {
        Storage::for_each_entry(&self.storage, |key, _| callback(key));
    }

    pub fn begin(&self) -> ConstIterator {
        ConstIterator {
            set: Gc::from_ref(self),
            position: Position::first(&self.storage.borrow()),
        }
    }

    pub fn copy(&self, vm: &Vm) -> Gc<Set> {
        let realm = vm.current_realm().expect("a Set is copied in a realm");
        // FIXME: This is inefficient: it searches the result for every value. Copy the live entries of the storage
        //        instead.
        let result = Set::create(vm, realm);
        let iterator = self.begin();
        while !iterator.is_end() {
            result.set_add(vm, iterator.current());
            iterator.advance();
        }
        result
    }
}

/// An iterator that stays valid while the set is modified, with the visiting rules of Position.
#[derive(Trace)]
pub struct ConstIterator {
    set: Gc<Set>,
    position: Position,
}

impl ConstIterator {
    pub fn is_end(&self) -> bool {
        self.position.is_end(&self.set.storage.borrow())
    }

    /// operator++.
    pub fn advance(&self) {
        self.position.advance(&self.set.storage.borrow());
    }

    /// operator*.
    pub fn current(&self) -> Value {
        self.position.current(&self.set.storage.borrow()).0
    }
}

// 24.2.1.1 Set Records, https://tc39.es/ecma262/#sec-set-records
pub struct SetRecord {
    pub set_object: Gc<Object>,   // [[SetObject]]
    pub size: f64,                // [[Size]
    pub has: Gc<FunctionObject>,  // [[Has]]
    pub keys: Gc<FunctionObject>, // [[Keys]]
}

// 24.2.1.2 GetSetRecord ( obj ), https://tc39.es/ecma262/#sec-getsetrecord
pub fn get_set_record(vm: &Vm, value: Value) -> ThrowCompletionOr<SetRecord> {
    // 1. If obj is not an Object, throw a TypeError exception.
    if !value.is_object() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObject, &[&value]);
    }
    let object = value.as_object();

    // 2. Let rawSize be ? Get(obj, "size").
    let raw_size = object.get(vm, &vm.names.size)?;

    // 3. Let numSize be ? ToNumber(rawSize).
    let number_size = raw_size.to_number(vm)?;

    // 4. NOTE: If rawSize is undefined, then numSize will be NaN.
    // 5. If numSize is NaN, throw a TypeError exception.
    if number_size.is_nan() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NumberIsNaN, &[&"size"]);
    }

    // 6. Let intSize be ! ToIntegerOrInfinity(numSize).
    let integer_size = number_size.to_integer_or_infinity(vm).must();

    // 7. If intSize < 0, throw a RangeError exception.
    if integer_size < 0.0 {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::NumberIsNegative, &[&"size"]);
    }

    // 8. Let has be ? Get(obj, "has").
    let has = object.get(vm, &vm.names.has)?;

    // 9. If IsCallable(has) is false, throw a TypeError exception.
    if !has.is_function() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&has]);
    }

    // 10. Let keys be ? Get(obj, "keys").
    let keys = object.get(vm, &vm.names.keys)?;

    // 11. If IsCallable(keys) is false, throw a TypeError exception.
    if !keys.is_function() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&keys]);
    }

    // 12. Return a new Set Record { [[SetObject]]: obj, [[Size]]: intSize, [[Has]]: has, [[Keys]]: keys }.
    Ok(SetRecord {
        set_object: object,
        size: integer_size,
        has: has.as_function(),
        keys: keys.as_function(),
    })
}

// 24.2.1.3 SetDataHas ( setData, value ), https://tc39.es/ecma262/#sec-setdatahas
pub fn set_data_has(set_data: Gc<Set>, value: Value) -> bool {
    // NOTE: We do not need to implement SetDataIndex, as we do not implement the use of empty slots in Set. But we do
    //       need to match its behavior of always canonicalizing the provided value.
    let value = canonicalize_keyed_collection_key(value);

    // 1. If SetDataIndex(setData, value) is not-found, return false.
    // 2. Return true.
    set_data.set_has(value)
}
