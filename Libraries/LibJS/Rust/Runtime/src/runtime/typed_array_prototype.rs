/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::gc::root::MarkedVec;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{call_function_object, checked_js_string_length_sum, species_constructor};
use crate::runtime::array::{Holes, sort_indexed_properties};
use crate::runtime::array_buffer::{ElementType, Order, clone_array_buffer};
use crate::runtime::array_iterator::ArrayIterator;
use crate::runtime::canonical_index::{CanonicalIndex, CanonicalIndexType};
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::{AkDouble, ErrorType};
use crate::runtime::function_object::FunctionObject;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{
    MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, PropertyKind, ShouldThrowExceptions,
    define_object_class,
};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::typed_array::{
    ContentType, Kind, TypedArrayBase, canonical_index_from_double, compare_typed_array_elements,
    initialize_typed_array_from_array_buffer, is_typed_array_out_of_bounds, is_valid_integer_index,
    make_typed_array_with_buffer_witness_record, typed_array_byte_length, typed_array_create,
    typed_array_create_same_type, typed_array_from, typed_array_length, typed_array_of_object, typed_array_set_element,
    validate_typed_array,
};
use crate::runtime::value::{is_strictly_equal, same_value, same_value_zero};
use crate::runtime::value_conversions::MAX_ARRAY_LIKE_INDEX;
use crate::utf16::{Utf16StringBuilder, Utf16View};

/// %TypedArray.prototype%.
#[repr(C)]
#[derive(Trace)]
pub struct TypedArrayPrototype {
    base: Object,
}

define_object_class!(TypedArrayPrototype, extends: [Object], methods: {
    initialize: TypedArrayPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

fn typed_array_from_this(vm: &Vm) -> ThrowCompletionOr<Gc<TypedArrayBase>> {
    let this_value = vm.this_value();
    typed_array_from(vm, this_value)
}

fn callback_from_args(vm: &Vm, prototype_name: &str) -> ThrowCompletionOr<Gc<FunctionObject>> {
    if vm.argument_count() < 1 {
        return vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::TypedArrayPrototypeOneArg,
            &[&prototype_name],
        );
    }
    let callback = vm.argument(0);
    if !callback.is_function() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&callback]);
    }
    Ok(callback.as_function())
}

fn index_value(index: u32) -> Value {
    Value::from_f64(f64::from(index))
}

fn size_value(index: usize) -> Value {
    Value::from_f64(index as f64)
}

// 23.2.4.1 TypedArraySpeciesCreate ( exemplar, argumentList ), https://tc39.es/ecma262/#typedarray-species-create
//
// OPTIMIZATION: When the resolved species constructor is the default intrinsic (the common case), `default_construct`
// is invoked to build the result directly, bypassing the user-observable Construct(...) call. This avoids the
// throwaway ArrayBuffer that the public TypedArray constructor allocates in its `is_object()` branch before
// overwriting it via InitializeTypedArrayFromArrayBuffer/TypedArray/etc.
fn typed_array_species_create(
    vm: &Vm,
    exemplar: &TypedArrayBase,
    default_construct: impl FnOnce() -> ThrowCompletionOr<Gc<TypedArrayBase>>,
    slow_path_arguments: &[Value],
) -> ThrowCompletionOr<Gc<TypedArrayBase>> {
    let realm = vm.current_realm().expect("a typed array is created in a realm");

    // 1. Let defaultConstructor be the intrinsic object listed in column one of Table 72 for exemplar.[[TypedArrayName]].
    let default_constructor = exemplar.intrinsic_constructor(vm, realm);

    // 2. Let constructor be ? SpeciesConstructor(exemplar, defaultConstructor).
    let constructor = species_constructor(vm, exemplar, default_constructor)?;

    let result = if constructor == default_constructor {
        // OPTIMIZATION: Same outcome as `Construct(defaultConstructor, argumentList)` would produce, but without the
        //               throwaway buffer or the constructor invocation overhead.
        default_construct()?
    } else {
        // 3. Let result be ? TypedArrayCreate(constructor, argumentList).
        typed_array_create(vm, constructor, slow_path_arguments)?
    };

    // 4. Assert: result has [[TypedArrayName]] and [[ContentType]] internal slots.
    // 5. If result.[[ContentType]] ≠ exemplar.[[ContentType]], throw a TypeError exception.
    if result.content_type() != exemplar.content_type() {
        return vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::TypedArrayContentTypeMismatch,
            &[&result.class_name(), &exemplar.class_name()],
        );
    }

    // 6. Return result.
    Ok(result)
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Direction {
    Ascending,
    Descending,
}

struct FoundValue {
    index: Option<u32>, // [[Index]]
    value: Value,       // [[Value]]
}

impl FoundValue {
    fn index_to_value(&self) -> Value {
        match self.index {
            None => Value::from_i32(-1),
            Some(index) => index_value(index),
        }
    }
}

// 23.1.3.12.1 FindViaPredicate ( O, len, direction, predicate, thisArg ), https://tc39.es/ecma262/#sec-findviapredicate
fn find_via_predicate(
    vm: &Vm,
    typed_array: Gc<TypedArrayBase>,
    length: u32,
    direction: Direction,
    this_arg: Value,
    prototype_name: &str,
) -> ThrowCompletionOr<FoundValue> {
    // 1. If IsCallable(predicate) is false, throw a TypeError exception.
    let predicate = callback_from_args(vm, prototype_name)?;

    // 2. If direction is ascending, then
    let indices: Vec<u32> = if direction == Direction::Ascending {
        // a. Let indices be a List of the integers in the interval from 0 (inclusive) to len (exclusive), in ascending order.
        (0..length).collect()
    }
    // 3. Else,
    else {
        // a. Let indices be a List of the integers in the interval from 0 (inclusive) to len (exclusive), in descending order.
        (0..length).rev().collect()
    };

    // 4. For each integer k of indices, do
    for k in indices {
        // a. Let Pk be ! ToString(𝔽(k)).
        let property_key = PropertyKey::from(k);

        // b. NOTE: If O is a TypedArray, the following invocation of Get will return a normal completion.

        // c. Let kValue be ? Get(O, Pk).
        let value = typed_array.get(vm, &property_key)?;

        // d. Let testResult be ? Call(predicate, thisArg, « kValue, 𝔽(k), O »).
        let test_result = call_function_object(
            vm,
            predicate,
            this_arg,
            &[value, index_value(k), Value::from_object(typed_array)],
        )?;

        // e. If ToBoolean(testResult) is true, return the Record { [[Index]]: 𝔽(k), [[Value]]: kValue }.
        if test_result.to_boolean() {
            return Ok(FoundValue { index: Some(k), value });
        }
    }

    // 5. Return the Record { [[Index]]: -1𝔽, [[Value]]: undefined }.
    Ok(FoundValue {
        index: None,
        value: Value::UNDEFINED,
    })
}

// NOTE: This function assumes that the index is valid within the TypedArray,
//       and that the TypedArray is not detached.
fn fast_typed_array_fill(typed_array: &TypedArrayBase, begin: u32, end: u32, element: &[u8]) {
    // NB: The range is empty when it was clamped from both sides, e.g. fill(v, -1, -3).
    //     The byte count below is unsigned, so an inverted range must not reach it.
    if begin >= end {
        return;
    }

    let element_size = element.len();
    let computed_begin = (begin as usize)
        .checked_mul(element_size)
        .and_then(|begin| begin.checked_add(typed_array.byte_offset() as usize));
    let computed_end = (end as usize)
        .checked_mul(element_size)
        .and_then(|end| end.checked_add(typed_array.byte_offset() as usize));

    let (Some(computed_begin), Some(computed_end)) = (computed_begin, computed_end) else {
        return;
    };

    let array_buffer = typed_array.viewed_array_buffer();
    if computed_begin >= array_buffer.byte_length() || computed_end > array_buffer.byte_length() {
        return;
    }

    // This fast path is only taken for a non-shared buffer (fill() routes a shared buffer through the per-element
    // SetValueInBuffer path — so a bulk memcpy never races another agent's access).
    let mut byte_index = computed_begin;
    const PATTERN_BYTE_SIZE: usize = 256;
    let pattern_element_count = (PATTERN_BYTE_SIZE / element_size).max(1);
    let pattern: Vec<u8> = element.repeat(pattern_element_count);

    let mut remaining_bytes = (end - begin) as usize * element_size;
    while remaining_bytes > 0 {
        let chunk_size = remaining_bytes.min(pattern.len());
        array_buffer.overwrite(byte_index, &pattern[..chunk_size]);
        byte_index += chunk_size;
        remaining_bytes -= chunk_size;
    }
}

fn typed_array_element_types_have_same_bit_encoding(source: &TypedArrayBase, target: &TypedArrayBase) -> bool {
    if source.kind() == target.kind() {
        return true;
    }

    if (source.kind() == Kind::Uint8Array && target.kind() == Kind::Uint8ClampedArray)
        || (source.kind() == Kind::Uint8ClampedArray && target.kind() == Kind::Uint8Array)
    {
        return true;
    }

    if source.element_size() != target.element_size() {
        return false;
    }

    if source.is_unclamped_integer_element_type() && target.is_unclamped_integer_element_type() {
        return true;
    }

    source.is_bigint_element_type() && target.is_bigint_element_type()
}

