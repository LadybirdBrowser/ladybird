/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::array_buffer::{ElementType, Order};
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::data_view::{
    DataView, get_view_byte_length, is_view_out_of_bounds, make_data_view_with_buffer_witness_record,
};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::typed_this_value;
use crate::runtime::realm::Realm;

#[repr(C)]
#[derive(Trace)]
pub struct DataViewPrototype {
    base: Object,
}

define_object_class!(DataViewPrototype, extends: [Object], methods: {
    initialize: DataViewPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

fn this_data_view(vm: &Vm) -> ThrowCompletionOr<Gc<DataView>> {
    typed_this_value::<DataView>(vm, "DataView")
}

// 25.3.1.5 GetViewValue ( view, requestIndex, isLittleEndian, type ), https://tc39.es/ecma262/#sec-getviewvalue
fn get_view_value(
    vm: &Vm,
    request_index: Value,
    is_little_endian: Value,
    element_type: ElementType,
) -> ThrowCompletionOr<Value> {
    // 1. Perform ? RequireInternalSlot(view, [[DataView]]).
    // 2. Assert: view has a [[ViewedArrayBuffer]] internal slot.
    let view = this_data_view(vm)?;

    // 3. Let getIndex be ? ToIndex(requestIndex).
    let get_index = request_index.to_index(vm)? as usize;

    // 4. Set isLittleEndian to ToBoolean(isLittleEndian).
    let little_endian = is_little_endian.to_boolean();

    // 5. Let viewOffset be view.[[ByteOffset]].
    let view_offset = view.byte_offset();

    // 6. Let viewRecord be MakeDataViewWithBufferWitnessRecord(view, unordered).
    let view_record = make_data_view_with_buffer_witness_record(view, Order::Unordered);

    // 7. NOTE: Bounds checking is not a synchronizing operation when view's backing buffer is a growable SharedArrayBuffer.
    // 8. If IsViewOutOfBounds(viewRecord) is true, throw a TypeError exception.
    if is_view_out_of_bounds(&view_record) {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::BufferOutOfBounds, &[&"DataView"]);
    }

    // 9. Let viewSize be GetViewByteLength(viewRecord).
    let view_size = get_view_byte_length(&view_record);

    // 10. Let elementSize be the Element Size value specified in Table 71 for Element Type type.
    let element_size = element_type.size();

    // 11. If getIndex + elementSize > viewSize, throw a RangeError exception.
    let end_index = get_index.checked_add(element_size);

    if end_index.is_none_or(|end_index| end_index > view_size as usize) {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::DataViewOutOfRangeByteOffset,
            &[&get_index, &view_size],
        );
    }

    // 12. Let bufferIndex be getIndex + viewOffset.
    let Some(buffer_index) = get_index.checked_add(view_offset as usize) else {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::DataViewOutOfRangeByteOffset,
            &[&get_index, &view_size],
        );
    };

    // 13. Return GetValueFromBuffer(view.[[ViewedArrayBuffer]], bufferIndex, type, false, unordered, isLittleEndian).
    Ok(view
        .viewed_array_buffer()
        .get_value(vm, buffer_index, element_type, false, Order::Unordered, little_endian))
}

