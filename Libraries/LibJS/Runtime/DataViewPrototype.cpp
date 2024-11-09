/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/TypeCasts.h>
#include <LibJS/Runtime/DataViewPrototype.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS {

JS_DEFINE_ALLOCATOR(DataViewPrototype);

DataViewPrototype::DataViewPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void DataViewPrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);
    u8 attr = Attribute::Writable | Attribute::Configurable;

    define_native_function(realm, vm.names.getBigInt64, get_big_int_64, 1, attr);
    define_native_function(realm, vm.names.getBigUint64, get_big_uint_64, 1, attr);
    define_native_function(realm, vm.names.getFloat32, get_float_32, 1, attr);
    define_native_function(realm, vm.names.getFloat64, get_float_64, 1, attr);
    define_native_function(realm, vm.names.getInt8, get_int_8, 1, attr);
    define_native_function(realm, vm.names.getInt16, get_int_16, 1, attr);
    define_native_function(realm, vm.names.getInt32, get_int_32, 1, attr);
    define_native_function(realm, vm.names.getUint8, get_uint_8, 1, attr);
    define_native_function(realm, vm.names.getUint16, get_uint_16, 1, attr);
    define_native_function(realm, vm.names.getUint32, get_uint_32, 1, attr);
    define_native_function(realm, vm.names.setBigInt64, set_big_int_64, 2, attr);
    define_native_function(realm, vm.names.setBigUint64, set_big_uint_64, 2, attr);
    define_native_function(realm, vm.names.setFloat32, set_float_32, 2, attr);
    define_native_function(realm, vm.names.setFloat64, set_float_64, 2, attr);
    define_native_function(realm, vm.names.setInt8, set_int_8, 2, attr);
    define_native_function(realm, vm.names.setInt16, set_int_16, 2, attr);
    define_native_function(realm, vm.names.setInt32, set_int_32, 2, attr);
    define_native_function(realm, vm.names.setUint8, set_uint_8, 2, attr);
    define_native_function(realm, vm.names.setUint16, set_uint_16, 2, attr);
    define_native_function(realm, vm.names.setUint32, set_uint_32, 2, attr);

    define_native_accessor(realm, vm.names.buffer, buffer_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.byteLength, byte_length_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.byteOffset, byte_offset_getter, {}, Attribute::Configurable);

    // 25.3.4.25 DataView.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-dataview.prototype-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, vm.names.DataView.as_string()), Attribute::Configurable);
}

// 25.3.1.5 GetViewValue ( view, requestIndex, isLittleEndian, type ), https://tc39.es/ecma262/#sec-getviewvalue
template<typename T>
static ThrowCompletionOr<Value> get_view_value(VM& vm, Value request_index, Value is_little_endian)
{
    // 1. Perform ? RequireInternalSlot(view, [[DataView]]).
    // 2. Assert: view has a [[ViewedArrayBuffer]] internal slot.
    auto view = TRY(DataViewPrototype::typed_this_value(vm));

    // 3. Let getIndex be ? ToIndex(requestIndex).
    auto get_index = TRY(request_index.to_index(vm));

    // 4. Set isLittleEndian to ToBoolean(isLittleEndian).
    auto little_endian = is_little_endian.to_boolean();

    // 5. Let viewOffset be view.[[ByteOffset]].
    auto view_offset = view->byte_offset();

    // 6. Let viewRecord be MakeDataViewWithBufferWitnessRecord(view, unordered).
    auto view_record = make_data_view_with_buffer_witness_record(*view, ArrayBuffer::Order::Unordered);

    // 7. NOTE: Bounds checking is not a synchronizing operation when view's backing buffer is a growable SharedArrayBuffer.
    // 8. If IsViewOutOfBounds(viewRecord) is true, throw a TypeError exception.
    if (is_view_out_of_bounds(view_record))
        return vm.throw_completion<TypeError>(ErrorType::BufferOutOfBounds, "DataView"sv);

    // 9. Let viewSize be GetViewByteLength(viewRecord).
    auto view_size = get_view_byte_length(view_record);

    // 10. Let elementSize be the Element Size value specified in Table 71 for Element Type type.
    auto element_size = sizeof(T);

    // 11. If getIndex + elementSize > viewSize, throw a RangeError exception.
    Checked<size_t> end_index = get_index;
    end_index += element_size;

    if (end_index.has_overflow() || end_index.value() > view_size)
        return vm.throw_completion<RangeError>(ErrorType::DataViewOutOfRangeByteOffset, get_index, view_size);

    // 12. Let bufferIndex be getIndex + viewOffset.
    Checked<size_t> buffer_index = get_index;
    buffer_index += view_offset;

    if (buffer_index.has_overflow())
        return vm.throw_completion<RangeError>(ErrorType::DataViewOutOfRangeByteOffset, get_index, view_size);

    // 13. Return GetValueFromBuffer(view.[[ViewedArrayBuffer]], bufferIndex, type, false, unordered, isLittleEndian).
    return view->viewed_array_buffer()->get_value<T>(buffer_index.value(), false, ArrayBuffer::Order::Unordered, little_endian);
}

