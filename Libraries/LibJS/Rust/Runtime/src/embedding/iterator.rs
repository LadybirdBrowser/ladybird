/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Iterators and iterator result objects.
//!
//! The functions here follow the contract object.rs states for the embedding module. The operations on an Iterator
//! Record call the iterator's methods, which may run any JavaScript.

#![allow(
    clippy::missing_safety_doc,
    reason = "object.rs states the contract every exported function shares"
)]

use crate::embedding::abi_types::{
    JSRealm, append_to_value_sink, cell_from_abi, completion_into_abi, completion_writing_result_to, object_into_abi,
    optional_cell_from_abi, vm_from_abi,
};
use crate::embedding::object::function_from_abi;
use crate::layout::host_class::{JSCompletion, JSObject, JSVM, JSValue, JSValueSink};
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::iterator::{
    IteratorHint, IteratorRecordImpl, create_iterator_result_object, get_iterator_from_method_impl, get_iterator_impl,
    iterator_complete, iterator_next, iterator_step_value, iterator_to_list, iterator_value,
};

/// The kind of iterator GetIterator gets, in the order of the C++ JS::IteratorHint.
pub type JSIteratorHint = u8;

pub const JS_ITERATOR_HINT_SYNC: JSIteratorHint = 0;
pub const JS_ITERATOR_HINT_ASYNC: JSIteratorHint = 1;

fn iterator_hint_from_abi(hint: JSIteratorHint) -> IteratorHint {
    match hint {
        JS_ITERATOR_HINT_SYNC => IteratorHint::Sync,
        JS_ITERATOR_HINT_ASYNC => IteratorHint::Async,
        hint => panic!("the embedder passed an unknown iterator hint {hint}"),
    }
}

/// A normal completion whose payload is whether the step produced something, which goes to `value`, or a throw
/// completion.
///
/// # Safety
///
/// `value` must be writable.
unsafe fn step_completion_into_abi(step: ThrowCompletionOr<Option<Value>>, value: *mut JSValue) -> JSCompletion {
    completion_into_abi(step.map(|step| {
        let Some(stepped_value) = step else {
            return false;
        };
        assert!(!value.is_null(), "the embedder passes an out parameter");
        // SAFETY: The caller guarantees that `value` is writable.
        unsafe { value.write(stepped_value.0) };
        true
    }))
}

/// IteratorComplete ( iteratorResult ), whose payload is the bool of its "done". Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_iterator_complete(vm: *mut JSVM, iterator_result: *mut JSObject) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, iterator_result) = unsafe { (vm_from_abi(vm), cell_from_abi::<JSObject>(iterator_result)) };
    completion_into_abi(iterator_complete(vm, iterator_result))
}

/// IteratorValue ( iteratorResult ), whose payload is its "value". Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_iterator_value(vm: *mut JSVM, iterator_result: *mut JSObject) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, iterator_result) = unsafe { (vm_from_abi(vm), cell_from_abi::<JSObject>(iterator_result)) };
    completion_into_abi(iterator_value(vm, iterator_result))
}

/// The fields of an Iterator Record that the embedder keeps in a cell of its own, in the order of the C++
/// IteratorRecordImpl. The operations below take such a record by its fields and update its [[Done]] in place.
#[repr(C)]
pub struct JSIteratorRecordFields {
    pub done: bool,              // [[Done]]
    pub iterator: *mut JSObject, // [[Iterator]]
    pub next_method: JSValue,    // [[NextMethod]]
}

fn iterator_record_fields_into_abi(record: &IteratorRecordImpl) -> JSIteratorRecordFields {
    JSIteratorRecordFields {
        done: record.done(),
        iterator: object_into_abi(record.iterator()),
        next_method: record.next_method().0,
    }
}

/// Runs `operation` on the record whose fields `fields` points to, and then writes back its [[Done]].
///
/// # Safety
///
/// `fields` must point to the readable and writable fields of a record whose iterator is a live object.
unsafe fn with_iterator_record_fields<T>(
    fields: *mut JSIteratorRecordFields,
    operation: impl FnOnce(&IteratorRecordImpl) -> T,
) -> T {
    // SAFETY: The caller passes the fields of a record.
    let fields = unsafe { fields.as_mut() }.expect("the embedder passes the fields of an iterator record");
    // SAFETY: The caller guarantees that the iterator is a live object.
    let iterator = unsafe { optional_cell_from_abi::<JSObject>(fields.iterator) };
    let record = IteratorRecordImpl::new(iterator, Value(fields.next_method), fields.done);
    let result = operation(&record);
    fields.done = record.done();
    result
}

