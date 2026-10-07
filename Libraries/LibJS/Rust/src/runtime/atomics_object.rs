/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::time::Duration;

use libjs_runtime_macros::Trace;

use crate::futex::AtomicWaitResult;
use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::agent::agent_can_suspend;
use crate::runtime::array_buffer::{Order, ReadWriteModifyOperation, numeric_to_raw_bytes, raw_bytes_to_numeric};
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::typed_array::{
    ContentType, Kind, TypedArrayBase, TypedArrayWithBufferWitness, is_typed_array_out_of_bounds,
    make_typed_array_with_buffer_witness_record, typed_array_from, typed_array_length, validate_typed_array,
};

#[repr(C)]
#[derive(Trace)]
pub struct AtomicsObject {
    base: Object,
}

define_object_class!(AtomicsObject, extends: [Object], methods: {
    initialize: AtomicsObject::initialize,
    ..ORDINARY_OBJECT_METHODS
});

// 25.4.3.1 ValidateIntegerTypedArray ( typedArray, waitable ), https://tc39.es/ecma262/#sec-validateintegertypedarray
fn validate_integer_typed_array(
    vm: &Vm,
    typed_array: &TypedArrayBase,
    waitable: bool,
) -> ThrowCompletionOr<TypedArrayWithBufferWitness> {
    // 1. Let taRecord be ? ValidateTypedArray(typedArray, unordered).
    let typed_array_record = validate_typed_array(vm, typed_array, Order::Unordered)?;

    // 2. NOTE: Bounds checking is not a synchronizing operation when typedArray's backing buffer is a growable SharedArrayBuffer.

    let type_name = PropertyKey::from(typed_array.element_name(vm));

    // 3. If waitable is true, then
    if waitable {
        // a. If typedArray.[[TypedArrayName]] is neither "Int32Array" nor "BigInt64Array", throw a TypeError exception.
        if !matches!(typed_array.kind(), Kind::Int32Array | Kind::BigInt64Array) {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::TypedArrayTypeIsNot,
                &[&type_name, &"Int32 or BigInt64"],
            );
        }
    }
    // 4. Else,
    else {
        // a. Let type be TypedArrayElementType(typedArray).

        // b. If IsUnclampedIntegerElementType(type) is false and IsBigIntElementType(type) is false, throw a TypeError exception.
        if !typed_array.is_unclamped_integer_element_type() && !typed_array.is_bigint_element_type() {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::TypedArrayTypeIsNot,
                &[&type_name, &"an unclamped integer or BigInt"],
            );
        }
    }

    // 5. Return taRecord.
    Ok(typed_array_record)
}

// 25.4.3.2 ValidateAtomicAccess ( taRecord, requestIndex ), https://tc39.es/ecma262/#sec-validateatomicaccess
fn validate_atomic_access(
    vm: &Vm,
    typed_array_record: &TypedArrayWithBufferWitness,
    request_index: Value,
) -> ThrowCompletionOr<usize> {
    // 1. Let length be TypedArrayLength(taRecord).
    let length = typed_array_length(typed_array_record);

    // 2. Let accessIndex be ? ToIndex(requestIndex).
    // 3. Assert: accessIndex ≥ 0.
    let access_index = request_index.to_index(vm)? as usize;

    // 4. If accessIndex ≥ length, throw a RangeError exception.
    if access_index >= length as usize {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::IndexOutOfRange,
            &[&access_index, &length],
        );
    }

    // 5. Let typedArray be taRecord.[[Object]].
    let typed_array = typed_array_record.object;

    // 6. Let elementSize be TypedArrayElementSize(typedArray).
    let element_size = typed_array.element_size() as usize;

    // 7. Let offset be typedArray.[[ByteOffset]].
    let offset = typed_array.byte_offset() as usize;

    // 8. Return (accessIndex × elementSize) + offset.
    Ok((access_index * element_size) + offset)
}