// 25.3.1.6 SetViewValue ( view, requestIndex, isLittleEndian, type, value ), https://tc39.es/ecma262/#sec-setviewvalue
template<typename T>
static ThrowCompletionOr<Value> set_view_value(VM& vm, Value request_index, Value is_little_endian, Value value)
{
    // 1. Perform ? RequireInternalSlot(view, [[DataView]]).
    // 2. Assert: view has a [[ViewedArrayBuffer]] internal slot.
    auto view = TRY(DataViewPrototype::typed_this_value(vm));

    // 3. Let getIndex be ? ToIndex(requestIndex).
    auto get_index = TRY(request_index.to_index(vm));

    Value number_value;

    // 4. If IsBigIntElementType(type) is true, let numberValue be ? ToBigInt(value).
    if constexpr (IsIntegral<T> && sizeof(T) == 8)
        number_value = TRY(value.to_bigint(vm));
    // 5. Otherwise, let numberValue be ? ToNumber(value).
    else
        number_value = TRY(value.to_number(vm));

    // 6. Set isLittleEndian to ToBoolean(isLittleEndian).
    auto little_endian = is_little_endian.to_boolean();

    // 7. Let viewOffset be view.[[ByteOffset]].
    auto view_offset = view->byte_offset();

    // 8. Let viewRecord be MakeDataViewWithBufferWitnessRecord(view, unordered).
    auto view_record = make_data_view_with_buffer_witness_record(*view, ArrayBuffer::Order::Unordered);

    // 9. NOTE: Bounds checking is not a synchronizing operation when view's backing buffer is a growable SharedArrayBuffer.
    // 10. If IsViewOutOfBounds(viewRecord) is true, throw a TypeError exception.
    if (is_view_out_of_bounds(view_record))
        return vm.throw_completion<TypeError>(ErrorType::BufferOutOfBounds, "DataView"sv);

    // 11. Let viewSize be GetViewByteLength(viewRecord).
    auto view_size = get_view_byte_length(view_record);

    // 12. Let elementSize be the Element Size value specified in Table 71 for Element Type type.
    auto element_size = sizeof(T);

    // 13. If getIndex + elementSize > viewSize, throw a RangeError exception.
    Checked<size_t> end_index = get_index;
    end_index += element_size;

    if (end_index.has_overflow() || end_index.value() > view_size)
        return vm.throw_completion<RangeError>(ErrorType::DataViewOutOfRangeByteOffset, get_index, view_size);

    // 14. Let bufferIndex be getIndex + viewOffset.
    Checked<size_t> buffer_index = get_index;
    buffer_index += view_offset;

    if (buffer_index.has_overflow())
        return vm.throw_completion<RangeError>(ErrorType::DataViewOutOfRangeByteOffset, get_index, view_size);

    // 15. Perform SetValueInBuffer(view.[[ViewedArrayBuffer]], bufferIndex, type, numberValue, false, unordered, isLittleEndian).
    view->viewed_array_buffer()->set_value<T>(buffer_index.value(), number_value, false, ArrayBuffer::Order::Unordered, little_endian);

    // 16. Return undefined.
    return js_undefined();
}