// 23.2.3.26.2 SetTypedArrayFromTypedArray ( target, targetOffset, source ), https://tc39.es/ecma262/#sec-settypedarrayfromtypedarray
fn set_typed_array_from_typed_array(
    vm: &Vm,
    target: &TypedArrayBase,
    target_offset: f64,
    source: &TypedArrayBase,
) -> ThrowCompletionOr<()> {
    // 1. Let targetBuffer be target.[[ViewedArrayBuffer]].
    let target_buffer = target.viewed_array_buffer();

    // 2. Let targetRecord be ? ValidateTypedArrayBounds(target, seq-cst).
    let target_record = make_typed_array_with_buffer_witness_record(target, Order::SeqCst);
    if is_typed_array_out_of_bounds(&target_record) {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::BufferOutOfBounds, &[&"TypedArray"]);
    }

    // 3. Let targetLength be TypedArrayLength(targetRecord).
    let target_length = typed_array_length(&target_record);

    // 4. Let sourceBuffer be source.[[ViewedArrayBuffer]].
    let mut source_buffer = source.viewed_array_buffer();

    // 5. Let sourceRecord be ? ValidateTypedArrayBounds(source, seq-cst).
    let source_record = make_typed_array_with_buffer_witness_record(source, Order::SeqCst);
    if is_typed_array_out_of_bounds(&source_record) {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::BufferOutOfBounds, &[&"TypedArray"]);
    }

    // 6. Let sourceLength be TypedArrayLength(sourceRecord).
    let source_length = typed_array_length(&source_record);

    // 7. Let targetType be TypedArrayElementType(target).
    // 8. Let targetElementSize be TypedArrayElementSize(target).
    let target_element_size = target.element_size() as usize;

    // 9. Let targetByteOffset be target.[[ByteOffset]].
    let target_byte_offset = target.byte_offset() as usize;

    // 10. Let sourceType be TypedArrayElementType(source).
    let source_type = source.kind().element_type();

    // 11. Let sourceElementSize be TypedArrayElementSize(source).
    let source_element_size = source.element_size() as usize;

    // 12. Let sourceByteOffset be source.[[ByteOffset]].
    let source_byte_offset = source.byte_offset() as usize;

    // 13. If targetOffset = +∞, throw a RangeError exception.
    if target_offset == f64::INFINITY {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::TypedArrayInvalidTargetOffset,
            &[&"finite"],
        );
    }

    // 14. If sourceLength + targetOffset > targetLength, throw a RangeError exception.
    if target_offset > MAX_ARRAY_LIKE_INDEX {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::TypedArrayOverflowOrOutOfBounds,
            &[&"target offset"],
        );
    }

    let checked = (source_length as usize).checked_add(target_offset as usize);
    if checked.is_none_or(|checked| checked > target_length as usize) {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::TypedArrayOverflowOrOutOfBounds,
            &[&"target length"],
        );
    }

    // 15. If target.[[ContentType]] is not source.[[ContentType]], throw a TypeError exception.
    if target.content_type() != source.content_type() {
        return vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::TypedArrayInvalidCopy,
            &[&target.class_name(), &source.class_name()],
        );
    }

    let mut same_shared_array_buffer = false;

    // 16. If IsSharedArrayBuffer(sourceBuffer) is true, IsSharedArrayBuffer(targetBuffer) is true, and sourceBuffer.[[ArrayBufferData]] is targetBuffer.[[ArrayBufferData]], let sameSharedArrayBuffer be true; else let sameSharedArrayBuffer be false.
    if source_buffer.is_shared_array_buffer()
        && target_buffer.is_shared_array_buffer()
        && source_buffer.shares_storage_with(&target_buffer)
    {
        same_shared_array_buffer = true;
    }

    let mut source_byte_index;

    // 17. If SameValue(sourceBuffer, targetBuffer) is true or sameSharedArrayBuffer is true, then
    if same_shared_array_buffer || same_value(Value::from_object(source_buffer), Value::from_object(target_buffer)) {
        // a. Let sourceByteLength be TypedArrayByteLength(sourceRecord).
        let source_byte_length = typed_array_byte_length(&source_record);

        // b. Set sourceBuffer to ? CloneArrayBuffer(sourceBuffer, sourceByteOffset, sourceByteLength).
        source_buffer = clone_array_buffer(vm, source_buffer, source_byte_offset, source_byte_length as usize)?;

        // c. Let sourceByteIndex be 0.
        source_byte_index = 0;
    }
    // 18. Else,
    else {
        // a. Let sourceByteIndex be sourceByteOffset.
        source_byte_index = source_byte_offset;
    }

    // 19. Let targetByteIndex be (targetOffset × targetElementSize) + targetByteOffset.
    let Some(mut target_byte_index) = (target_offset as usize)
        .checked_mul(target_element_size)
        .and_then(|byte_index| byte_index.checked_add(target_byte_offset))
    else {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::TypedArrayOverflow,
            &[&"target byte index"],
        );
    };

    // 20. Let limit be targetByteIndex + (targetElementSize × sourceLength).
    let Some(limit) = (source_length as usize)
        .checked_mul(target_element_size)
        .and_then(|limit| limit.checked_add(target_byte_index))
    else {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::TypedArrayOverflow, &[&"target limit"]);
    };

    // 21. If sourceType is targetType, then
    if typed_array_element_types_have_same_bit_encoding(source, target) {
        // a. NOTE: The transfer must be performed in a manner that preserves the bit-level encoding of the source data.
        // b. Repeat, while targetByteIndex < limit,
        //     i. Let value be GetValueFromBuffer(sourceBuffer, sourceByteIndex, uint8, true, unordered).
        //     ii. Perform SetValueInBuffer(targetBuffer, targetByteIndex, uint8, value, true, unordered).
        //     iii. Set sourceByteIndex to sourceByteIndex + 1.
        //     iv. Set targetByteIndex to targetByteIndex + 1.
        // OPTIMIZATION: If neither buffer is shared, a single bulk copy realizes the byte-granular Unordered events
        //               above. A shared buffer instead uses per-byte relaxed-atomic accesses so the copy never races
        //               another agent with a non-atomic memcpy. (Step 17 above cloned sourceBuffer if it aliased
        //               targetBuffer — so the two never overlap here.)
        if !source_buffer.is_shared_array_buffer() && !target_buffer.is_shared_array_buffer() {
            source_buffer.copy_data_to(
                &target_buffer,
                source_byte_index,
                target_byte_index,
                limit - target_byte_index,
            );
        } else {
            while target_byte_index < limit {
                let value =
                    source_buffer.get_value(vm, source_byte_index, ElementType::Uint8, true, Order::Unordered, true);
                target_buffer.set_value(
                    vm,
                    target_byte_index,
                    ElementType::Uint8,
                    value,
                    true,
                    Order::Unordered,
                    true,
                );
                source_byte_index += 1;
                target_byte_index += 1;
            }
        }
    }
    // 22. Else,
    else {
        // a. Repeat, while targetByteIndex < limit,
        while target_byte_index < limit {
            // i. Let value be GetValueFromBuffer(sourceBuffer, sourceByteIndex, sourceType, true, unordered).
            let value = source_buffer.get_value(vm, source_byte_index, source_type, true, Order::Unordered, true);

            // ii. Perform SetValueInBuffer(targetBuffer, targetByteIndex, targetType, value, true, unordered).
            target.set_value_in_buffer(vm, target_byte_index, value, Order::Unordered);

            // iii. Set sourceByteIndex to sourceByteIndex + sourceElementSize.
            source_byte_index += source_element_size;

            // iv. Set targetByteIndex to targetByteIndex + targetElementSize.
            target_byte_index += target_element_size;
        }
    }

    // 23. Return unused.
    Ok(())
}

// 23.2.3.26.2 SetTypedArrayFromArrayLike ( target, targetOffset, source ), https://tc39.es/ecma262/#sec-settypedarrayfromarraylike
fn set_typed_array_from_array_like(
    vm: &Vm,
    target: &TypedArrayBase,
    target_offset: f64,
    source: Value,
) -> ThrowCompletionOr<()> {
    // 1. Let targetRecord be MakeTypedArrayWithBufferWitnessRecord(target, seq-cst)
    let target_record = make_typed_array_with_buffer_witness_record(target, Order::SeqCst);

    // 2. If IsTypedArrayOutOfBounds(targetRecord) is true, throw a TypeError exception.
    if is_typed_array_out_of_bounds(&target_record) {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::BufferOutOfBounds, &[&"TypedArray"]);
    }

    // 3. Let targetLength be TypedArrayLength(targetRecord).
    let target_length = typed_array_length(&target_record);

    // 4. Let src be ? ToObject(source).
    let source_object = source.to_object(vm)?;

    // 5. Let srcLength be ? LengthOfArrayLike(src).
    let source_length = crate::runtime::abstract_operations::length_of_array_like(vm, &source_object)? as usize;

    // 6. If targetOffset = +∞, throw a RangeError exception.
    if target_offset == f64::INFINITY {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::TypedArrayInvalidTargetOffset,
            &[&"finite"],
        );
    }

    // 7. If srcLength + targetOffset > targetLength, throw a RangeError exception.
    if target_offset > MAX_ARRAY_LIKE_INDEX {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::TypedArrayOverflowOrOutOfBounds,
            &[&"target offset"],
        );
    }

    let checked = source_length.checked_add(target_offset as usize);
    if checked.is_none_or(|checked| checked > target_length as usize) {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::TypedArrayOverflowOrOutOfBounds,
            &[&"target length"],
        );
    }

    // 8. Let k be 0.
    let mut k = 0;

    // 9. Repeat, while k < srcLength,
    while k < source_length {
        // a. Let Pk be ! ToString(𝔽(k)).
        let property_key = PropertyKey::from_number(k as u64);

        // b. Let value be ? Get(src, Pk).
        let value = source_object.get(vm, &property_key)?;

        // c. Let targetIndex be 𝔽(targetOffset + k).
        // NOTE: We verify above that target_offset + source_length is valid, so this cannot fail.
        let target_index = canonical_index_from_double(vm, CanonicalIndexType::Index, target_offset + k as f64).must();

        // d. Perform ? TypedArraySetElement(target, targetIndex, value).
        typed_array_set_element(vm, target, target_index, value)?;

        // e. Set k to k + 1.
        k += 1;
    }

    // 10. Return unused.
    Ok(())
}