// 25.4.3.3 ValidateAtomicAccessOnIntegerTypedArray ( typedArray, requestIndex [ , waitable ] ), https://tc39.es/ecma262/#sec-validateatomicaccessonintegertypedarray
fn validate_atomic_access_on_integer_typed_array(
    vm: &Vm,
    typed_array: &TypedArrayBase,
    request_index: Value,
    waitable: bool,
) -> ThrowCompletionOr<usize> {
    // 1. If waitable is not present, set waitable to false.

    // 2. Let taRecord be ? ValidateIntegerTypedArray(typedArray, waitable).
    let typed_array_record = validate_integer_typed_array(vm, typed_array, waitable)?;

    // 3. Return ? ValidateAtomicAccess(taRecord, requestIndex).
    validate_atomic_access(vm, &typed_array_record, request_index)
}

// 25.4.3.4 RevalidateAtomicAccess ( typedArray, byteIndexInBuffer ), https://tc39.es/ecma262/#sec-revalidateatomicaccess
fn revalidate_atomic_access(
    vm: &Vm,
    typed_array: &TypedArrayBase,
    byte_index_in_buffer: usize,
) -> ThrowCompletionOr<()> {
    // 1. Let taRecord be MakeTypedArrayWithBufferWitnessRecord(typedArray, unordered).
    let typed_array_record = make_typed_array_with_buffer_witness_record(typed_array, Order::Unordered);

    // 2. NOTE: Bounds checking is not a synchronizing operation when typedArray's backing buffer is a growable SharedArrayBuffer.
    // 3. If IsTypedArrayOutOfBounds(taRecord) is true, throw a TypeError exception.
    if is_typed_array_out_of_bounds(&typed_array_record) {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::BufferOutOfBounds, &[&"TypedArray"]);
    }

    // 4. Assert: byteIndexInBuffer ≥ typedArray.[[ByteOffset]].
    assert!(byte_index_in_buffer >= typed_array.byte_offset() as usize);

    // 5. If byteIndexInBuffer ≥ taRecord.[[CachedBufferByteLength]], throw a RangeError exception.
    // AD-HOC: The spec step strictly only bounds-checks the element's first byte. A length-tracking element whose start
    //         is in bounds but end is past a buffer shrunk during argument coercion would otherwise reach the buffer
    //         accessors and read/write out of bounds, violating their sufficient-bytes assertion. Bound-check the whole
    //         element instead. That aligns with what V8, JSC, and SpiderMonkey already are all also functionally doing.
    //         (The OOB access from #10759 isn’t reproducible in any of those engines.)
    //         https://github.com/tc39/ecma262/issues/3924
    let buffer_byte_length = typed_array_record.cached_buffer_byte_length.length();
    if byte_index_in_buffer + typed_array.element_size() as usize > buffer_byte_length as usize {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::IndexOutOfRange,
            &[&byte_index_in_buffer, &buffer_byte_length],
        );
    }

    // 6. Return unused.
    Ok(())
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum WaitMode {
    Sync,
    Async,
}