// 25.3.4.1 get DataView.prototype.buffer, https://tc39.es/ecma262/#sec-get-dataview.prototype.buffer
JS_DEFINE_NATIVE_FUNCTION(DataViewPrototype::buffer_getter)
{
    // 1. Let O be the this value.
    // 2. Perform ? RequireInternalSlot(O, [[DataView]]).
    // 3. Assert: O has a [[ViewedArrayBuffer]] internal slot.
    auto data_view = TRY(typed_this_value(vm));

    // 4. Let buffer be O.[[ViewedArrayBuffer]].
    // 5. Return buffer.
    return data_view->viewed_array_buffer();
}

// 25.3.4.2 get DataView.prototype.byteLength, https://tc39.es/ecma262/#sec-get-dataview.prototype.bytelength
JS_DEFINE_NATIVE_FUNCTION(DataViewPrototype::byte_length_getter)
{
    // 1. Let O be the this value.
    // 2. Perform ? RequireInternalSlot(O, [[DataView]]).
    // 3. Assert: O has a [[ViewedArrayBuffer]] internal slot.
    auto data_view = TRY(typed_this_value(vm));

    // 4. Let viewRecord be MakeDataViewWithBufferWitnessRecord(O, seq-cst).
    auto view_record = make_data_view_with_buffer_witness_record(data_view, ArrayBuffer::Order::SeqCst);

    // 5. If IsViewOutOfBounds(viewRecord) is true, throw a TypeError exception.
    if (is_view_out_of_bounds(view_record))
        return vm.throw_completion<TypeError>(ErrorType::BufferOutOfBounds, "DataView"sv);

    // 6. Let size be GetViewByteLength(viewRecord).
    auto size = get_view_byte_length(view_record);

    // 7. Return 𝔽(size).
    return Value { size };
}

// 25.3.4.3 get DataView.prototype.byteOffset, https://tc39.es/ecma262/#sec-get-dataview.prototype.byteoffset
JS_DEFINE_NATIVE_FUNCTION(DataViewPrototype::byte_offset_getter)
{
    // 1. Let O be the this value.
    // 2. Perform ? RequireInternalSlot(O, [[DataView]]).
    // 3. Assert: O has a [[ViewedArrayBuffer]] internal slot.
    auto data_view = TRY(typed_this_value(vm));

    // 4. Let viewRecord be MakeDataViewWithBufferWitnessRecord(O, seq-cst).
    auto view_record = make_data_view_with_buffer_witness_record(data_view, ArrayBuffer::Order::SeqCst);

    // 5. If IsViewOutOfBounds(viewRecord) is true, throw a TypeError exception.
    if (is_view_out_of_bounds(view_record))
        return vm.throw_completion<TypeError>(ErrorType::BufferOutOfBounds, "DataView"sv);

    // 6. Let offset be O.[[ByteOffset]].
    auto offset = data_view->byte_offset();

    // 7. Return 𝔽(offset).
    return Value { offset };
}

// 25.3.4.5 DataView.prototype.getBigInt64 ( byteOffset [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.getbigint64
JS_DEFINE_NATIVE_FUNCTION(DataViewPrototype::get_big_int_64)
{
    // 1. Let v be the this value.
    // 2. Return ? GetViewValue(v, byteOffset, littleEndian, BigInt64).
    return get_view_value<i64>(vm, vm.argument(0), vm.argument(1));
}

// 25.3.4.6 DataView.prototype.getBigUint64 ( byteOffset [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.getbiguint64
JS_DEFINE_NATIVE_FUNCTION(DataViewPrototype::get_big_uint_64)
{
    // 1. Let v be the this value.
    // 2. Return ? GetViewValue(v, byteOffset, littleEndian, BigUint64).
    return get_view_value<u64>(vm, vm.argument(0), vm.argument(1));
}

// 25.3.4.7 DataView.prototype.getFloat32 ( byteOffset [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.getfloat32
JS_DEFINE_NATIVE_FUNCTION(DataViewPrototype::get_float_32)
{
    // 1. Let v be the this value.
    // 2. If littleEndian is not present, set littleEndian to false.
    // 3. Return ? GetViewValue(v, byteOffset, littleEndian, Float32).
    return get_view_value<float>(vm, vm.argument(0), vm.argument(1));
}