impl TypedArrayPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<TypedArrayPrototype> {
        realm.create_object(
            vm,
            TypedArrayPrototype {
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
        let configurable = PropertyAttributes::new(Attribute::CONFIGURABLE);

        let define_accessor = |property_key: &PropertyKey, getter| {
            object.define_native_accessor(vm, realm, property_key, getter, None, configurable);
        };
        define_accessor(&names.buffer, raw_native!(TypedArrayPrototype::buffer_getter));
        define_accessor(&names.byteLength, raw_native!(TypedArrayPrototype::byte_length_getter));
        define_accessor(&names.byteOffset, raw_native!(TypedArrayPrototype::byte_offset_getter));
        define_accessor(&names.length, raw_native!(TypedArrayPrototype::length_getter));

        let define_function = |property_key: &PropertyKey, function, length| {
            object.define_native_function(vm, realm, property_key, function, length, attr, None);
        };
        define_function(&names.at, raw_native!(TypedArrayPrototype::at), 1);
        define_function(&names.copyWithin, raw_native!(TypedArrayPrototype::copy_within), 2);
        define_function(&names.entries, raw_native!(TypedArrayPrototype::entries), 0);
        define_function(&names.every, raw_native!(TypedArrayPrototype::every), 1);
        define_function(&names.fill, raw_native!(TypedArrayPrototype::fill), 1);
        define_function(&names.filter, raw_native!(TypedArrayPrototype::filter), 1);
        define_function(&names.find, raw_native!(TypedArrayPrototype::find), 1);
        define_function(&names.findIndex, raw_native!(TypedArrayPrototype::find_index), 1);
        define_function(&names.findLast, raw_native!(TypedArrayPrototype::find_last), 1);
        define_function(
            &names.findLastIndex,
            raw_native!(TypedArrayPrototype::find_last_index),
            1,
        );
        define_function(&names.forEach, raw_native!(TypedArrayPrototype::for_each), 1);
        define_function(&names.includes, raw_native!(TypedArrayPrototype::includes), 1);
        define_function(&names.indexOf, raw_native!(TypedArrayPrototype::index_of), 1);
        define_function(&names.join, raw_native!(TypedArrayPrototype::join), 1);
        define_function(&names.keys, raw_native!(TypedArrayPrototype::keys), 0);
        define_function(&names.lastIndexOf, raw_native!(TypedArrayPrototype::last_index_of), 1);
        define_function(&names.map, raw_native!(TypedArrayPrototype::map), 1);
        define_function(&names.reduce, raw_native!(TypedArrayPrototype::reduce), 1);
        define_function(&names.reduceRight, raw_native!(TypedArrayPrototype::reduce_right), 1);
        define_function(&names.reverse, raw_native!(TypedArrayPrototype::reverse), 0);
        define_function(&names.set, raw_native!(TypedArrayPrototype::set), 1);
        define_function(&names.slice, raw_native!(TypedArrayPrototype::slice), 2);
        define_function(&names.some, raw_native!(TypedArrayPrototype::some), 1);
        define_function(&names.sort, raw_native!(TypedArrayPrototype::sort), 1);
        define_function(&names.subarray, raw_native!(TypedArrayPrototype::subarray), 2);
        define_function(
            &names.toLocaleString,
            raw_native!(TypedArrayPrototype::to_locale_string),
            0,
        );
        define_function(&names.toReversed, raw_native!(TypedArrayPrototype::to_reversed), 0);
        define_function(&names.toSorted, raw_native!(TypedArrayPrototype::to_sorted), 1);
        define_function(&names.with, raw_native!(TypedArrayPrototype::with), 2);
        define_function(&names.values, raw_native!(TypedArrayPrototype::values), 0);

        define_accessor(
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            raw_native!(TypedArrayPrototype::to_string_tag_getter),
        );

        // 23.2.3.34 %TypedArray%.prototype.toString ( ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.tostring
        object.define_direct_property(
            vm,
            &names.toString,
            realm
                .intrinsics()
                .array_prototype(vm)
                .get_without_side_effects(vm, &names.toString),
            attr,
        );

        // 23.2.3.37 %TypedArray%.prototype [ @@iterator ] ( ), https://tc39.es/ecma262/#sec-%typedarray%.prototype-@@iterator
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().iterator),
            object.get_without_side_effects(vm, &names.values),
            attr,
        );
    }

    // 23.2.3.1 %TypedArray%.prototype.at ( index ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.at
    fn at(vm: &Vm) -> ThrowCompletionOr<Value> {
        let index = vm.argument(0);

        // 1. Let O be the this value.
        let typed_array = typed_array_from_this(vm)?;

        // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
        let typed_array_record = validate_typed_array(vm, &typed_array, Order::SeqCst)?;

        // 3. Let len be TypedArrayLength(taRecord).
        let length = f64::from(typed_array_length(&typed_array_record));

        // 4. Let relativeIndex be ? ToIntegerOrInfinity(index).
        let relative_index = index.to_integer_or_infinity(vm)?;

        if relative_index.is_infinite() {
            return Ok(Value::UNDEFINED);
        }

        // 5. If relativeIndex ≥ 0, then
        let k = if relative_index >= 0.0 {
            // a. Let k be relativeIndex.
            relative_index
        }
        // 6. Else,
        else {
            // a. Let k be len + relativeIndex.
            length + relative_index
        };

        // 7. If k < 0 or k ≥ len, return undefined.
        if k < 0.0 || k >= length {
            return Ok(Value::UNDEFINED);
        }

        // 8. Return ! Get(O, ! ToString(𝔽(k))).
        Ok(typed_array.get(vm, &PropertyKey::from_number(k as u64)).must())
    }

    // 23.2.3.2 get %TypedArray%.prototype.buffer, https://tc39.es/ecma262/#sec-get-%typedarray%.prototype.buffer
    fn buffer_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be the this value.
        // 2. Perform ? RequireInternalSlot(O, [[TypedArrayName]]).
        // 3. Assert: O has a [[ViewedArrayBuffer]] internal slot.
        let typed_array = typed_array_from_this(vm)?;

        // 4. Let buffer be O.[[ViewedArrayBuffer]].
        let buffer = typed_array.viewed_array_buffer();

        // 5. Return buffer.
        Ok(Value::from_object(buffer))
    }

    // 23.2.3.3 get %TypedArray%.prototype.byteLength, https://tc39.es/ecma262/#sec-get-%typedarray%.prototype.bytelength
    fn byte_length_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be the this value.
        // 2. Perform ? RequireInternalSlot(O, [[TypedArrayName]]).
        // 3. Assert: O has a [[ViewedArrayBuffer]] internal slot.
        let typed_array = typed_array_from_this(vm)?;

        // 4. Let taRecord be MakeTypedArrayWithBufferWitnessRecord(O, seq-cst).
        let typed_array_record = make_typed_array_with_buffer_witness_record(&typed_array, Order::SeqCst);

        // 5. Let size be TypedArrayByteLength(taRecord).
        let size = typed_array_byte_length(&typed_array_record);

        // 6. Return 𝔽(size).
        Ok(index_value(size))
    }

    // 23.2.3.4 get %TypedArray%.prototype.byteOffset, https://tc39.es/ecma262/#sec-get-%typedarray%.prototype.byteoffset
    fn byte_offset_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be the this value.
        // 2. Perform ? RequireInternalSlot(O, [[TypedArrayName]]).
        // 3. Assert: O has a [[ViewedArrayBuffer]] internal slot.
        let typed_array = typed_array_from_this(vm)?;

        // 4. Let taRecord be MakeTypedArrayWithBufferWitnessRecord(O, seq-cst).
        let typed_array_record = make_typed_array_with_buffer_witness_record(&typed_array, Order::SeqCst);

        // 5. If IsTypedArrayOutOfBounds(taRecord) is true, return +0𝔽.
        if is_typed_array_out_of_bounds(&typed_array_record) {
            return Ok(Value::from_i32(0));
        }

        // 6. Let offset be O.[[ByteOffset]].
        let offset = typed_array.byte_offset();

        // 7. Return 𝔽(offset).
        Ok(index_value(offset))
    }

    // 23.2.3.6 %TypedArray%.prototype.copyWithin ( target, start [ , end ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.copywithin
    fn copy_within(vm: &Vm) -> ThrowCompletionOr<Value> {
        let target = vm.argument(0);
        let start = vm.argument(1);
        let end = vm.argument(2);

        // 1. Let O be the this value.
        let typed_array = typed_array_from_this(vm)?;

        // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
        let typed_array_record = validate_typed_array(vm, &typed_array, Order::SeqCst)?;

        // 3. Let len be TypedArrayLength(taRecord).
        let mut length = f64::from(typed_array_length(&typed_array_record));

        // 4. Let relativeTarget be ? ToIntegerOrInfinity(target).
        let relative_target = target.to_integer_or_infinity(vm)?;

        // 5. If relativeTarget = -∞, let targetIndex be 0.
        let target_index = if relative_target == f64::NEG_INFINITY {
            0.0
        }
        // 6. Else if relativeTarget < 0, let targetIndex be max(len + relativeTarget, 0).
        else if relative_target < 0.0 {
            (length + relative_target).max(0.0)
        }
        // 7. Else, let targetIndex be min(relativeTarget, len).
        else {
            relative_target.min(length)
        };

        // 8. Let relativeStart be ? ToIntegerOrInfinity(start).
        let relative_start = start.to_integer_or_infinity(vm)?;

        // 9. If relativeStart = -∞, let startIndex be 0.
        let start_index = if relative_start == f64::NEG_INFINITY {
            0.0
        }
        // 10. Else if relativeStart < 0, let startIndex be max(len + relativeStart, 0).
        else if relative_start < 0.0 {
            (length + relative_start).max(0.0)
        }
        // 11. Else, let startIndex be min(relativeStart, len).
        else {
            relative_start.min(length)
        };

        // 12. If end is undefined, let relativeEnd be len; else let relativeEnd be ? ToIntegerOrInfinity(end).
        let relative_end = if end.is_undefined() {
            length
        } else {
            end.to_integer_or_infinity(vm)?
        };

        // 13. If relativeEnd = -∞, let endIndex be 0.
        let end_index = if relative_end == f64::NEG_INFINITY {
            0.0
        }
        // 14. Else if relativeEnd < 0, let endIndex be max(len + relativeEnd, 0).
        else if relative_end < 0.0 {
            (length + relative_end).max(0.0)
        }
        // 15. Else, let endIndex be min(relativeEnd, len).
        else {
            relative_end.min(length)
        };

        // 16. Let count be min(endIndex - startIndex, len - targetIndex).
        let mut count = (end_index - start_index).min(length - target_index);

        // 17. If count > 0, then
        if count > 0.0 {
            // a. NOTE: The copying must be performed in a manner that preserves the bit-level encoding of the source data.

            // b. Let buffer be O.[[ViewedArrayBuffer]].
            let buffer = typed_array.viewed_array_buffer();

            // c. Set taRecord to MakeTypedArrayWithBufferWitnessRecord(O, SEQ-CST).
            let typed_array_record = make_typed_array_with_buffer_witness_record(&typed_array, Order::SeqCst);

            // d. If IsTypedArrayOutOfBounds(taRecord) is true, throw a TypeError exception.
            if is_typed_array_out_of_bounds(&typed_array_record) {
                return vm.throw_completion(ErrorKind::TypeError, ErrorType::BufferOutOfBounds, &[&"TypedArray"]);
            }

            // e. Set len to TypedArrayLength(taRecord).
            length = f64::from(typed_array_length(&typed_array_record));

            // f. NOTE: Side-effects of the above steps may have reduced the size of O, in which case copying should proceed
            //    with the longest still-applicable prefix.

            // g. Set count to min(count, len - startIndex, len - targetIndex).
            count = count.min((length - start_index).min(length - target_index));

            // NB: If a coercion side effect shrank the buffer, count may be non-positive. Return before attempting
            //     conversion to an unsigned type.
            if count <= 0.0 {
                return Ok(Value::from_object(typed_array));
            }

            // h. Let elementSize be TypedArrayElementSize(O).
            let element_size = typed_array.element_size() as usize;

            // i. Let byteOffset be O.[[ByteOffset]].
            let byte_offset = typed_array.byte_offset() as usize;

            // FIXME: Not exactly sure what we should do when overflow occurs. Just return as if succeeded for now.

            // j. Let toByteIndex be (targetIndex × elementSize) + byteOffset.
            let Some(mut to_byte_index) = (target_index as usize)
                .checked_mul(element_size)
                .and_then(|index| index.checked_add(byte_offset))
            else {
                eprintln!("TypedArrayPrototype::copy_within: to_byte_index overflowed, returning as if succeeded.");
                return Ok(Value::from_object(typed_array));
            };

            // k. Let fromByteIndex be (startIndex × elementSize) + byteOffset.
            let Some(mut from_byte_index) = (start_index as usize)
                .checked_mul(element_size)
                .and_then(|index| index.checked_add(byte_offset))
            else {
                eprintln!("TypedArrayPrototype::copy_within: from_byte_index overflowed, returning as if succeeded.");
                return Ok(Value::from_object(typed_array));
            };

            // l. Let countBytes be count × elementSize.
            let Some(mut count_bytes) = (count as usize).checked_mul(element_size) else {
                eprintln!("TypedArrayPrototype::copy_within: count_bytes overflowed, returning as if succeeded.");
                return Ok(Value::from_object(typed_array));
            };

            let Some(from_plus_count) = from_byte_index.checked_add(count_bytes) else {
                eprintln!("TypedArrayPrototype::copy_within: from_plus_count overflowed, returning as if succeeded.");
                return Ok(Value::from_object(typed_array));
            };

            if !buffer.is_shared_array_buffer() {
                buffer.move_data(to_byte_index, from_byte_index, count_bytes);
                return Ok(Value::from_object(typed_array));
            }

            let direction: isize;

            // m. If fromByteIndex < toByteIndex and toByteIndex < fromByteIndex + countBytes, then
            if from_byte_index < to_byte_index && to_byte_index < from_plus_count {
                // i. Let direction be -1.
                direction = -1;

                // ii. Set fromByteIndex to fromByteIndex + countBytes - 1.
                from_byte_index = from_plus_count - 1;

                let Some(to_plus_count) = to_byte_index.checked_add(count_bytes) else {
                    eprintln!("TypedArrayPrototype::copy_within: to_plus_count overflowed, returning as if succeeded.");
                    return Ok(Value::from_object(typed_array));
                };

                // iii. Set toByteIndex to toByteIndex + countBytes - 1.
                to_byte_index = to_plus_count - 1;
            }
            // n. Else,
            else {
                // i. Let direction be 1.
                direction = 1;
            }

            // o. Repeat, while countBytes > 0,
            while count_bytes > 0 {
                // i. Let value be GetValueFromBuffer(buffer, fromByteIndex, UINT8, true, UNORDERED).
                let value = buffer.get_value(vm, from_byte_index, ElementType::Uint8, true, Order::Unordered, true);

                // ii. Perform SetValueInBuffer(buffer, toByteIndex, UINT8, value, true, UNORDERED).
                buffer.set_value(
                    vm,
                    to_byte_index,
                    ElementType::Uint8,
                    value,
                    true,
                    Order::Unordered,
                    true,
                );

                // iii. Set fromByteIndex to fromByteIndex + direction.
                from_byte_index = from_byte_index.wrapping_add_signed(direction);

                // iv. Set toByteIndex to toByteIndex + direction.
                to_byte_index = to_byte_index.wrapping_add_signed(direction);

                // v. Set countBytes to countBytes - 1.
                count_bytes -= 1;
            }
        }

        // 18. Return O.
        Ok(Value::from_object(typed_array))
    }

    // 23.2.3.7 %TypedArray%.prototype.entries ( ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.entries
    fn entries(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        // 1. Let O be the this value.
        let typed_array = typed_array_from_this(vm)?;

        // 2. Perform ? ValidateTypedArray(O, seq-cst).
        validate_typed_array(vm, &typed_array, Order::SeqCst)?;

        // 3. Return CreateArrayIterator(O, key+value).
        Ok(Value::from_object(ArrayIterator::create(
            vm,
            realm,
            Value::from_object(typed_array),
            PropertyKind::KeyAndValue,
        )))
    }

    // 23.2.3.8 %TypedArray%.prototype.every ( callbackfn [ , thisArg ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.every
    fn every(vm: &Vm) -> ThrowCompletionOr<Value> {
        let this_arg = vm.argument(1);

        // 1. Let O be the this value.
        let typed_array = typed_array_from_this(vm)?;

        // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
        let typed_array_record = validate_typed_array(vm, &typed_array, Order::SeqCst)?;

        // 3. Let len be TypedArrayLength(taRecord).
        let length = typed_array_length(&typed_array_record) as usize;

        // 4. If IsCallable(callbackfn) is false, throw a TypeError exception.
        let callback_function = callback_from_args(vm, "every")?;

        // 5. Let k be 0.
        // 6. Repeat, while k < len,
        for k in 0..length {
            // a. Let Pk be ! ToString(𝔽(k)).
            let property_key = PropertyKey::from_number(k as u64);

            // b. Let kValue be ! Get(O, Pk).
            let value = typed_array.get(vm, &property_key).must();

            // c. Let testResult be ToBoolean(? Call(callbackfn, thisArg, « kValue, 𝔽(k), O »)).
            let test_result = call_function_object(
                vm,
                callback_function,
                this_arg,
                &[value, size_value(k), Value::from_object(typed_array)],
            )?
            .to_boolean();

            // d. If testResult is false, return false.
            if !test_result {
                return Ok(Value::FALSE);
            }

            // e. Set k to k + 1.
        }

        // 7. Return true.
        Ok(Value::TRUE)
    }

    // 23.2.3.9 %TypedArray%.prototype.fill ( value [ , start [ , end ] ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.fill
    fn fill(vm: &Vm) -> ThrowCompletionOr<Value> {
        let mut value = vm.argument(0);
        let start = vm.argument(1);
        let end = vm.argument(2);

        // 1. Let O be the this value.
        let typed_array = typed_array_from_this(vm)?;

        // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
        let typed_array_record = validate_typed_array(vm, &typed_array, Order::SeqCst)?;

        // 3. Let len be TypedArrayLength(taRecord).
        let mut length = typed_array_length(&typed_array_record);

        // 4. If O.[[ContentType]] is BigInt, set value to ? ToBigInt(value).
        if typed_array.content_type() == ContentType::BigInt {
            value = Value::from_bigint(value.to_bigint(vm)?);
        }
        // 5. Otherwise, set value to ? ToNumber(value).
        else {
            value = value.to_number(vm)?;
        }

        // 6. Let relativeStart be ? ToIntegerOrInfinity(start).
        let relative_start = start.to_integer_or_infinity(vm)?;

        // 7. If relativeStart = -∞, let k be 0.
        let mut k: u32 = if relative_start == f64::NEG_INFINITY {
            0
        }
        // 8. Else if relativeStart < 0, let k be max(len + relativeStart, 0).
        else if relative_start < 0.0 {
            (f64::from(length) + relative_start).max(0.0) as u32
        }
        // 9. Else, let k be min(relativeStart, len).
        else {
            relative_start.min(f64::from(length)) as u32
        };

        // 10. If end is undefined, let relativeEnd be len; else let relativeEnd be ? ToIntegerOrInfinity(end).
        let relative_end = if end.is_undefined() {
            f64::from(length)
        } else {
            end.to_integer_or_infinity(vm)?
        };

        // 11. If relativeEnd = -∞, let final be 0.
        let mut final_: u32 = if relative_end == f64::NEG_INFINITY {
            0
        }
        // 12. Else if relativeEnd < 0, let final be max(len + relativeEnd, 0).
        else if relative_end < 0.0 {
            (f64::from(length) + relative_end).max(0.0) as u32
        }
        // 13. Else, let final be min(relativeEnd, len).
        else {
            relative_end.min(f64::from(length)) as u32
        };

        // 14. Set taRecord to MakeTypedArrayWithBufferWitnessRecord(O, seq-cst).
        let typed_array_record = make_typed_array_with_buffer_witness_record(&typed_array, Order::SeqCst);

        // 15. If IsTypedArrayOutOfBounds(taRecord) is true, throw a TypeError exception.
        if is_typed_array_out_of_bounds(&typed_array_record) {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::BufferOutOfBounds, &[&"TypedArray"]);
        }

        // 16. Set len to TypedArrayLength(taRecord).
        length = typed_array_length(&typed_array_record);

        // 17. Set final to min(final, len).
        final_ = final_.min(length);

        // The int32 fast path uses a bulk memcpy — so it's only safe for a non-shared buffer; a shared buffer falls through
        // to the per-element SetValueInBuffer loop below (a tear-free relaxed-atomic write per element).
        if value.is_int32() && !typed_array.viewed_array_buffer().is_shared_array_buffer() {
            let int_value = value.as_i32();
            let fast_fill = |element: &[u8]| fast_typed_array_fill(&typed_array, k, final_, element);
            match typed_array.kind() {
                Kind::Uint8Array => {
                    fast_fill(&(int_value as u8).to_le_bytes());
                    return Ok(Value::from_object(typed_array));
                }
                Kind::Uint16Array => {
                    fast_fill(&(int_value as u16).to_le_bytes());
                    return Ok(Value::from_object(typed_array));
                }
                Kind::Uint32Array => {
                    fast_fill(&(int_value as u32).to_le_bytes());
                    return Ok(Value::from_object(typed_array));
                }
                Kind::Int8Array => {
                    fast_fill(&(int_value as i8).to_le_bytes());
                    return Ok(Value::from_object(typed_array));
                }
                Kind::Int16Array => {
                    fast_fill(&(int_value as i16).to_le_bytes());
                    return Ok(Value::from_object(typed_array));
                }
                Kind::Int32Array => {
                    fast_fill(&int_value.to_le_bytes());
                    return Ok(Value::from_object(typed_array));
                }
                Kind::Uint8ClampedArray => {
                    fast_fill(&(int_value.clamp(0, 255) as u8).to_le_bytes());
                    return Ok(Value::from_object(typed_array));
                }
                // FIXME: Support more TypedArray kinds.
                _ => {}
            }
        }

        // 18. Repeat, while k < final,
        while k < final_ {
            // a. Let Pk be ! ToString(𝔽(k)).
            // b. Perform ! Set(O, Pk, value, true).
            let canonical_index = CanonicalIndex::new(CanonicalIndexType::Index, k);
            let _ = typed_array_set_element(vm, &typed_array, canonical_index, value);

            // c. Set k to k + 1.
            k += 1;
        }

        // 19. Return O.
        Ok(Value::from_object(typed_array))
    }

    // 23.2.3.10 %TypedArray%.prototype.filter ( callbackfn [ , thisArg ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.filter
    fn filter(vm: &Vm) -> ThrowCompletionOr<Value> {
        let this_arg = vm.argument(1);

        // 1. Let O be the this value.
        let typed_array = typed_array_from_this(vm)?;

        // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
        let typed_array_record = validate_typed_array(vm, &typed_array, Order::SeqCst)?;

        // 3. Let len be TypedArrayLength(taRecord).
        let length = typed_array_length(&typed_array_record) as usize;

        // 4. If IsCallable(callbackfn) is false, throw a TypeError exception.
        let callback_function = callback_from_args(vm, "filter")?;

        // 5. Let kept be a new empty List.
        let kept = MarkedVec::new(vm);

        // 6. Let captured be 0.
        let mut captured = 0usize;

        // 7. Let k be 0.
        // 8. Repeat, while k < len,
        for k in 0..length {
            // a. Let Pk be ! ToString(𝔽(k)).
            let property_key = PropertyKey::from_number(k as u64);

            // b. Let kValue be ! Get(O, Pk).
            let value = typed_array.get(vm, &property_key).must();

            // c. Let selected be ToBoolean(? Call(callbackfn, thisArg, « kValue, 𝔽(k), O »)).
            let selected = call_function_object(
                vm,
                callback_function,
                this_arg,
                &[value, size_value(k), Value::from_object(typed_array)],
            )?
            .to_boolean();

            // d. If selected is true, then
            if selected {
                // i. Append kValue to kept.
                kept.push(value);

                // ii. Set captured to captured + 1.
                captured += 1;
            }

            // e. Set k to k + 1.
        }

        // 9. Let A be ? TypedArraySpeciesCreate(O, « 𝔽(captured) »).
        let realm = vm.current_realm().expect("a builtin runs in a realm");
        let filter_array = typed_array_species_create(
            vm,
            &typed_array,
            || typed_array.create_default(vm, realm, captured as u32),
            &[size_value(captured)],
        )?;

        // 10. Let n be 0.
        // 11. For each element e of kept, do
        for index in 0..kept.len() {
            // a. Perform ! Set(A, ! ToString(𝔽(n)), e, true).
            let value = kept.get(index).expect("the index is in bounds");
            filter_array.set(
                vm,
                &PropertyKey::from_number(index as u64),
                value,
                ShouldThrowExceptions::Yes,
            )?;

            // b. Set n to n + 1.
        }

        // 12. Return A.
        Ok(Value::from_object(filter_array))
    }

    // 23.2.3.11 %TypedArray%.prototype.find ( predicate [ , thisArg ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.find
    fn find(vm: &Vm) -> ThrowCompletionOr<Value> {
        let this_arg = vm.argument(1);

        // 1. Let O be the this value.
        let typed_array = typed_array_from_this(vm)?;

        // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
        let typed_array_record = validate_typed_array(vm, &typed_array, Order::SeqCst)?;

        // 3. Let len be TypedArrayLength(taRecord).
        let length = typed_array_length(&typed_array_record);

        // 4. Let findRec be ? FindViaPredicate(O, len, ascending, predicate, thisArg).
        let find_record = find_via_predicate(vm, typed_array, length, Direction::Ascending, this_arg, "find")?;

        // 5. Return findRec.[[Value]].
        Ok(find_record.value)
    }

    // 23.2.3.12 %TypedArray%.prototype.findIndex ( predicate [ , thisArg ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.findindex
    fn find_index(vm: &Vm) -> ThrowCompletionOr<Value> {
        let this_arg = vm.argument(1);

        // 1. Let O be the this value.
        let typed_array = typed_array_from_this(vm)?;

        // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
        let typed_array_record = validate_typed_array(vm, &typed_array, Order::SeqCst)?;

        // 3. Let len be TypedArrayLength(taRecord).
        let length = typed_array_length(&typed_array_record);

        // 4. Let findRec be ? FindViaPredicate(O, len, ascending, predicate, thisArg).
        let find_record = find_via_predicate(vm, typed_array, length, Direction::Ascending, this_arg, "findIndex")?;

        // 5. Return findRec.[[Index]].
        Ok(find_record.index_to_value())
    }

    // 23.2.3.13 %TypedArray%.prototype.findLast ( predicate [ , thisArg ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.findlast
    fn find_last(vm: &Vm) -> ThrowCompletionOr<Value> {
        let this_arg = vm.argument(1);

        // 1. Let O be the this value.
        let typed_array = typed_array_from_this(vm)?;

        // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
        let typed_array_record = validate_typed_array(vm, &typed_array, Order::SeqCst)?;

        // 3. Let len be TypedArrayLength(taRecord).
        let length = typed_array_length(&typed_array_record);

        // 4. Let findRec be ? FindViaPredicate(O, len, descending, predicate, thisArg).
        let find_record = find_via_predicate(vm, typed_array, length, Direction::Descending, this_arg, "findLast")?;

        // 5. Return findRec.[[Value]].
        Ok(find_record.value)
    }

    // 23.2.3.14 %TypedArray%.prototype.findLastIndex ( predicate [ , thisArg ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.findlastindex
    fn find_last_index(vm: &Vm) -> ThrowCompletionOr<Value> {
        let this_arg = vm.argument(1);

        // 1. Let O be the this value.
        let typed_array = typed_array_from_this(vm)?;

        // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
        let typed_array_record = validate_typed_array(vm, &typed_array, Order::SeqCst)?;

        // 3. Let len be TypedArrayLength(taRecord).
        let length = typed_array_length(&typed_array_record);

        // 4. Let findRec be ? FindViaPredicate(O, len, descending, predicate, thisArg).
        let find_record = find_via_predicate(
            vm,
            typed_array,
            length,
            Direction::Descending,
            this_arg,
            "findLastIndex",
        )?;

        // 5. Return findRec.[[Index]].
        Ok(find_record.index_to_value())
    }

    // 23.2.3.15 %TypedArray%.prototype.forEach ( callbackfn [ , thisArg ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.foreach
    fn for_each(vm: &Vm) -> ThrowCompletionOr<Value> {
        let this_arg = vm.argument(1);

        // 1. Let O be the this value.
        let typed_array = typed_array_from_this(vm)?;

        // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
        let typed_array_record = validate_typed_array(vm, &typed_array, Order::SeqCst)?;

        // 3. Let len be TypedArrayLength(taRecord).
        let length = typed_array_length(&typed_array_record) as usize;

        // 4. If IsCallable(callbackfn) is false, throw a TypeError exception.
        let callback_function = callback_from_args(vm, "forEach")?;

        // 5. Let k be 0.
        // 6. Repeat, while k < len,
        for k in 0..length {
            // a. Let Pk be ! ToString(𝔽(k)).
            let property_key = PropertyKey::from_number(k as u64);

            // b. Let kValue be ! Get(O, Pk).
            let value = typed_array.get(vm, &property_key).must();

            // c. Perform ? Call(callbackfn, thisArg, « kValue, 𝔽(k), O »).
            call_function_object(
                vm,
                callback_function,
                this_arg,
                &[value, size_value(k), Value::from_object(typed_array)],
            )?;

            // d. Set k to k + 1.
        }

        // 7. Return undefined.
        Ok(Value::UNDEFINED)
    }

    // 23.2.3.16 %TypedArray%.prototype.includes ( searchElement [ , fromIndex ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.includes
    fn includes(vm: &Vm) -> ThrowCompletionOr<Value> {
        let search_element = vm.argument(0);
        let from_index = vm.argument(1);

        // 1. Let O be the this value.
        let typed_array = typed_array_from_this(vm)?;

        // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
        let typed_array_record = validate_typed_array(vm, &typed_array, Order::SeqCst)?;

        // 3. Let len be TypedArrayLength(taRecord).
        let length = typed_array_length(&typed_array_record);

        // 4. If len = 0, return false.
        if length == 0 {
            return Ok(Value::FALSE);
        }

        // 5. Let n be ? ToIntegerOrInfinity(fromIndex).
        let mut n = from_index.to_integer_or_infinity(vm)?;

        // 6. Assert: If fromIndex is undefined, then n is 0.
        if from_index.is_undefined() {
            assert!(n == 0.0);
        }

        // 7. If n = +∞, return false.
        if n == f64::INFINITY {
            return Ok(Value::FALSE);
        }
        // 8. Else if n = -∞, set n to 0.
        else if n == f64::NEG_INFINITY {
            n = 0.0;
        }

        let mut k: u32;
        // 9. If n ≥ 0, then
        if n >= 0.0 {
            // AD-HOC: A fromIndex at or beyond len matches nothing. Return before converting it to an unsigned type.
            if n >= f64::from(length) {
                return Ok(Value::FALSE);
            }

            // a. Let k be n.
            k = n as u32;
        }
        // 10. Else,
        else {
            // a. Let k be len + n.
            let mut relative_k = f64::from(length) + n; // Ensures we dont overflow `k`.

            // b. If k < 0, set k to 0.
            if relative_k < 0.0 {
                relative_k = 0.0;
            }

            k = relative_k as u32;
        }

        // 11. Repeat, while k < len,
        while k < length {
            // a. Let elementK be ! Get(O, ! ToString(𝔽(k))).
            let element_k = typed_array.get(vm, &PropertyKey::from(k)).must();

            // b. If SameValueZero(searchElement, elementK) is true, return true.
            if same_value_zero(search_element, element_k) {
                return Ok(Value::TRUE);
            }

            // c. Set k to k + 1.
            k += 1;
        }

        // 12. Return false.
        Ok(Value::FALSE)
    }

    // 23.2.3.17 %TypedArray%.prototype.indexOf ( searchElement [ , fromIndex ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.indexof
    fn index_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        let search_element = vm.argument(0);
        let from_index = vm.argument(1);

        // 1. Let O be the this value.
        let typed_array = typed_array_from_this(vm)?;

        // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
        let typed_array_record = validate_typed_array(vm, &typed_array, Order::SeqCst)?;

        // 3. Let len be TypedArrayLength(taRecord).
        let length = typed_array_length(&typed_array_record);

        // 4. If len = 0, return -1𝔽.
        if length == 0 {
            return Ok(Value::from_i32(-1));
        }

        // 5. Let n be ? ToIntegerOrInfinity(fromIndex).
        let mut n = from_index.to_integer_or_infinity(vm)?;

        // 6. Assert: If fromIndex is undefined, then n is 0.
        if from_index.is_undefined() {
            assert!(n == 0.0);
        }

        // 7. If n = +∞, return -1𝔽.
        if n == f64::INFINITY {
            return Ok(Value::from_i32(-1));
        }
        // 8. Else if n = -∞, set n to 0.
        else if n == f64::NEG_INFINITY {
            n = 0.0;
        }

        let mut k: u32;
        // 9. If n ≥ 0, then
        if n >= 0.0 {
            // AD-HOC: A fromIndex at or beyond len matches nothing. Return before converting it to an unsigned type.
            if n >= f64::from(length) {
                return Ok(Value::from_i32(-1));
            }

            // a. Let k be n.
            k = n as u32;
        }
        // 10. Else,
        else {
            // a. Let k be len + n.
            let mut relative_k = f64::from(length) + n;

            // b. If k < 0, set k to 0.
            if relative_k < 0.0 {
                relative_k = 0.0;
            }

            k = relative_k as u32;
        }

        // 11. Repeat, while k < len,
        while k < length {
            // a. Let kPresent be ! HasProperty(O, ! ToString(𝔽(k))).
            let k_present = typed_array.has_property(vm, &PropertyKey::from(k)).must();

            // b. If kPresent is true, then
            if k_present {
                // i. Let elementK be ! Get(O, ! ToString(𝔽(k))).
                let element_k = typed_array.get(vm, &PropertyKey::from(k)).must();

                // ii. If IsStrictlyEqual(searchElement, elementK) is true, return 𝔽(k).
                if is_strictly_equal(search_element, element_k) {
                    return Ok(index_value(k));
                }
            }

            // c. Set k to k + 1.
            k += 1;
        }

        // 12. Return -1𝔽.
        Ok(Value::from_i32(-1))
    }

    // 23.2.3.18 %TypedArray%.prototype.join ( separator ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.join
    fn join(vm: &Vm) -> ThrowCompletionOr<Value> {
        let separator = vm.argument(0);

        // 1. Let O be the this value.
        let typed_array = typed_array_from_this(vm)?;

        // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
        let typed_array_record = validate_typed_array(vm, &typed_array, Order::SeqCst)?;

        // 3. Let len be TypedArrayLength(taRecord).
        let length = typed_array_length(&typed_array_record) as usize;

        // 4. If separator is undefined, let sep be ",".
        let sep = if separator.is_undefined() {
            Utf16String::from_utf8(",")
        }
        // 5. Else, let sep be ? ToString(separator).
        else {
            separator.to_utf16_string(vm)?
        };

        // 6. Let R be the empty String.
        let mut builder = Utf16StringBuilder::new();
        let mut result_length = 0usize;

        // 7. Let k be 0.
        // 8. Repeat, while k < len,
        for k in 0..length {
            // a. If k > 0, set R to the string-concatenation of R and sep.
            if k > 0 {
                result_length = checked_js_string_length_sum(
                    vm,
                    result_length,
                    Utf16View::of_string(&sep).length_in_code_units(),
                    ErrorType::StringSizeMustNotOverflow,
                )?;
                builder.append(Utf16View::of_string(&sep));
            }

            // b. Let element be ! Get(O, ! ToString(𝔽(k))).
            let element = typed_array.get(vm, &PropertyKey::from_number(k as u64)).must();

            // c. If element is undefined, let next be the empty String; otherwise, let next be ! ToString(element).
            let next = if element.is_undefined() {
                Utf16String::default()
            } else {
                element.to_utf16_string(vm).must()
            };

            // d. Set R to the string-concatenation of R and next.
            result_length = checked_js_string_length_sum(
                vm,
                result_length,
                Utf16View::of_string(&next).length_in_code_units(),
                ErrorType::StringSizeMustNotOverflow,
            )?;
            builder.append(Utf16View::of_string(&next));

            // e. Set k to k + 1.
        }

        // 9. Return R.
        Ok(Value::from_string(PrimitiveString::create(
            vm,
            builder.to_utf16_string(),
        )))
    }

    // 23.2.3.19 %TypedArray%.prototype.keys ( ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.keys
    fn keys(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        // 1. Let O be the this value.
        let typed_array = typed_array_from_this(vm)?;

        // 2. Perform ? ValidateTypedArray(O, seq-cst).
        validate_typed_array(vm, &typed_array, Order::SeqCst)?;

        // 3. Return CreateArrayIterator(O, key).
        Ok(Value::from_object(ArrayIterator::create(
            vm,
            realm,
            Value::from_object(typed_array),
            PropertyKind::Key,
        )))
    }

    // 23.2.3.20 %TypedArray%.prototype.lastIndexOf ( searchElement [ , fromIndex ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.lastindexof
    fn last_index_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        let search_element = vm.argument(0);
        let from_index = vm.argument(1);

        // 1. Let O be the this value.
        let typed_array = typed_array_from_this(vm)?;

        // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
        let typed_array_record = validate_typed_array(vm, &typed_array, Order::SeqCst)?;

        // 3. Let len be TypedArrayLength(taRecord).
        let length = typed_array_length(&typed_array_record);

        // 4. If len = 0, return -1𝔽.
        if length == 0 {
            return Ok(Value::from_i32(-1));
        }

        // 5. If fromIndex is present, let n be ? ToIntegerOrInfinity(fromIndex); else let n be len - 1.
        let n = if vm.argument_count() > 1 {
            from_index.to_integer_or_infinity(vm)?
        } else {
            f64::from(length - 1)
        };

        // 6. If n = -∞, return -1𝔽.
        if n == f64::NEG_INFINITY {
            return Ok(Value::from_i32(-1));
        }

        let mut k: i32;
        // 7. If n ≥ 0, then
        if n >= 0.0 {
            // a. Let k be min(n, len - 1).
            k = n.min(f64::from(length as i32 - 1)) as i32;
        }
        // 8. Else,
        else {
            // a. Let k be len + n.
            let mut relative_k = f64::from(length) + n; // Ensures we dont overflow `k`.

            if relative_k < 0.0 {
                relative_k = -1.0;
            }

            k = relative_k as i32;
        }

        // 9. Repeat, while k ≥ 0,
        while k >= 0 {
            let property_key = PropertyKey::from(k as u32);

            // a. Let kPresent be ! HasProperty(O, ! ToString(𝔽(k))).
            let k_present = typed_array.has_property(vm, &property_key).must();

            // b. If kPresent is true, then
            if k_present {
                // i. Let elementK be ! Get(O, ! ToString(𝔽(k))).
                let element_k = typed_array.get(vm, &property_key).must();

                // ii. If IsStrictlyEqual(searchElement, elementK) is true, return 𝔽(k).
                if is_strictly_equal(search_element, element_k) {
                    return Ok(Value::from_i32(k));
                }
            }

            // c. Set k to k - 1.
            k -= 1;
        }

        // 10. Return -1𝔽.
        Ok(Value::from_i32(-1))
    }

    // 23.2.3.21 get %TypedArray%.prototype.length, https://tc39.es/ecma262/#sec-get-%typedarray%.prototype.length
    fn length_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be the this value.
        // 2. Perform ? RequireInternalSlot(O, [[TypedArrayName]]).
        // 3. Assert: O has [[ViewedArrayBuffer]] and [[ArrayLength]] internal slots.
        let typed_array = typed_array_from_this(vm)?;

        // 4. Let taRecord be MakeTypedArrayWithBufferWitnessRecord(O, seq-cst).
        let typed_array_record = make_typed_array_with_buffer_witness_record(&typed_array, Order::SeqCst);

        // 5. If IsTypedArrayOutOfBounds(taRecord) is true, return +0𝔽.
        if is_typed_array_out_of_bounds(&typed_array_record) {
            return Ok(Value::from_i32(0));
        }

        // 6. Let length be TypedArrayLength(taRecord).
        let length = typed_array_length(&typed_array_record);

        // 7. Return 𝔽(length).
        Ok(index_value(length))
    }

    // 23.2.3.22 %TypedArray%.prototype.map ( callbackfn [ , thisArg ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.map
    fn map(vm: &Vm) -> ThrowCompletionOr<Value> {
        let this_arg = vm.argument(1);

        // 1. Let O be the this value.
        let typed_array = typed_array_from_this(vm)?;

        // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
        let typed_array_record = validate_typed_array(vm, &typed_array, Order::SeqCst)?;

        // 3. Let len be TypedArrayLength(taRecord).
        let length = typed_array_length(&typed_array_record);

        // 4. If IsCallable(callbackfn) is false, throw a TypeError exception.
        let callback_function = callback_from_args(vm, "map")?;

        // 5. Let A be ? TypedArraySpeciesCreate(O, « 𝔽(len) »).
        let realm = vm.current_realm().expect("a builtin runs in a realm");
        let array = typed_array_species_create(
            vm,
            &typed_array,
            || typed_array.create_default(vm, realm, length),
            &[index_value(length)],
        )?;

        // 6. Let k be 0.
        // 7. Repeat, while k < len,
        for k in 0..length as usize {
            // a. Let Pk be ! ToString(𝔽(k)).
            let property_key = PropertyKey::from_number(k as u64);

            // b. Let kValue be ! Get(O, Pk).
            let value = typed_array.get(vm, &property_key).must();

            // c. Let mappedValue be ? Call(callbackfn, thisArg, « kValue, 𝔽(k), O »).
            let mapped_value = call_function_object(
                vm,
                callback_function,
                this_arg,
                &[value, size_value(k), Value::from_object(typed_array)],
            )?;

            // d. Perform ? Set(A, Pk, mappedValue, true).
            array.set(vm, &property_key, mapped_value, ShouldThrowExceptions::Yes)?;

            // e. Set k to k + 1.
        }

        // 8. Return A.
        Ok(Value::from_object(array))
    }

    // 23.2.3.23 %TypedArray%.prototype.reduce ( callbackfn [ , initialValue ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.reduce
    fn reduce(vm: &Vm) -> ThrowCompletionOr<Value> {
        let initial_value = vm.argument(1);

        // 1. Let O be the this value.
        let typed_array = typed_array_from_this(vm)?;

        // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
        let typed_array_record = validate_typed_array(vm, &typed_array, Order::SeqCst)?;

        // 3. Let len be TypedArrayLength(taRecord).
        let length = typed_array_length(&typed_array_record);

        // 4. If IsCallable(callbackfn) is false, throw a TypeError exception.
        let callback_function = callback_from_args(vm, "reduce")?;

        // 5. If len = 0 and initialValue is not present, throw a TypeError exception.
        if length == 0 && vm.argument_count() <= 1 {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::ReduceNoInitial, &[]);
        }

        // 6. Let k be 0.
        let mut k: u32 = 0;

        // 7. Let accumulator be undefined.
        let mut accumulator;

        // 8. If initialValue is present, then
        if vm.argument_count() > 1 {
            // a. Set accumulator to initialValue.
            accumulator = initial_value;
        }
        // 9. Else,
        else {
            // a. Let Pk be ! ToString(𝔽(k)).
            let property_key = PropertyKey::from(k);

            // b. Set accumulator to ! Get(O, Pk).
            accumulator = typed_array.get(vm, &property_key).must();

            // c. Set k to k + 1.
            k += 1;
        }

        // 10. Repeat, while k < len,
        while k < length {
            // a. Let Pk be ! ToString(𝔽(k)).
            let property_key = PropertyKey::from(k);

            // b. Let kValue be ! Get(O, Pk).
            let value = typed_array.get(vm, &property_key).must();

            // c. Set accumulator to ? Call(callbackfn, undefined, « accumulator, kValue, 𝔽(k), O »).
            accumulator = call_function_object(
                vm,
                callback_function,
                Value::UNDEFINED,
                &[accumulator, value, index_value(k), Value::from_object(typed_array)],
            )?;

            // d. Set k to k + 1.
            k += 1;
        }

        // 11. Return accumulator.
        Ok(accumulator)
    }

    // 23.2.3.24 %TypedArray%.prototype.reduceRight ( callbackfn [ , initialValue ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.reduceright
    fn reduce_right(vm: &Vm) -> ThrowCompletionOr<Value> {
        let initial_value = vm.argument(1);

        // 1. Let O be the this value.
        let typed_array = typed_array_from_this(vm)?;

        // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
        let typed_array_record = validate_typed_array(vm, &typed_array, Order::SeqCst)?;

        // 3. Let len be TypedArrayLength(taRecord).
        let length = typed_array_length(&typed_array_record);

        // 4. If IsCallable(callbackfn) is false, throw a TypeError exception.
        let callback_function = callback_from_args(vm, "reduceRight")?;

        // 5. If len = 0 and initialValue is not present, throw a TypeError exception.
        if length == 0 && vm.argument_count() <= 1 {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::ReduceNoInitial, &[]);
        }

        // 6. Let k be len - 1.
        let mut k = length as i32 - 1;

        // 7. Let accumulator be undefined.
        let mut accumulator;

        // 8. If initialValue is present, then
        if vm.argument_count() > 1 {
            // a. Set accumulator to initialValue.
            accumulator = initial_value;
        }
        // 9. Else,
        else {
            // a. Let Pk be ! ToString(𝔽(k)).
            let property_key = PropertyKey::from(k as u32);

            // b. Set accumulator to ! Get(O, Pk).
            accumulator = typed_array.get(vm, &property_key).must();

            // c. Set k to k - 1.
            k -= 1;
        }

        // 10. Repeat, while k ≥ 0,
        while k >= 0 {
            // a. Let Pk be ! ToString(𝔽(k)).
            let property_key = PropertyKey::from(k as u32);

            // b. Let kValue be ! Get(O, Pk).
            let value = typed_array.get(vm, &property_key).must();

            // c. Set accumulator to ? Call(callbackfn, undefined, « accumulator, kValue, 𝔽(k), O »).
            accumulator = call_function_object(
                vm,
                callback_function,
                Value::UNDEFINED,
                &[accumulator, value, Value::from_i32(k), Value::from_object(typed_array)],
            )?;

            // d. Set k to k - 1.
            k -= 1;
        }

        // 11. Return accumulator.
        Ok(accumulator)
    }

    // 23.2.3.25 %TypedArray%.prototype.reverse ( ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.reverse
    fn reverse(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be the this value.
        let typed_array = typed_array_from_this(vm)?;

        // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
        let typed_array_record = validate_typed_array(vm, &typed_array, Order::SeqCst)?;

        // 3. Let len be TypedArrayLength(taRecord).
        let length = typed_array_length(&typed_array_record);

        // 4. Let middle be floor(len / 2).
        let middle = length / 2;

        // 5. Let lower be 0.
        // 6. Repeat, while lower ≠ middle,
        for lower in 0..middle {
            // a. Let upper be len - lower - 1.
            let upper = length - lower - 1;

            // b. Let upperP be ! ToString(𝔽(upper)).
            let upper_property_key = PropertyKey::from(upper);

            // c. Let lowerP be ! ToString(𝔽(lower)).
            let lower_property_key = PropertyKey::from(lower);

            // d. Let lowerValue be ! Get(O, lowerP).
            let lower_value = typed_array.get(vm, &lower_property_key).must();

            // e. Let upperValue be ! Get(O, upperP).
            let upper_value = typed_array.get(vm, &upper_property_key).must();

            // f. Perform ! Set(O, lowerP, upperValue, true).
            typed_array
                .set(vm, &lower_property_key, upper_value, ShouldThrowExceptions::Yes)
                .must();

            // g. Perform ! Set(O, upperP, lowerValue, true).
            typed_array
                .set(vm, &upper_property_key, lower_value, ShouldThrowExceptions::Yes)
                .must();

            // h. Set lower to lower + 1.
        }

        // 7. Return O.
        Ok(Value::from_object(typed_array))
    }

    // 23.2.3.26 %TypedArray%.prototype.set ( source [ , offset ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.set
    fn set(vm: &Vm) -> ThrowCompletionOr<Value> {
        let source = vm.argument(0);
        let offset = vm.argument(1);

        // 1. Let target be the this value.
        // 2. Perform ? RequireInternalSlot(target, [[TypedArrayName]]).
        // 3. Assert: target has a [[ViewedArrayBuffer]] internal slot.
        let typed_array = typed_array_from_this(vm)?;

        // 4. Let targetOffset be ? ToIntegerOrInfinity(offset).
        let target_offset = offset.to_integer_or_infinity(vm)?;

        // 5. If targetOffset < 0, throw a RangeError exception.
        if target_offset < 0.0 {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::TypedArrayInvalidTargetOffset,
                &[&"positive"],
            );
        }

        // 6. If source is an Object that has a [[TypedArrayName]] internal slot, then
        if source.is_object() && source.as_object().is::<TypedArrayBase>() {
            // a. Perform ? SetTypedArrayFromTypedArray(target, targetOffset, source).
            let source_object = source.as_object();
            set_typed_array_from_typed_array(vm, &typed_array, target_offset, typed_array_of_object(&source_object))?;
        }
        // 7. Else,
        else {
            // a. Perform ? SetTypedArrayFromArrayLike(target, targetOffset, source).
            set_typed_array_from_array_like(vm, &typed_array, target_offset, source)?;
        }

        // 8. Return undefined.
        Ok(Value::UNDEFINED)
    }

    // 23.2.3.27 %TypedArray%.prototype.slice ( start, end ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.slice
    fn slice(vm: &Vm) -> ThrowCompletionOr<Value> {
        let start = vm.argument(0);
        let end = vm.argument(1);

        // 1. Let O be the this value.
        let typed_array = typed_array_from_this(vm)?;

        // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
        let typed_array_record = validate_typed_array(vm, &typed_array, Order::SeqCst)?;

        // 3. Let len be TypedArrayLength(taRecord).
        let mut length = typed_array_length(&typed_array_record);

        // 4. Let relativeStart be ? ToIntegerOrInfinity(start).
        let relative_start = start.to_integer_or_infinity(vm)?;

        // 5. If relativeStart = -∞, let k be 0.
        let mut k: i32 = if relative_start == f64::NEG_INFINITY {
            0
        }
        // 6. Else if relativeStart < 0, let k be max(len + relativeStart, 0).
        else if relative_start < 0.0 {
            (f64::from(length) + relative_start).max(0.0) as i32
        }
        // 7. Else, let k be min(relativeStart, len).
        else {
            relative_start.min(f64::from(length)) as i32
        };

        // 8. If end is undefined, let relativeEnd be len; else let relativeEnd be ? ToIntegerOrInfinity(end).
        let relative_end = if end.is_undefined() {
            f64::from(length)
        } else {
            end.to_integer_or_infinity(vm)?
        };

        // 9. If relativeEnd is -∞, let final be 0.
        let mut final_: i32 = if relative_end == f64::NEG_INFINITY {
            0
        }
        // 10. Else if relativeEnd < 0, let final be max(len + relativeEnd, 0).
        else if relative_end < 0.0 {
            (f64::from(length) + relative_end).max(0.0) as i32
        }
        // 11. Else, let final be min(relativeEnd, len).
        else {
            relative_end.min(f64::from(length)) as i32
        };

        // 12. Let count be max(final - k, 0).
        let mut count = (final_ - k).max(0);

        // 13. Let A be ? TypedArraySpeciesCreate(O, « 𝔽(count) »).
        let realm = vm.current_realm().expect("a builtin runs in a realm");
        let array = typed_array_species_create(
            vm,
            &typed_array,
            || typed_array.create_default(vm, realm, count as u32),
            &[Value::from_i32(count)],
        )?;

        // 14. If count > 0, then
        if count > 0 {
            // a. Set taRecord to MakeTypedArrayWithBufferWitnessRecord(O, seq-cst).
            let typed_array_record = make_typed_array_with_buffer_witness_record(&typed_array, Order::SeqCst);

            // b. If IsTypedArrayOutOfBounds(taRecord) is true, throw a TypeError exception.
            if is_typed_array_out_of_bounds(&typed_array_record) {
                return vm.throw_completion(ErrorKind::TypeError, ErrorType::BufferOutOfBounds, &[&"TypedArray"]);
            }

            // c. Set len to TypedArrayLength(taRecord).
            length = typed_array_length(&typed_array_record);

            // d. Set final to min(final, len).
            final_ = final_.min(length as i32);

            // e. Set count to max(final - k, 0).
            count = (final_ - k).max(0);

            // NB: The source shrank away while its bounds were being coerced, so there is
            //     nothing left to copy. The remaining steps are all no-ops for an empty
            //     range, but the source byte index is now past the end of its buffer.
            if count == 0 {
                return Ok(Value::from_object(array));
            }

            // f. Let srcType be TypedArrayElementType(O).
            // g. Let targetType be TypedArrayElementType(A).

            // h. If srcType is targetType, then
            if typed_array.kind() == array.kind()
                || (typed_array.kind() == Kind::Uint8Array && array.kind() == Kind::Uint8ClampedArray)
                || (typed_array.kind() == Kind::Uint8ClampedArray && array.kind() == Kind::Uint8Array)
            {
                // i. NOTE: The transfer must be performed in a manner that preserves the bit-level encoding of the source data.

                // ii. Let srcBuffer be O.[[ViewedArrayBuffer]].
                let source_buffer = typed_array.viewed_array_buffer();

                // iii. Let targetBuffer be A.[[ViewedArrayBuffer]].
                let target_buffer = array.viewed_array_buffer();

                // iv. Let elementSize be TypedArrayElementSize(O).
                let element_size = typed_array.element_size();

                // v. Let srcByteOffset be O.[[ByteOffset]].
                let source_byte_offset = typed_array.byte_offset();

                // vi. Let srcByteIndex be (k × elementSize) + srcByteOffset.
                let Some(mut source_byte_index) = (k as u32)
                    .checked_mul(element_size)
                    .and_then(|index| index.checked_add(source_byte_offset))
                else {
                    eprintln!("TypedArrayPrototype::slice: source_byte_index overflowed, returning as if succeeded.");
                    return Ok(Value::from_object(array));
                };

                // vii. Let targetByteIndex be A.[[ByteOffset]].
                let mut target_byte_index = array.byte_offset();

                // viii. Let limit be targetByteIndex + (count × elementSize).
                let Some(limit) = (count as u32)
                    .checked_mul(element_size)
                    .and_then(|limit| limit.checked_add(target_byte_index))
                else {
                    eprintln!("TypedArrayPrototype::slice: limit overflowed, returning as if succeeded.");
                    return Ok(Value::from_object(array));
                };

                // OPTIMIZATION: If the buffers are not detached and not shared, we can do a single bulk copy.
                if !target_buffer.is_detached()
                    && !target_buffer.is_shared_array_buffer()
                    && !source_buffer.is_detached()
                    && !source_buffer.is_shared_array_buffer()
                    && !target_buffer.shares_storage_with(&source_buffer)
                {
                    source_buffer.copy_data_to(
                        &target_buffer,
                        source_byte_index as usize,
                        target_byte_index as usize,
                        (limit - target_byte_index) as usize,
                    );
                } else {
                    // ix. Repeat, while targetByteIndex < limit,
                    while target_byte_index < limit {
                        // 1. Let value be GetValueFromBuffer(srcBuffer, srcByteIndex, uint8, true, unordered).
                        let value = source_buffer.get_value(
                            vm,
                            source_byte_index as usize,
                            ElementType::Uint8,
                            true,
                            Order::Unordered,
                            true,
                        );

                        // 2. Perform SetValueInBuffer(targetBuffer, targetByteIndex, uint8, value, true, unordered).
                        target_buffer.set_value(
                            vm,
                            target_byte_index as usize,
                            ElementType::Uint8,
                            value,
                            true,
                            Order::Unordered,
                            true,
                        );

                        // 3. Set srcByteIndex to srcByteIndex + 1.
                        source_byte_index += 1;

                        // 4. Set targetByteIndex to targetByteIndex + 1.
                        target_byte_index += 1;
                    }
                }
            }
            // i. Else,
            else {
                // i. Let n be 0.
                let mut n: u32 = 0;

                // ii. Repeat, while k < final,
                while k < final_ {
                    // 1. Let Pk be ! ToString(𝔽(k)).
                    let property_key = PropertyKey::from(k as u32);

                    // 2. Let kValue be ! Get(O, Pk).
                    let value = typed_array.get(vm, &property_key).must();

                    // 3. Perform ! Set(A, ! ToString(𝔽(n)), kValue, true).
                    array
                        .set(vm, &PropertyKey::from(n), value, ShouldThrowExceptions::Yes)
                        .must();

                    // 4. Set k to k + 1.
                    k += 1;

                    // 5. Set n to n + 1.
                    n += 1;
                }
            }
        }

        // 15. Return A.
        Ok(Value::from_object(array))
    }

    // 23.2.3.28 %TypedArray%.prototype.some ( callbackfn [ , thisArg ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.some
    fn some(vm: &Vm) -> ThrowCompletionOr<Value> {
        let this_arg = vm.argument(1);

        // 1. Let O be the this value.
        let typed_array = typed_array_from_this(vm)?;

        // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
        let typed_array_record = validate_typed_array(vm, &typed_array, Order::SeqCst)?;

        // 3. Let len be TypedArrayLength(taRecord).
        let length = typed_array_length(&typed_array_record) as usize;

        // 4. If IsCallable(callbackfn) is false, throw a TypeError exception.
        let callback_function = callback_from_args(vm, "some")?;

        // 5. Let k be 0.
        // 6. Repeat, while k < len,
        for k in 0..length {
            // a. Let Pk be ! ToString(𝔽(k)).
            let property_key = PropertyKey::from_number(k as u64);

            // b. Let kValue be ! Get(O, Pk).
            let value = typed_array.get(vm, &property_key).must();

            // c. Let testResult be ToBoolean(? Call(callbackfn, thisArg, « kValue, 𝔽(k), O »)).
            let test_result = call_function_object(
                vm,
                callback_function,
                this_arg,
                &[value, size_value(k), Value::from_object(typed_array)],
            )?
            .to_boolean();

            // d. If testResult is true, return true.
            if test_result {
                return Ok(Value::TRUE);
            }

            // e. Set k to k + 1.
        }

        // 7. Return false.
        Ok(Value::FALSE)
    }

    // 23.2.3.29 %TypedArray%.prototype.sort ( comparefn ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.sort
    fn sort(vm: &Vm) -> ThrowCompletionOr<Value> {
        let compare_function = vm.argument(0);

        // 1. If comparefn is not undefined and IsCallable(comparefn) is false, throw a TypeError exception.
        if !compare_function.is_undefined() && !compare_function.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&compare_function]);
        }

        // 2. Let obj be the this value.
        let typed_array = typed_array_from_this(vm)?;

        // 3. Let taRecord be ? ValidateTypedArray(obj, seq-cst).
        let typed_array_record = validate_typed_array(vm, &typed_array, Order::SeqCst)?;

        // 4. Let len be TypedArrayLength(taRecord).
        let length = typed_array_length(&typed_array_record);

        // 5. NOTE: The following closure performs a numeric comparison rather than the string comparison used in 23.1.3.30.
        // 6. Let SortCompare be a new Abstract Closure with parameters (x, y) that captures comparefn and performs the following steps when called:
        let comparefn = (!compare_function.is_undefined()).then(|| compare_function.as_function());
        let sort_compare = |x: Value, y: Value| -> ThrowCompletionOr<f64> {
            // a. Return ? CompareTypedArrayElements(x, y, comparefn).
            compare_typed_array_elements(vm, x, y, comparefn)
        };

        // 7. Let sortedList be ? SortIndexedProperties(obj, len, SortCompare, read-through-holes).
        let sorted_list = sort_indexed_properties(
            vm,
            &typed_array,
            u64::from(length),
            &sort_compare,
            Holes::ReadThroughHoles,
        )?;

        // 8. Let j be 0.
        // 9. Repeat, while j < len,
        for j in 0..length as usize {
            // a. Perform ! Set(obj, ! ToString(𝔽(j)), sortedList[j], true).
            typed_array
                .set(
                    vm,
                    &PropertyKey::from_number(j as u64),
                    sorted_list.get(j).expect("the index is in bounds"),
                    ShouldThrowExceptions::Yes,
                )
                .must();

            // b. Set j to j + 1.
        }

        // 10. Return obj.
        Ok(Value::from_object(typed_array))
    }

    // 23.2.3.30 %TypedArray%.prototype.subarray ( begin, end ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.subarray
    fn subarray(vm: &Vm) -> ThrowCompletionOr<Value> {
        let begin = vm.argument(0);
        let end = vm.argument(1);

        // 1. Let O be the this value.
        // 2. Perform ? RequireInternalSlot(O, [[TypedArrayName]]).
        // 3. Assert: O has a [[ViewedArrayBuffer]] internal slot.
        let typed_array = typed_array_from_this(vm)?;

        // 4. Let buffer be O.[[ViewedArrayBuffer]].
        let buffer = typed_array.viewed_array_buffer();

        // 5. Let srcRecord be MakeTypedArrayWithBufferWitnessRecord(O, seq-cst).
        let source_record = make_typed_array_with_buffer_witness_record(&typed_array, Order::SeqCst);

        // 6. If IsTypedArrayOutOfBounds(srcRecord) is true, then
        let source_length = if is_typed_array_out_of_bounds(&source_record) {
            // a. Let srcLength be 0.
            0
        }
        // 7. Else,
        else {
            // a. Let srcLength be TypedArrayLength(srcRecord).
            typed_array_length(&source_record)
        };

        // 8. Let relativeBegin be ? ToIntegerOrInfinity(begin).
        let relative_begin = begin.to_integer_or_infinity(vm)?;

        // 7. If relativeBegin = -∞, let beginIndex be 0.
        let begin_index: u32 = if relative_begin == f64::NEG_INFINITY {
            0
        }
        // 8. Else if relativeBegin < 0, let beginIndex be max(srcLength + relativeBegin, 0).
        else if relative_begin < 0.0 {
            (f64::from(source_length) + relative_begin).max(0.0) as u32
        }
        // 9. Else, let beginIndex be min(relativeBegin, srcLength).
        else {
            relative_begin.min(f64::from(source_length)) as u32
        };

        // 12. Let elementSize be TypedArrayElementSize(O).
        let element_size = typed_array.element_size();

        // 13. Let srcByteOffset be O.[[ByteOffset]].
        let source_byte_offset = typed_array.byte_offset();

        // 14. Let beginByteOffset be srcByteOffset + beginIndex × elementSize.
        let Some(begin_byte_offset) = begin_index
            .checked_mul(element_size)
            .and_then(|offset| offset.checked_add(source_byte_offset))
        else {
            eprintln!("TypedArrayPrototype::begin_byte_offset: limit overflowed, returning as if succeeded.");
            return Ok(Value::from_object(typed_array));
        };

        // NB: The arguments live on the stack, where the collector sees them.
        let arguments: [Value; 3];
        let argument_count;
        let mut new_length: Option<u32> = None;

        // 15. If O.[[ArrayLength]] is auto and end is undefined, then
        if typed_array.array_length().is_auto() && end.is_undefined() {
            // a. Let argumentsList be « buffer, 𝔽(beginByteOffset) ».
            arguments = [
                Value::from_object(buffer),
                index_value(begin_byte_offset),
                Value::UNDEFINED,
            ];
            argument_count = 2;
        }
        // 16. Else,
        else {
            // a. If end is undefined, let relativeEnd be srcLength; else let relativeEnd be ? ToIntegerOrInfinity(end).
            let relative_end = if end.is_undefined() {
                f64::from(source_length)
            } else {
                end.to_integer_or_infinity(vm)?
            };

            // 11. If relativeEnd = -∞, let endIndex be 0.
            let end_index: u32 = if relative_end == f64::NEG_INFINITY {
                0
            }
            // 12. Else if relativeEnd < 0, let endIndex be max(srcLength + relativeEnd, 0).
            else if relative_end < 0.0 {
                (f64::from(source_length) + relative_end).max(0.0) as u32
            }
            // 13. Else, let endIndex be min(relativeEnd, srcLength).
            else {
                relative_end.min(f64::from(source_length)) as u32
            };

            // e. Let newLength be max(endIndex - beginIndex, 0).
            let length = end_index.saturating_sub(begin_index);
            new_length = Some(length);

            // f. Let argumentsList be « buffer, 𝔽(beginByteOffset), 𝔽(newLength) ».
            arguments = [
                Value::from_object(buffer),
                index_value(begin_byte_offset),
                index_value(length),
            ];
            argument_count = 3;
        }

        let realm = vm.current_realm().expect("a builtin runs in a realm");

        // 17. Return ? TypedArraySpeciesCreate(O, argumentsList).
        let result = typed_array_species_create(
            vm,
            &typed_array,
            || {
                let view = typed_array.create_default_view_on_buffer(vm, realm, buffer);
                initialize_typed_array_from_array_buffer(
                    vm,
                    &view,
                    buffer,
                    index_value(begin_byte_offset),
                    new_length.map_or(Value::UNDEFINED, index_value),
                )?;
                Ok(view)
            },
            &arguments[..argument_count],
        )?;
        Ok(Value::from_object(result))
    }

    // 23.2.3.31 %TypedArray%.prototype.toLocaleString ( [ reserved1 [ , reserved2 ] ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.tolocalestring
    // 19.5.1 Array.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sup-array.prototype.tolocalestring
    fn to_locale_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        let locales = vm.argument(0);
        let options = vm.argument(1);

        // This function is not generic. ValidateTypedArray is applied to the this value prior to evaluating the algorithm.
        // If its result is an abrupt completion that exception is thrown instead of evaluating the algorithm.

        // 1. Let array be ? ToObject(this value).
        let typed_array = typed_array_from_this(vm)?;

        // 2. Let len be ? ToLength(? Get(array, "length")).
        // The implementation of the algorithm may be optimized with the knowledge that the this value is an object that
        // has a fixed length and whose integer-indexed properties are not sparse.
        let typed_array_record = validate_typed_array(vm, &typed_array, Order::SeqCst)?;
        let length = typed_array_length(&typed_array_record) as usize;

        // 3. Let separator be the implementation-defined list-separator String value appropriate for the host environment's current locale (such as ", ").
        const SEPARATOR: &str = ",";

        // 4. Let R be the empty String.
        let mut builder = Utf16StringBuilder::new();
        let mut result_length = 0usize;

        // 5. Let k be 0.
        // 6. Repeat, while k < len,
        for k in 0..length {
            // a. If k > 0, then
            if k > 0 {
                // i. Set R to the string-concatenation of R and separator.
                result_length =
                    checked_js_string_length_sum(vm, result_length, 1, ErrorType::StringSizeMustNotOverflow)?;
                builder.append_ascii(SEPARATOR);
            }

            // b. Let nextElement be ? Get(array, ! ToString(k)).
            let next_element = typed_array.get(vm, &PropertyKey::from_number(k as u64))?;

            // c. If nextElement is not undefined or null, then
            if !next_element.is_nullish() {
                // i. Let S be ? ToString(? Invoke(nextElement, "toLocaleString", « locales, options »)).
                let locale_string_value = next_element.invoke(vm, &vm.names.toLocaleString, &[locales, options])?;
                let locale_string = locale_string_value.to_utf16_string(vm)?;

                // ii. Set R to the string-concatenation of R and S.
                result_length = checked_js_string_length_sum(
                    vm,
                    result_length,
                    Utf16View::of_string(&locale_string).length_in_code_units(),
                    ErrorType::StringSizeMustNotOverflow,
                )?;
                builder.append(Utf16View::of_string(&locale_string));
            }

            // d. Set k to k + 1.
        }

        // 7. Return R.
        Ok(Value::from_string(PrimitiveString::create(
            vm,
            builder.to_utf16_string(),
        )))
    }

    // 23.2.3.32 %TypedArray%.prototype.toReversed ( ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.toreversed
    fn to_reversed(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be the this value.
        let typed_array = typed_array_from_this(vm)?;

        // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
        let typed_array_record = validate_typed_array(vm, &typed_array, Order::SeqCst)?;

        // 3. Let length be TypedArrayLength(taRecord).
        let length = typed_array_length(&typed_array_record);

        // 4. Let A be ? TypedArrayCreateSameType(O, « 𝔽(length) »).
        let array = typed_array_create_same_type(vm, &typed_array, &[index_value(length)])?;

        // 5. Let k be 0.
        // 6. Repeat, while k < length,
        for k in 0..length {
            // a. Let from be ! ToString(𝔽(length - k - 1)).
            let from = PropertyKey::from(length - k - 1);

            // b. Let Pk be ! ToString(𝔽(k)).
            let property_key = PropertyKey::from(k);

            // c. Let fromValue be ! Get(O, from).
            let from_value = typed_array.get(vm, &from).must();

            // d. Perform ! Set(A, Pk, fromValue, true).
            array
                .set(vm, &property_key, from_value, ShouldThrowExceptions::Yes)
                .must();

            // e. Set k to k + 1.
        }

        // 7. Return A.
        Ok(Value::from_object(array))
    }

    // 23.2.3.33 %TypedArray%.prototype.toSorted ( comparefn ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.tosorted
    fn to_sorted(vm: &Vm) -> ThrowCompletionOr<Value> {
        let compare_function = vm.argument(0);

        // 1. If comparefn is not undefined and IsCallable(comparefn) is false, throw a TypeError exception.
        if !compare_function.is_undefined() && !compare_function.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&compare_function]);
        }

        // 2. Let O be the this value.
        let typed_array = typed_array_from_this(vm)?;

        // 3. Let taRecord be ? ValidateTypedArray(O, seq-cst).
        let typed_array_record = validate_typed_array(vm, &typed_array, Order::SeqCst)?;

        // 4. Let len be TypedArrayLength(taRecord).
        let length = typed_array_length(&typed_array_record);

        // 5. Let A be ? TypedArrayCreateSameType(O, « 𝔽(len) »).
        let array = typed_array_create_same_type(vm, &typed_array, &[index_value(length)])?;

        // 6. NOTE: The following closure performs a numeric comparison rather than the string comparison used in 23.1.3.34.
        let comparefn = (!compare_function.is_undefined()).then(|| compare_function.as_function());
        let sort_compare = |x: Value, y: Value| -> ThrowCompletionOr<f64> {
            // a. Return ? CompareTypedArrayElements(x, y, comparefn).
            compare_typed_array_elements(vm, x, y, comparefn)
        };

        // 8. Let sortedList be ? SortIndexedProperties(O, len, SortCompare, read-through-holes).
        let sorted_list = sort_indexed_properties(
            vm,
            &typed_array,
            u64::from(length),
            &sort_compare,
            Holes::ReadThroughHoles,
        )?;

        // 9. Let j be 0.
        // 10. Repeat, while j < len,
        for j in 0..length as usize {
            // a. Perform ! Set(A, ! ToString(𝔽(j)), sortedList[j], true).
            array
                .set(
                    vm,
                    &PropertyKey::from_number(j as u64),
                    sorted_list.get(j).expect("the index is in bounds"),
                    ShouldThrowExceptions::Yes,
                )
                .must();

            // b. Set j to j + 1.
        }

        // 11. Return A.
        Ok(Value::from_object(array))
    }

    // 23.2.3.35 %TypedArray%.prototype.values ( ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.values
    fn values(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        // 1. Let O be the this value.
        let typed_array = typed_array_from_this(vm)?;

        // 2. Perform ? ValidateTypedArray(O, seq-cst).
        validate_typed_array(vm, &typed_array, Order::SeqCst)?;

        // 3. Return CreateArrayIterator(O, value).
        Ok(Value::from_object(ArrayIterator::create(
            vm,
            realm,
            Value::from_object(typed_array),
            PropertyKind::Value,
        )))
    }

    // 23.2.3.36 %TypedArray%.prototype.with ( index, value ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.with
    fn with(vm: &Vm) -> ThrowCompletionOr<Value> {
        let index = vm.argument(0);
        let value = vm.argument(1);

        // 1. Let O be the this value.
        let typed_array = typed_array_from_this(vm)?;

        // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
        let typed_array_record = validate_typed_array(vm, &typed_array, Order::SeqCst)?;

        // 3. Let len be TypedArrayLength(taRecord).
        let length = typed_array_length(&typed_array_record);

        // 4. Let relativeIndex be ? ToIntegerOrInfinity(index).
        let relative_index = index.to_integer_or_infinity(vm)?;

        // 5. If relativeIndex ≥ 0, let actualIndex be relativeIndex.
        let actual_index = if relative_index >= 0.0 {
            relative_index
        }
        // 6. Else, let actualIndex be len + relativeIndex.
        else {
            f64::from(length) + relative_index
        };

        // 7. If O.[[ContentType]] is BigInt, let numericValue be ? ToBigInt(value).
        let numeric_value = if typed_array.content_type() == ContentType::BigInt {
            Value::from_bigint(value.to_bigint(vm)?)
        }
        // 8. Else, let numericValue be ? ToNumber(value).
        else {
            value.to_number(vm)?
        };

        // 9. If IsValidIntegerIndex(O, 𝔽(actualIndex)) is false, throw a RangeError exception.
        if !is_valid_integer_index(
            &typed_array,
            canonical_index_from_double(vm, CanonicalIndexType::Index, actual_index)?,
        ) {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::TypedArrayInvalidIntegerIndex,
                &[&AkDouble(actual_index)],
            );
        }

        // 10. Let A be ? TypedArrayCreateSameType(O, « 𝔽(len) »).
        let array = typed_array_create_same_type(vm, &typed_array, &[index_value(length)])?;

        // 11. Let k be 0.
        // 12. Repeat, while k < len,
        for k in 0..length as usize {
            // a. Let Pk be ! ToString(𝔽(k)).
            let property_key = PropertyKey::from_number(k as u64);

            // b. If k is actualIndex, let fromValue be numericValue.
            let from_value = if k as f64 == actual_index {
                numeric_value
            }
            // c. Else, let fromValue be ! Get(O, Pk).
            else {
                typed_array.get(vm, &property_key).must()
            };

            // d. Perform ! Set(A, Pk, fromValue, true).
            // AD-HOC: The specification asserts this Set cannot throw, but resizing a BigInt source during
            // value conversion can make a later Get return undefined, which throws during the Set.
            array.set(vm, &property_key, from_value, ShouldThrowExceptions::Yes)?;

            // e. Set k to k + 1.
        }

        // 13. Return A.
        Ok(Value::from_object(array))
    }

    // 23.2.3.38 get %TypedArray%.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-get-%typedarray%.prototype-@@tostringtag
    #[allow(clippy::unnecessary_wraps, reason = "native functions return a completion")]
    fn to_string_tag_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be the this value.
        let this_value = vm.this_value();

        // 2. If O is not an Object, return undefined.
        if !this_value.is_object() {
            return Ok(Value::UNDEFINED);
        }

        let this_object = this_value.as_object();

        // 3. If O does not have a [[TypedArrayName]] internal slot, return undefined.
        if !this_object.is_typed_array() {
            return Ok(Value::UNDEFINED);
        }

        // 4. Let name be O.[[TypedArrayName]].
        // 5. Assert: name is a String.
        // 6. Return name.
        Ok(Value::from_string(PrimitiveString::create_from_fly_string(
            vm,
            &typed_array_of_object(&this_object).element_name(vm),
        )))
    }
}