// 25.4.3.14 DoWait ( mode, typedArray, index, value, timeout ), https://tc39.es/ecma262/#sec-dowait
fn do_wait(
    vm: &Vm,
    mode: WaitMode,
    typed_array: &TypedArrayBase,
    index: Value,
    expected_value: Value,
    timeout_value: Value,
) -> ThrowCompletionOr<Value> {
    // 1. Let taRecord be ? ValidateIntegerTypedArray(typedArray, true).
    let typed_array_record = validate_integer_typed_array(vm, typed_array, true)?;

    // 2. Let buffer be taRecord.[[Object]].[[ViewedArrayBuffer]].
    let buffer = typed_array_record.object.viewed_array_buffer();

    // 3. If IsSharedArrayBuffer(buffer) is false, throw a TypeError exception.
    if !buffer.is_shared_array_buffer() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotASharedArrayBuffer, &[]);
    }

    // 4. Let byteIndexInBuffer be ? ValidateAtomicAccess(taRecord, index).
    let byte_index_in_buffer = validate_atomic_access(vm, &typed_array_record, index)?;

    // 5. Let arrayTypeName be typedArray.[[TypedArrayName]].
    // 6. If arrayTypeName is "BigInt64Array", let v be ? ToBigInt64(value).
    let value: i64 = if typed_array.kind() == Kind::BigInt64Array {
        expected_value.to_bigint_int64(vm)?
    }
    // 7. Else, let v be ? ToInt32(value).
    else {
        i64::from(expected_value.to_i32(vm)?)
    };

    // 8. Let q be ? ToNumber(timeout).
    let timeout_number = timeout_value.to_number(vm)?;

    // 9. If q is either NaN or +∞𝔽, let t be +∞; else if q is -∞𝔽, let t be 0; else let t be max(ℝ(q), 0).
    let timeout = if timeout_number.is_nan() || timeout_number.is_positive_infinity() {
        f64::INFINITY
    } else if timeout_number.is_negative_infinity() {
        0.0
    } else {
        timeout_number.as_f64().max(0.0)
    };

    // 10. If mode is sync and AgentCanSuspend() is false, throw a TypeError exception.
    if mode == WaitMode::Sync && !agent_can_suspend(vm) {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::AgentCannotSuspend, &[]);
    }

    // 11. Let block be buffer.[[ArrayBufferData]].
    // 12-24. AD-HOC: Rather than maintaining the spec's WaiterList, we wait on the shared-memory word using an OS
    //        futex. The word lives in memory mapped into every agent's process — so wait and notify coordinate across
    //        processes. We don't yet implement Atomics.waitAsync (async mode).
    if mode == WaitMode::Async {
        return vm.throw_completion(
            ErrorKind::InternalError,
            ErrorType::NotImplemented,
            &[&"Atomics.waitAsync"],
        );
    }

    // t is in milliseconds. Convert to nanoseconds in floating point — so sub-millisecond timeouts survive, and guard
    // the cast: A value too large for i64 nanoseconds (or a non-finite t) means an infinite wait — rather than the UB
    // of an out-of-range double-to-integer conversion.
    let mut wait_timeout = None;
    if timeout.is_finite() {
        let timeout_ns = timeout * 1_000_000.0;
        if timeout_ns < i64::MAX as f64 {
            wait_timeout = Some(Duration::from_nanos(timeout_ns as i64 as u64));
        }
    }

    let result = buffer.atomic_wait(
        byte_index_in_buffer,
        value as u64,
        typed_array.element_size() as usize,
        wait_timeout,
    );
    let result = match result {
        // 25. If waiterRecord.[[Result]] is "not-equal", return the String "not-equal".
        AtomicWaitResult::NotEqual => "not-equal",
        // 26. If waiterRecord.[[Result]] is "timed-out", return the String "timed-out".
        AtomicWaitResult::TimedOut => "timed-out",
        // 27. Return the String "ok".
        AtomicWaitResult::Woken => "ok",
    };
    Ok(Value::from_string(PrimitiveString::create_from_utf8(vm, result)))
}

// 25.4.3.17 AtomicReadModifyWrite ( typedArray, index, value, op ), https://tc39.es/ecma262/#sec-atomicreadmodifywrite
fn atomic_read_modify_write(
    vm: &Vm,
    typed_array: &TypedArrayBase,
    index: Value,
    value: Value,
    operation: ReadWriteModifyOperation,
) -> ThrowCompletionOr<Value> {
    // 1. Let byteIndexInBuffer be ? ValidateAtomicAccessOnIntegerTypedArray(typedArray, index).
    let byte_index_in_buffer = validate_atomic_access_on_integer_typed_array(vm, typed_array, index, false)?;

    // 2. If typedArray.[[ContentType]] is bigint, let v be ? ToBigInt(value).
    let value_to_set = if typed_array.content_type() == ContentType::BigInt {
        Value::from_bigint(value.to_bigint(vm)?)
    }
    // 3. Otherwise, let v be 𝔽(? ToIntegerOrInfinity(value)).
    else {
        Value::from_f64(value.to_integer_or_infinity(vm)?)
    };

    // 4. Perform ? RevalidateAtomicAccess(typedArray, byteIndexInBuffer).
    revalidate_atomic_access(vm, typed_array, byte_index_in_buffer)?;

    // 5. Let buffer be typedArray.[[ViewedArrayBuffer]].
    // 6. Let elementType be TypedArrayElementType(typedArray).
    // 7. Return GetModifySetValueInBuffer(buffer, byteIndexInBuffer, elementType, v, op).
    Ok(typed_array.get_modify_set_value_in_buffer(vm, byte_index_in_buffer, value_to_set, operation))
}