// 25.3.4.8 DataView.prototype.getFloat64 ( byteOffset [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.getfloat64
JS_DEFINE_NATIVE_FUNCTION(DataViewPrototype::get_float_64)
{
    // 1. Let v be the this value.
    // 2. If littleEndian is not present, set littleEndian to false.
    // 3. Return ? GetViewValue(v, byteOffset, littleEndian, Float64).
    return get_view_value<double>(vm, vm.argument(0), vm.argument(1));
}

// 25.3.4.9 DataView.prototype.getInt8 ( byteOffset ), https://tc39.es/ecma262/#sec-dataview.prototype.getint8
JS_DEFINE_NATIVE_FUNCTION(DataViewPrototype::get_int_8)
{
    // 1. Let v be the this value.
    // 2. Return ? GetViewValue(v, byteOffset, true, Int8).
    return get_view_value<i8>(vm, vm.argument(0), Value(true));
}

// 25.3.4.10 DataView.prototype.getInt16 ( byteOffset [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.getint16
JS_DEFINE_NATIVE_FUNCTION(DataViewPrototype::get_int_16)
{
    // 1. Let v be the this value.
    // 2. If littleEndian is not present, set littleEndian to false.
    // 3. Return ? GetViewValue(v, byteOffset, littleEndian, Int16).
    return get_view_value<i16>(vm, vm.argument(0), vm.argument(1));
}

// 25.3.4.11 DataView.prototype.getInt32 ( byteOffset [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.getint32
JS_DEFINE_NATIVE_FUNCTION(DataViewPrototype::get_int_32)
{
    // 1. Let v be the this value.
    // 2. If littleEndian is not present, set littleEndian to false.
    // 3. Return ? GetViewValue(v, byteOffset, littleEndian, Int32).
    return get_view_value<i32>(vm, vm.argument(0), vm.argument(1));
}

// 25.3.4.12 DataView.prototype.getUint8 ( byteOffset ), https://tc39.es/ecma262/#sec-dataview.prototype.getuint8
JS_DEFINE_NATIVE_FUNCTION(DataViewPrototype::get_uint_8)
{
    // 1. Let v be the this value.
    // 2. Return ? GetViewValue(v, byteOffset, true, Uint8).
    return get_view_value<u8>(vm, vm.argument(0), Value(true));
}

// 25.3.4.13 DataView.prototype.getUint16 ( byteOffset [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.getuint16
JS_DEFINE_NATIVE_FUNCTION(DataViewPrototype::get_uint_16)
{
    // 1. Let v be the this value.
    // 2. If littleEndian is not present, set littleEndian to false.
    // 3. Return ? GetViewValue(v, byteOffset, littleEndian, Uint16).
    return get_view_value<u16>(vm, vm.argument(0), vm.argument(1));
}

// 25.3.4.14 DataView.prototype.getUint32 ( byteOffset [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.getuint32
JS_DEFINE_NATIVE_FUNCTION(DataViewPrototype::get_uint_32)
{
    // 1. Let v be the this value.
    // 2. If littleEndian is not present, set littleEndian to false.
    // 3. Return ? GetViewValue(v, byteOffset, littleEndian, Uint32).
    return get_view_value<u32>(vm, vm.argument(0), vm.argument(1));
}

// 25.3.4.15 DataView.prototype.setBigInt64 ( byteOffset, value [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.setbigint64
JS_DEFINE_NATIVE_FUNCTION(DataViewPrototype::set_big_int_64)
{
    // 1. Let v be the this value.
    // 2. Return ? SetViewValue(v, byteOffset, littleEndian, BigInt64, value).
    return set_view_value<i64>(vm, vm.argument(0), vm.argument(2), vm.argument(1));
}

// 25.3.4.16 DataView.prototype.setBigUint64 ( byteOffset, value [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.setbiguint64
JS_DEFINE_NATIVE_FUNCTION(DataViewPrototype::set_big_uint_64)
{
    // 1. Let v be the this value.
    // 2. Return ? SetViewValue(v, byteOffset, littleEndian, BigUint64, value).
    return set_view_value<u64>(vm, vm.argument(0), vm.argument(2), vm.argument(1));
}