// 25.3.1.6 SetViewValue ( view, requestIndex, isLittleEndian, type, value ), https://tc39.es/ecma262/#sec-setviewvalue
fn set_view_value(
    vm: &Vm,
    request_index: Value,
    is_little_endian: Value,
    element_type: ElementType,
    value: Value,
) -> ThrowCompletionOr<Value> {
    // 1. Perform ? RequireInternalSlot(view, [[DataView]]).
    // 2. Assert: view has a [[ViewedArrayBuffer]] internal slot.
    let view = this_data_view(vm)?;

    // 3. Let getIndex be ? ToIndex(requestIndex).
    let get_index = request_index.to_index(vm)? as usize;

    // 4. If IsBigIntElementType(type) is true, let numberValue be ? ToBigInt(value).
    let number_value = if matches!(element_type, ElementType::BigInt64 | ElementType::BigUint64) {
        Value::from_bigint(value.to_bigint(vm)?)
    }
    // 5. Otherwise, let numberValue be ? ToNumber(value).
    else {
        value.to_number(vm)?
    };

    // 6. Set isLittleEndian to ToBoolean(isLittleEndian).
    let little_endian = is_little_endian.to_boolean();

    // 7. Let viewOffset be view.[[ByteOffset]].
    let view_offset = view.byte_offset();

    // 8. Let viewRecord be MakeDataViewWithBufferWitnessRecord(view, unordered).
    let view_record = make_data_view_with_buffer_witness_record(view, Order::Unordered);

    // 9. NOTE: Bounds checking is not a synchronizing operation when view's backing buffer is a growable SharedArrayBuffer.
    // 10. If IsViewOutOfBounds(viewRecord) is true, throw a TypeError exception.
    if is_view_out_of_bounds(&view_record) {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::BufferOutOfBounds, &[&"DataView"]);
    }

    // 11. Let viewSize be GetViewByteLength(viewRecord).
    let view_size = get_view_byte_length(&view_record);

    // 12. Let elementSize be the Element Size value specified in Table 71 for Element Type type.
    let element_size = element_type.size();

    // 13. If getIndex + elementSize > viewSize, throw a RangeError exception.
    let end_index = get_index.checked_add(element_size);

    if end_index.is_none_or(|end_index| end_index > view_size as usize) {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::DataViewOutOfRangeByteOffset,
            &[&get_index, &view_size],
        );
    }

    // 14. Let bufferIndex be getIndex + viewOffset.
    let Some(buffer_index) = get_index.checked_add(view_offset as usize) else {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::DataViewOutOfRangeByteOffset,
            &[&get_index, &view_size],
        );
    };

    // 15. Perform SetValueInBuffer(view.[[ViewedArrayBuffer]], bufferIndex, type, numberValue, false, unordered, isLittleEndian).
    view.viewed_array_buffer().set_value(
        vm,
        buffer_index,
        element_type,
        number_value,
        false,
        Order::Unordered,
        little_endian,
    );

    // 16. Return undefined.
    Ok(Value::UNDEFINED)
}