/// GetIterator ( obj, kind ) with kind one of JS_ITERATOR_HINT_*, which writes the fields of the Iterator Record to
/// `fields`. An async iterator of an object without @@asyncIterator wraps its sync iterator, as
/// CreateAsyncFromSyncIterator does. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_iterator_get_fields(
    vm: *mut JSVM,
    value: JSValue,
    hint: JSIteratorHint,
    fields: *mut JSIteratorRecordFields,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let vm = unsafe { vm_from_abi(vm) };
    let record = get_iterator_impl(vm, Value(value), iterator_hint_from_abi(hint));
    // SAFETY: As above, `fields` is writable.
    unsafe { completion_writing_result_to(record.map(|record| iterator_record_fields_into_abi(&record)), fields) }
}

/// GetIteratorFromMethod ( obj, method ), which writes the fields of the Iterator Record to `fields`. Main thread
/// only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_iterator_get_fields_from_method(
    vm: *mut JSVM,
    value: JSValue,
    method: *mut JSObject,
    fields: *mut JSIteratorRecordFields,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, method) = unsafe { (vm_from_abi(vm), function_from_abi(method)) };
    let record = get_iterator_from_method_impl(vm, Value(value), method);
    // SAFETY: As above, `fields` is writable.
    unsafe { completion_writing_result_to(record.map(|record| iterator_record_fields_into_abi(&record)), fields) }
}

/// IteratorNext ( iteratorRecord [ , value ] ) for a record given by its fields. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_iterator_fields_next(
    vm: *mut JSVM,
    fields: *mut JSIteratorRecordFields,
    value: *const JSValue,
) -> JSCompletion {
    // SAFETY: See the module documentation; `value` is null or readable.
    let (vm, value) = unsafe { (vm_from_abi(vm), value.as_ref().map(|value| Value(*value))) };
    // SAFETY: As above, the fields are those of a record.
    completion_into_abi(unsafe { with_iterator_record_fields(fields, |record| iterator_next(vm, record, value)) })
}

/// IteratorStepValue ( iteratorRecord ) for a record given by its fields. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_iterator_fields_step_value(
    vm: *mut JSVM,
    fields: *mut JSIteratorRecordFields,
    value: *mut JSValue,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let vm = unsafe { vm_from_abi(vm) };
    // SAFETY: As above, the fields are those of a record.
    let step = unsafe { with_iterator_record_fields(fields, |record| iterator_step_value(vm, record)) };
    // SAFETY: As above, `value` is writable.
    unsafe { step_completion_into_abi(step, value) }
}

/// IteratorToList ( iteratorRecord ) for a record given by its fields. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_iterator_fields_to_list(
    vm: *mut JSVM,
    fields: *mut JSIteratorRecordFields,
    values: *const JSValueSink,
) -> JSCompletion {
    // SAFETY: See the module documentation; the sink is readable.
    let (vm, sink) = unsafe { (vm_from_abi(vm), values.as_ref().expect("the embedder passes a sink")) };
    // SAFETY: As above, the fields are those of a record.
    let list = unsafe { with_iterator_record_fields(fields, |record| iterator_to_list(vm, record)) };
    completion_into_abi(list.map(|list| {
        for index in 0..list.len() {
            let value = list.get(index).expect("the index is within the list");
            append_to_value_sink(sink, value);
        }
    }))
}

/// CreateIteratorResultObject ( value, done ), an object of the realm's %Object.prototype%. Returns an unrooted object.
/// Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_iterator_create_result_object(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    value: JSValue,
    done: bool,
) -> *mut JSObject {
    // SAFETY: See the module documentation.
    let (vm, realm) = unsafe { (vm_from_abi(vm), cell_from_abi::<JSRealm>(realm)) };
    object_into_abi(create_iterator_result_object(vm, realm, Value(value), done))
}