// 25.3.4.17 DataView.prototype.setFloat32 ( byteOffset, value [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.setfloat32
JS_DEFINE_NATIVE_FUNCTION(DataViewPrototype::set_float_32)
{
    // 1. Let v be the this value.
    // 2. If littleEndian is not present, set littleEndian to false.
    // 3. Return ? SetViewValue(v, byteOffset, littleEndian, Float32, value).
    return set_view_value<float>(vm, vm.argument(0), vm.argument(2), vm.argument(1));
}

// 25.3.4.18 DataView.prototype.setFloat64 ( byteOffset, value [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.setfloat64
JS_DEFINE_NATIVE_FUNCTION(DataViewPrototype::set_float_64)
{
    // 1. Let v be the this value.
    // 2. If littleEndian is not present, set littleEndian to false.
    // 3. Return ? SetViewValue(v, byteOffset, littleEndian, Float64, value).
    return set_view_value<double>(vm, vm.argument(0), vm.argument(2), vm.argument(1));
}

// 25.3.4.19 DataView.prototype.setInt8 ( byteOffset, value ), https://tc39.es/ecma262/#sec-dataview.prototype.setint8
JS_DEFINE_NATIVE_FUNCTION(DataViewPrototype::set_int_8)
{
    // 1. Let v be the this value.
    // 2. Return ? SetViewValue(v, byteOffset, true, Int8, value).
    return set_view_value<i8>(vm, vm.argument(0), Value(true), vm.argument(1));
}

// 25.3.4.20 DataView.prototype.setInt16 ( byteOffset, value [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.setint16
JS_DEFINE_NATIVE_FUNCTION(DataViewPrototype::set_int_16)
{
    // 1. Let v be the this value.
    // 2. If littleEndian is not present, set littleEndian to false.
    // 3. Return ? SetViewValue(v, byteOffset, littleEndian, Int16, value).
    return set_view_value<i16>(vm, vm.argument(0), vm.argument(2), vm.argument(1));
}

// 25.3.4.21 DataView.prototype.setInt32 ( byteOffset, value [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.setint32
JS_DEFINE_NATIVE_FUNCTION(DataViewPrototype::set_int_32)
{
    // 1. Let v be the this value.
    // 2. If littleEndian is not present, set littleEndian to false.
    // 3. Return ? SetViewValue(v, byteOffset, littleEndian, Int32, value).
    return set_view_value<i32>(vm, vm.argument(0), vm.argument(2), vm.argument(1));
}

// 25.3.4.22 DataView.prototype.setUint8 ( byteOffset, value ), https://tc39.es/ecma262/#sec-dataview.prototype.setuint8
JS_DEFINE_NATIVE_FUNCTION(DataViewPrototype::set_uint_8)
{
    // 1. Let v be the this value.
    // 2. Return ? SetViewValue(v, byteOffset, true, Uint8, value).
    return set_view_value<u8>(vm, vm.argument(0), Value(true), vm.argument(1));
}

// 25.3.4.23 DataView.prototype.setUint16 ( byteOffset, value [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.setuint16
JS_DEFINE_NATIVE_FUNCTION(DataViewPrototype::set_uint_16)
{
    // 1. Let v be the this value.
    // 2. If littleEndian is not present, set littleEndian to false.
    // 3. Return ? SetViewValue(v, byteOffset, littleEndian, Uint16, value).
    return set_view_value<u16>(vm, vm.argument(0), vm.argument(2), vm.argument(1));
}

// 25.3.4.24 DataView.prototype.setUint32 ( byteOffset, value [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.setuint32
JS_DEFINE_NATIVE_FUNCTION(DataViewPrototype::set_uint_32)
{
    // 1. Let v be the this value.
    // 2. If littleEndian is not present, set littleEndian to false.
    // 3. Return ? SetViewValue(v, byteOffset, littleEndian, Uint32, value).
    return set_view_value<u32>(vm, vm.argument(0), vm.argument(2), vm.argument(1));
}

}