impl DataViewPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<DataViewPrototype> {
        realm.create_object(
            vm,
            DataViewPrototype {
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
        let define_function = |property_key: &PropertyKey, function, length| {
            object.define_native_function(vm, realm, property_key, function, length, attr, None);
        };

        define_function(&names.getBigInt64, raw_native!(DataViewPrototype::get_big_int_64), 1);
        define_function(&names.getBigUint64, raw_native!(DataViewPrototype::get_big_uint_64), 1);
        define_function(&names.getFloat16, raw_native!(DataViewPrototype::get_float_16), 1);
        define_function(&names.getFloat32, raw_native!(DataViewPrototype::get_float_32), 1);
        define_function(&names.getFloat64, raw_native!(DataViewPrototype::get_float_64), 1);
        define_function(&names.getInt8, raw_native!(DataViewPrototype::get_int_8), 1);
        define_function(&names.getInt16, raw_native!(DataViewPrototype::get_int_16), 1);
        define_function(&names.getInt32, raw_native!(DataViewPrototype::get_int_32), 1);
        define_function(&names.getUint8, raw_native!(DataViewPrototype::get_uint_8), 1);
        define_function(&names.getUint16, raw_native!(DataViewPrototype::get_uint_16), 1);
        define_function(&names.getUint32, raw_native!(DataViewPrototype::get_uint_32), 1);
        define_function(&names.setBigInt64, raw_native!(DataViewPrototype::set_big_int_64), 2);
        define_function(&names.setBigUint64, raw_native!(DataViewPrototype::set_big_uint_64), 2);
        define_function(&names.setFloat16, raw_native!(DataViewPrototype::set_float_16), 2);
        define_function(&names.setFloat32, raw_native!(DataViewPrototype::set_float_32), 2);
        define_function(&names.setFloat64, raw_native!(DataViewPrototype::set_float_64), 2);
        define_function(&names.setInt8, raw_native!(DataViewPrototype::set_int_8), 2);
        define_function(&names.setInt16, raw_native!(DataViewPrototype::set_int_16), 2);
        define_function(&names.setInt32, raw_native!(DataViewPrototype::set_int_32), 2);
        define_function(&names.setUint8, raw_native!(DataViewPrototype::set_uint_8), 2);
        define_function(&names.setUint16, raw_native!(DataViewPrototype::set_uint_16), 2);
        define_function(&names.setUint32, raw_native!(DataViewPrototype::set_uint_32), 2);

        let configurable = PropertyAttributes::new(Attribute::CONFIGURABLE);
        let define_accessor = |property_key: &PropertyKey, getter| {
            object.define_native_accessor(vm, realm, property_key, getter, None, configurable);
        };
        define_accessor(&names.buffer, raw_native!(DataViewPrototype::buffer_getter));
        define_accessor(&names.byteLength, raw_native!(DataViewPrototype::byte_length_getter));
        define_accessor(&names.byteOffset, raw_native!(DataViewPrototype::byte_offset_getter));

        // 25.3.4.27 DataView.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-dataview.prototype-@@tostringtag
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_fly_string(vm, names.DataView.as_string())),
            configurable,
        );
    }

    // 25.3.4.1 get DataView.prototype.buffer, https://tc39.es/ecma262/#sec-get-dataview.prototype.buffer
    fn buffer_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be the this value.
        // 2. Perform ? RequireInternalSlot(O, [[DataView]]).
        // 3. Assert: O has a [[ViewedArrayBuffer]] internal slot.
        let data_view = this_data_view(vm)?;

        // 4. Let buffer be O.[[ViewedArrayBuffer]].
        // 5. Return buffer.
        Ok(Value::from_object(data_view.viewed_array_buffer()))
    }

    // 25.3.4.2 get DataView.prototype.byteLength, https://tc39.es/ecma262/#sec-get-dataview.prototype.bytelength
    fn byte_length_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be the this value.
        // 2. Perform ? RequireInternalSlot(O, [[DataView]]).
        // 3. Assert: O has a [[ViewedArrayBuffer]] internal slot.
        let data_view = this_data_view(vm)?;

        // 4. Let viewRecord be MakeDataViewWithBufferWitnessRecord(O, seq-cst).
        let view_record = make_data_view_with_buffer_witness_record(data_view, Order::SeqCst);

        // 5. If IsViewOutOfBounds(viewRecord) is true, throw a TypeError exception.
        if is_view_out_of_bounds(&view_record) {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::BufferOutOfBounds, &[&"DataView"]);
        }

        // 6. Let size be GetViewByteLength(viewRecord).
        let size = get_view_byte_length(&view_record);

        // 7. Return 𝔽(size).
        Ok(Value::from_f64(f64::from(size)))
    }

    // 25.3.4.3 get DataView.prototype.byteOffset, https://tc39.es/ecma262/#sec-get-dataview.prototype.byteoffset
    fn byte_offset_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be the this value.
        // 2. Perform ? RequireInternalSlot(O, [[DataView]]).
        // 3. Assert: O has a [[ViewedArrayBuffer]] internal slot.
        let data_view = this_data_view(vm)?;

        // 4. Let viewRecord be MakeDataViewWithBufferWitnessRecord(O, seq-cst).
        let view_record = make_data_view_with_buffer_witness_record(data_view, Order::SeqCst);

        // 5. If IsViewOutOfBounds(viewRecord) is true, throw a TypeError exception.
        if is_view_out_of_bounds(&view_record) {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::BufferOutOfBounds, &[&"DataView"]);
        }

        // 6. Let offset be O.[[ByteOffset]].
        let offset = data_view.byte_offset();

        // 7. Return 𝔽(offset).
        Ok(Value::from_f64(f64::from(offset)))
    }

    // 25.3.4.5 DataView.prototype.getBigInt64 ( byteOffset [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.getbigint64
    fn get_big_int_64(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let v be the this value.
        // 2. Return ? GetViewValue(v, byteOffset, littleEndian, BigInt64).
        get_view_value(vm, vm.argument(0), vm.argument(1), ElementType::BigInt64)
    }

    // 25.3.4.6 DataView.prototype.getBigUint64 ( byteOffset [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.getbiguint64
    fn get_big_uint_64(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let v be the this value.
        // 2. Return ? GetViewValue(v, byteOffset, littleEndian, BigUint64).
        get_view_value(vm, vm.argument(0), vm.argument(1), ElementType::BigUint64)
    }

    // 25.3.4.7 DataView.prototype.getFloat16 ( byteOffset [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.getfloat16
    fn get_float_16(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let v be the this value.
        // 2. If littleEndian is not present, set littleEndian to false.
        // 3. Return ? GetViewValue(v, byteOffset, littleEndian, Float16).
        get_view_value(vm, vm.argument(0), vm.argument(1), ElementType::Float16)
    }

    // 25.3.4.8 DataView.prototype.getFloat32 ( byteOffset [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.getfloat32
    fn get_float_32(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let v be the this value.
        // 2. If littleEndian is not present, set littleEndian to false.
        // 3. Return ? GetViewValue(v, byteOffset, littleEndian, Float32).
        get_view_value(vm, vm.argument(0), vm.argument(1), ElementType::Float32)
    }

    // 25.3.4.9 DataView.prototype.getFloat64 ( byteOffset [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.getfloat64
    fn get_float_64(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let v be the this value.
        // 2. If littleEndian is not present, set littleEndian to false.
        // 3. Return ? GetViewValue(v, byteOffset, littleEndian, Float64).
        get_view_value(vm, vm.argument(0), vm.argument(1), ElementType::Float64)
    }

    // 25.3.4.10 DataView.prototype.getInt8 ( byteOffset ), https://tc39.es/ecma262/#sec-dataview.prototype.getint8
    fn get_int_8(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let v be the this value.
        // 2. Return ? GetViewValue(v, byteOffset, true, Int8).
        get_view_value(vm, vm.argument(0), Value::TRUE, ElementType::Int8)
    }

    // 25.3.4.11 DataView.prototype.getInt16 ( byteOffset [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.getint16
    fn get_int_16(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let v be the this value.
        // 2. If littleEndian is not present, set littleEndian to false.
        // 3. Return ? GetViewValue(v, byteOffset, littleEndian, Int16).
        get_view_value(vm, vm.argument(0), vm.argument(1), ElementType::Int16)
    }

    // 25.3.4.12 DataView.prototype.getInt32 ( byteOffset [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.getint32
    fn get_int_32(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let v be the this value.
        // 2. If littleEndian is not present, set littleEndian to false.
        // 3. Return ? GetViewValue(v, byteOffset, littleEndian, Int32).
        get_view_value(vm, vm.argument(0), vm.argument(1), ElementType::Int32)
    }

    // 25.3.4.13 DataView.prototype.getUint8 ( byteOffset ), https://tc39.es/ecma262/#sec-dataview.prototype.getuint8
    fn get_uint_8(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let v be the this value.
        // 2. Return ? GetViewValue(v, byteOffset, true, Uint8).
        get_view_value(vm, vm.argument(0), Value::TRUE, ElementType::Uint8)
    }

    // 25.3.4.14 DataView.prototype.getUint16 ( byteOffset [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.getuint16
    fn get_uint_16(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let v be the this value.
        // 2. If littleEndian is not present, set littleEndian to false.
        // 3. Return ? GetViewValue(v, byteOffset, littleEndian, Uint16).
        get_view_value(vm, vm.argument(0), vm.argument(1), ElementType::Uint16)
    }

    // 25.3.4.15 DataView.prototype.getUint32 ( byteOffset [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.getuint32
    fn get_uint_32(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let v be the this value.
        // 2. If littleEndian is not present, set littleEndian to false.
        // 3. Return ? GetViewValue(v, byteOffset, littleEndian, Uint32).
        get_view_value(vm, vm.argument(0), vm.argument(1), ElementType::Uint32)
    }

    // 25.3.4.16 DataView.prototype.setBigInt64 ( byteOffset, value [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.setbigint64
    fn set_big_int_64(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let v be the this value.
        // 2. Return ? SetViewValue(v, byteOffset, littleEndian, BigInt64, value).
        set_view_value(
            vm,
            vm.argument(0),
            vm.argument(2),
            ElementType::BigInt64,
            vm.argument(1),
        )
    }

    // 25.3.4.17 DataView.prototype.setBigUint64 ( byteOffset, value [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.setbiguint64
    fn set_big_uint_64(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let v be the this value.
        // 2. Return ? SetViewValue(v, byteOffset, littleEndian, BigUint64, value).
        set_view_value(
            vm,
            vm.argument(0),
            vm.argument(2),
            ElementType::BigUint64,
            vm.argument(1),
        )
    }

    // 25.3.4.18 DataView.prototype.setFloat16 ( byteOffset, value [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.setfloat16
    fn set_float_16(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let v be the this value.
        // 2. If littleEndian is not present, set littleEndian to false.
        // 3. Return ? SetViewValue(v, byteOffset, littleEndian, Float16, value).
        set_view_value(vm, vm.argument(0), vm.argument(2), ElementType::Float16, vm.argument(1))
    }

    // 25.3.4.19 DataView.prototype.setFloat32 ( byteOffset, value [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.setfloat32
    fn set_float_32(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let v be the this value.
        // 2. If littleEndian is not present, set littleEndian to false.
        // 3. Return ? SetViewValue(v, byteOffset, littleEndian, Float32, value).
        set_view_value(vm, vm.argument(0), vm.argument(2), ElementType::Float32, vm.argument(1))
    }

    // 25.3.4.20 DataView.prototype.setFloat64 ( byteOffset, value [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.setfloat64
    fn set_float_64(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let v be the this value.
        // 2. If littleEndian is not present, set littleEndian to false.
        // 3. Return ? SetViewValue(v, byteOffset, littleEndian, Float64, value).
        set_view_value(vm, vm.argument(0), vm.argument(2), ElementType::Float64, vm.argument(1))
    }

    // 25.3.4.21 DataView.prototype.setInt8 ( byteOffset, value ), https://tc39.es/ecma262/#sec-dataview.prototype.setint8
    fn set_int_8(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let v be the this value.
        // 2. Return ? SetViewValue(v, byteOffset, true, Int8, value).
        set_view_value(vm, vm.argument(0), Value::TRUE, ElementType::Int8, vm.argument(1))
    }

    // 25.3.4.22 DataView.prototype.setInt16 ( byteOffset, value [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.setint16
    fn set_int_16(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let v be the this value.
        // 2. If littleEndian is not present, set littleEndian to false.
        // 3. Return ? SetViewValue(v, byteOffset, littleEndian, Int16, value).
        set_view_value(vm, vm.argument(0), vm.argument(2), ElementType::Int16, vm.argument(1))
    }

    // 25.3.4.23 DataView.prototype.setInt32 ( byteOffset, value [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.setint32
    fn set_int_32(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let v be the this value.
        // 2. If littleEndian is not present, set littleEndian to false.
        // 3. Return ? SetViewValue(v, byteOffset, littleEndian, Int32, value).
        set_view_value(vm, vm.argument(0), vm.argument(2), ElementType::Int32, vm.argument(1))
    }

    // 25.3.4.24 DataView.prototype.setUint8 ( byteOffset, value ), https://tc39.es/ecma262/#sec-dataview.prototype.setuint8
    fn set_uint_8(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let v be the this value.
        // 2. Return ? SetViewValue(v, byteOffset, true, Uint8, value).
        set_view_value(vm, vm.argument(0), Value::TRUE, ElementType::Uint8, vm.argument(1))
    }

    // 25.3.4.25 DataView.prototype.setUint16 ( byteOffset, value [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.setuint16
    fn set_uint_16(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let v be the this value.
        // 2. If littleEndian is not present, set littleEndian to false.
        // 3. Return ? SetViewValue(v, byteOffset, littleEndian, Uint16, value).
        set_view_value(vm, vm.argument(0), vm.argument(2), ElementType::Uint16, vm.argument(1))
    }

    // 25.3.4.26 DataView.prototype.setUint32 ( byteOffset, value [ , littleEndian ] ), https://tc39.es/ecma262/#sec-dataview.prototype.setuint32
    fn set_uint_32(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let v be the this value.
        // 2. If littleEndian is not present, set littleEndian to false.
        // 3. Return ? SetViewValue(v, byteOffset, littleEndian, Uint32, value).
        set_view_value(vm, vm.argument(0), vm.argument(2), ElementType::Uint32, vm.argument(1))
    }
}