fn perform_atomic_operation(vm: &Vm, operation: ReadWriteModifyOperation) -> ThrowCompletionOr<Value> {
    let typed_array = typed_array_from(vm, vm.argument(0))?;
    let index = vm.argument(1);
    let value = vm.argument(2);

    atomic_read_modify_write(vm, &typed_array, index, value, operation)
}

impl AtomicsObject {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<AtomicsObject> {
        realm.create_object(
            vm,
            AtomicsObject {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.object_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;
        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        let define = |property_key: &PropertyKey, function, length| {
            object.define_native_function(vm, realm, property_key, function, length, attr, None);
        };
        define(&names.add, raw_native!(AtomicsObject::add), 3);
        define(&names.and_, raw_native!(AtomicsObject::and), 3);
        define(&names.compareExchange, raw_native!(AtomicsObject::compare_exchange), 4);
        define(&names.exchange, raw_native!(AtomicsObject::exchange), 3);
        define(&names.isLockFree, raw_native!(AtomicsObject::is_lock_free), 1);
        define(&names.load, raw_native!(AtomicsObject::load), 2);
        define(&names.or_, raw_native!(AtomicsObject::or), 3);
        define(&names.pause, raw_native!(AtomicsObject::pause), 0);
        define(&names.store, raw_native!(AtomicsObject::store), 3);
        define(&names.sub, raw_native!(AtomicsObject::sub), 3);
        define(&names.wait, raw_native!(AtomicsObject::wait), 4);
        define(&names.waitAsync, raw_native!(AtomicsObject::wait_async), 4);
        define(&names.notify, raw_native!(AtomicsObject::notify), 3);
        define(&names.xor_, raw_native!(AtomicsObject::xor), 3);

        // 25.4.18 Atomics [ %Symbol.toStringTag% ], https://tc39.es/ecma262/#sec-atomics-@@tostringtag
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_utf8(vm, "Atomics")),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 25.4.4 Atomics.add ( typedArray, index, value ), https://tc39.es/ecma262/#sec-atomics.add
    fn add(vm: &Vm) -> ThrowCompletionOr<Value> {
        perform_atomic_operation(vm, ReadWriteModifyOperation::Add)
    }

    // 25.4.5 Atomics.and ( typedArray, index, value ), https://tc39.es/ecma262/#sec-atomics.and
    fn and(vm: &Vm) -> ThrowCompletionOr<Value> {
        perform_atomic_operation(vm, ReadWriteModifyOperation::And)
    }

    // 25.4.6 Atomics.compareExchange ( typedArray, index, expectedValue, replacementValue ), https://tc39.es/ecma262/#sec-atomics.compareexchange
    fn compare_exchange(vm: &Vm) -> ThrowCompletionOr<Value> {
        let typed_array = typed_array_from(vm, vm.argument(0))?;
        let index = vm.argument(1);
        let expected_value = vm.argument(2);
        let replacement_value = vm.argument(3);

        // 1. Let byteIndexInBuffer be ? ValidateAtomicAccessOnIntegerTypedArray(typedArray, index).
        let byte_index_in_buffer = validate_atomic_access_on_integer_typed_array(vm, &typed_array, index, false)?;

        // 4. If typedArray.[[ContentType]] is bigint, then
        let (expected, replacement) = if typed_array.content_type() == ContentType::BigInt {
            (
                // a. Let expected be ? ToBigInt(expectedValue).
                Value::from_bigint(expected_value.to_bigint(vm)?),
                // b. Let replacement be ? ToBigInt(replacementValue).
                Value::from_bigint(replacement_value.to_bigint(vm)?),
            )
        }
        // 5. Else,
        else {
            (
                // a. Let expected be 𝔽(? ToIntegerOrInfinity(expectedValue)).
                Value::from_f64(expected_value.to_integer_or_infinity(vm)?),
                // b. Let replacement be 𝔽(? ToIntegerOrInfinity(replacementValue)).
                Value::from_f64(replacement_value.to_integer_or_infinity(vm)?),
            )
        };

        // 6. Perform ? RevalidateAtomicAccess(typedArray, byteIndexInBuffer).
        revalidate_atomic_access(vm, &typed_array, byte_index_in_buffer)?;

        // NOTE: We defer steps 2 and 3 to ensure we have revalidated the TA before accessing these internal slots.
        //       In our implementation, accessing [[ArrayBufferData]] on a detached buffer will fail assertions.

        // 2. Let buffer be typedArray.[[ViewedArrayBuffer]].
        let buffer = typed_array.viewed_array_buffer();

        // 7. Let elementType be TypedArrayElementType(typedArray).
        let element_type = typed_array.kind().element_type();

        // 8. Let elementSize be TypedArrayElementSize(typedArray).
        let element_size = element_type.size();

        // 9. Let isLittleEndian be the value of the [[LittleEndian]] field of the surrounding agent's Agent Record.
        let is_little_endian = cfg!(target_endian = "little");

        // 10. Let expectedBytes be NumericToRawBytes(elementType, expected, isLittleEndian).
        let expected_bytes = numeric_to_raw_bytes(vm, element_type, expected, is_little_endian);

        // 11. Let replacementBytes be NumericToRawBytes(elementType, replacement, isLittleEndian).
        let replacement_bytes = numeric_to_raw_bytes(vm, element_type, replacement, is_little_endian);

        // 12. If IsSharedArrayBuffer(buffer) is true, then
        //     a. Let rawBytesRead be AtomicCompareExchangeInSharedBlock(block, byteIndexInBuffer, elementSize, expectedBytes, replacementBytes).
        // 13. Else,
        //     a. Let rawBytesRead be a List of length elementSize whose elements are the sequence of elementSize bytes starting with block[byteIndexInBuffer].
        //     b. If ByteListEqual(rawBytesRead, expectedBytes) is true, then
        //        i. Store the individual bytes of replacementBytes into block, starting at block[byteIndexInBuffer].
        // AD-HOC: A single sequentially-consistent compare-exchange on the live block implements both branches: for a
        //         shared block, it's the required atomic operation (step 12.a); for a non-shared block, there's no
        //         concurrency — so it's equivalent to the plain read-compare-store (steps 13.a-b).
        let mut raw_bytes_read =
            buffer.atomic_compare_exchange(byte_index_in_buffer, element_size, expected_bytes, replacement_bytes);

        // 14. Return RawBytesToNumeric(elementType, rawBytesRead, isLittleEndian).
        Ok(raw_bytes_to_numeric(
            vm,
            element_type,
            &mut raw_bytes_read[..element_size],
            is_little_endian,
        ))
    }

    // 25.4.7 Atomics.exchange ( typedArray, index, value ), https://tc39.es/ecma262/#sec-atomics.exchange
    fn exchange(vm: &Vm) -> ThrowCompletionOr<Value> {
        perform_atomic_operation(vm, ReadWriteModifyOperation::Exchange)
    }

    // 25.4.8 Atomics.isLockFree ( size ), https://tc39.es/ecma262/#sec-atomics.islockfree
    fn is_lock_free(vm: &Vm) -> ThrowCompletionOr<Value> {
        let size = vm.argument(0).to_integer_or_infinity(vm)?;
        if size == 1.0 {
            return Ok(Value::from_bool(cfg!(target_has_atomic = "8")));
        }
        if size == 2.0 {
            return Ok(Value::from_bool(cfg!(target_has_atomic = "16")));
        }
        if size == 4.0 {
            return Ok(Value::TRUE);
        }
        if size == 8.0 {
            return Ok(Value::from_bool(cfg!(target_has_atomic = "64")));
        }
        Ok(Value::FALSE)
    }

    // 25.4.9 Atomics.load ( typedArray, index ), https://tc39.es/ecma262/#sec-atomics.load
    fn load(vm: &Vm) -> ThrowCompletionOr<Value> {
        let typed_array = typed_array_from(vm, vm.argument(0))?;
        let index = vm.argument(1);

        // 1. Let byteIndexInBuffer be ? ValidateAtomicAccessOnIntegerTypedArray(typedArray, index).
        let byte_index_in_buffer = validate_atomic_access_on_integer_typed_array(vm, &typed_array, index, false)?;

        // 2. Perform ? RevalidateAtomicAccess(typedArray, byteIndexInBuffer).
        revalidate_atomic_access(vm, &typed_array, byte_index_in_buffer)?;

        // 3. Let buffer be typedArray.[[ViewedArrayBuffer]].
        // 4. Let elementType be TypedArrayElementType(typedArray).
        // 5. Return GetValueFromBuffer(buffer, byteIndexInBuffer, elementType, true, seq-cst).
        Ok(typed_array.get_value_from_buffer(vm, byte_index_in_buffer, Order::SeqCst))
    }

    // 25.4.10 Atomics.notify ( typedArray, index, count ), https://tc39.es/ecma262/#sec-atomics.notify
    fn notify(vm: &Vm) -> ThrowCompletionOr<Value> {
        let typed_array = typed_array_from(vm, vm.argument(0))?;
        let index = vm.argument(1);
        let count_value = vm.argument(2);

        // 1. Let byteIndexInBuffer be ? ValidateAtomicAccessOnIntegerTypedArray(typedArray, index, true).
        let byte_index_in_buffer = validate_atomic_access_on_integer_typed_array(vm, &typed_array, index, true)?;

        // 2. If count is undefined, then
        let count = if count_value.is_undefined() {
            // a. Let c be +∞.
            f64::INFINITY
        }
        // 3. Else,
        else {
            // a. Let intCount be ? ToIntegerOrInfinity(count).
            let int_count = count_value.to_integer_or_infinity(vm)?;

            // b. Let c be max(intCount, 0).
            int_count.max(0.0)
        };

        // 4. Let buffer be typedArray.[[ViewedArrayBuffer]].
        let buffer = typed_array.viewed_array_buffer();

        // 6. If IsSharedArrayBuffer(buffer) is false, return +0𝔽.
        if !buffer.is_shared_array_buffer() {
            return Ok(Value::from_i32(0));
        }

        // 7-15. AD-HOC: Wake up to 'count' agents waiting on the shared-memory word via the OS futex (see do_wait).
        // count is >= 0 (step 3b). But a huge finite value would overflow size_t (UB on the cast). So, treat anything at or
        // beyond size_t's range (and non-finite counts) as "wake every waiter".
        let max_count = if count.is_finite() && count < usize::MAX as f64 {
            count as usize
        } else {
            usize::MAX
        };
        let woken = buffer.atomic_notify(byte_index_in_buffer, typed_array.element_size() as usize, max_count);

        // 16. Return 𝔽(n).
        Ok(Value::from_f64(woken as f64))
    }

    // 25.4.11 Atomics.or ( typedArray, index, value ), https://tc39.es/ecma262/#sec-atomics.or
    fn or(vm: &Vm) -> ThrowCompletionOr<Value> {
        perform_atomic_operation(vm, ReadWriteModifyOperation::Or)
    }

    // 25.4.12 Atomics.pause ( ), https://tc39.es/ecma262/#sec-atomics.pause
    #[allow(clippy::unnecessary_wraps, reason = "native functions return a completion")]
    fn pause(_vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If the execution environment of the ECMAScript implementation supports signaling to the operating system or
        //    CPU that the current executing code is in a spin-wait loop, send that signal.
        core::hint::spin_loop();

        // 2. Return undefined.
        Ok(Value::UNDEFINED)
    }

    // 25.4.13 Atomics.store ( typedArray, index, value ), https://tc39.es/ecma262/#sec-atomics.store
    fn store(vm: &Vm) -> ThrowCompletionOr<Value> {
        let typed_array = typed_array_from(vm, vm.argument(0))?;
        let index = vm.argument(1);
        let mut value = vm.argument(2);

        // 1. Let byteIndexInBuffer be ? ValidateAtomicAccessOnIntegerTypedArray(typedArray, index).
        let byte_index_in_buffer = validate_atomic_access_on_integer_typed_array(vm, &typed_array, index, false)?;

        // 2. If typedArray.[[ContentType]] is bigint, let v be ? ToBigInt(value).
        if typed_array.content_type() == ContentType::BigInt {
            value = Value::from_bigint(value.to_bigint(vm)?);
        }
        // 3. Otherwise, let v be 𝔽(? ToIntegerOrInfinity(value)).
        else {
            value = Value::from_f64(value.to_integer_or_infinity(vm)?);
        }

        // 4. Perform ? RevalidateAtomicAccess(typedArray, byteIndexInBuffer).
        revalidate_atomic_access(vm, &typed_array, byte_index_in_buffer)?;

        // 5. Let buffer be typedArray.[[ViewedArrayBuffer]].
        // 6. Let elementType be TypedArrayElementType(typedArray).
        // 7. Perform SetValueInBuffer(buffer, byteIndexInBuffer, elementType, v, true, seq-cst).
        typed_array.set_value_in_buffer(vm, byte_index_in_buffer, value, Order::SeqCst);

        // 8. Return v.
        Ok(value)
    }

    // 25.4.14 Atomics.sub ( typedArray, index, value ), https://tc39.es/ecma262/#sec-atomics.sub
    fn sub(vm: &Vm) -> ThrowCompletionOr<Value> {
        perform_atomic_operation(vm, ReadWriteModifyOperation::Sub)
    }

    // 25.4.15 Atomics.wait ( typedArray, index, value, timeout ), https://tc39.es/ecma262/#sec-atomics.wait
    fn wait(vm: &Vm) -> ThrowCompletionOr<Value> {
        let typed_array = typed_array_from(vm, vm.argument(0))?;
        let index = vm.argument(1);
        let value = vm.argument(2);
        let timeout = vm.argument(3);

        // 1. Return ? DoWait(sync, typedArray, index, value, timeout).
        do_wait(vm, WaitMode::Sync, &typed_array, index, value, timeout)
    }

    // 25.4.16 Atomics.waitAsync ( typedArray, index, value, timeout ), https://tc39.es/ecma262/#sec-atomics.waitasync
    fn wait_async(vm: &Vm) -> ThrowCompletionOr<Value> {
        let typed_array = typed_array_from(vm, vm.argument(0))?;
        let index = vm.argument(1);
        let value = vm.argument(2);
        let timeout = vm.argument(3);

        // 1. Return ? DoWait(async, typedArray, index, value, timeout).
        do_wait(vm, WaitMode::Async, &typed_array, index, value, timeout)
    }

    // 25.4.17 Atomics.xor ( typedArray, index, value ), https://tc39.es/ecma262/#sec-atomics.xor
    fn xor(vm: &Vm) -> ThrowCompletionOr<Value> {
        perform_atomic_operation(vm, ReadWriteModifyOperation::Xor)
    }
}
