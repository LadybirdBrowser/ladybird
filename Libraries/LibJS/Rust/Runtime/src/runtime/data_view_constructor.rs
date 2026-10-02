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
use crate::runtime::abstract_operations::ordinary_create_from_constructor_of;
use crate::runtime::array_buffer::{ArrayBuffer, Order, array_buffer_byte_length};
use crate::runtime::byte_length::ByteLength;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::data_view::DataView;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::native_function::{NativeFunction, define_native_function_class};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;

#[repr(C)]
#[derive(Trace)]
pub struct DataViewConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    DataViewConstructor,
    initialize: DataViewConstructor::initialize,
    call: DataViewConstructor::call,
    construct: DataViewConstructor::construct
);

impl DataViewConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<DataViewConstructor> {
        realm.create_object(
            vm,
            DataViewConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.DataView.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 25.3.3.1 DataView.prototype, https://tc39.es/ecma262/#sec-dataview.prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.intrinsics().data_view_prototype(vm)),
            PropertyAttributes::new(0),
        );

        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_i32(1),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 25.3.2.1 DataView ( buffer [ , byteOffset [ , byteLength ] ] ), https://tc39.es/ecma262/#sec-dataview-buffer-byteoffset-bytelength
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, throw a TypeError exception.
        vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::ConstructorWithoutNew,
            &[&vm.names.DataView],
        )
    }

    // 25.3.2.1 DataView ( buffer [ , byteOffset [ , byteLength ] ] ), https://tc39.es/ecma262/#sec-dataview-buffer-byteoffset-bytelength
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let realm = vm.current_realm().expect("a constructor runs in a realm");

        let buffer_value = vm.argument(0);
        let byte_offset = vm.argument(1);
        let byte_length = vm.argument(2);

        // 2. Perform ? RequireInternalSlot(buffer, [[ArrayBufferData]]).
        let buffer = buffer_value
            .is_object()
            .then(|| buffer_value.as_object().downcast::<ArrayBuffer>())
            .flatten();
        let Some(buffer) = buffer else {
            // NB: Like C++, this formats the null ArrayBuffer pointer it did not find.
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::IsNotAn,
                &[&"0x0000000000000000", &vm.names.ArrayBuffer],
            );
        };

        // 3. Let offset be ? ToIndex(byteOffset).
        let offset = byte_offset.to_index(vm)? as usize;

        // 4. If IsDetachedBuffer(buffer) is true, throw a TypeError exception.
        if buffer.is_detached() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::DetachedArrayBuffer, &[]);
        }

        // 5. Let bufferByteLength be ArrayBufferByteLength(buffer, seq-cst).
        let mut buffer_byte_length = array_buffer_byte_length(&buffer, Order::SeqCst);

        // 6. If offset > bufferByteLength, throw a RangeError exception.
        if offset > buffer_byte_length {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::DataViewOutOfRangeByteOffset,
                &[&offset, &buffer_byte_length],
            );
        }

        // 7. Let bufferIsFixedLength be IsFixedLengthArrayBuffer(buffer).
        let buffer_is_fixed_length = buffer.is_fixed_length();

        let view_byte_length: ByteLength;

        // 8. If byteLength is undefined, then
        if byte_length.is_undefined() {
            // a. If bufferIsFixedLength is true, then
            if buffer_is_fixed_length {
                // i. Let viewByteLength be bufferByteLength - offset.
                // NB: Like C++, the length is truncated to a u32.
                view_byte_length = ByteLength::Length((buffer_byte_length - offset) as u32);
            }
            // b. Else,
            else {
                // i. Let viewByteLength be auto.
                view_byte_length = ByteLength::auto_();
            }
        }
        // 9. Else,
        else {
            // a. Let viewByteLength be ? ToIndex(byteLength).
            // NB: Like C++, the length is truncated to a u32.
            view_byte_length = ByteLength::Length(byte_length.to_index(vm)? as u32);

            // b. If offset + viewByteLength > bufferByteLength, throw a RangeError exception.
            let checked_add = offset.checked_add(view_byte_length.length() as usize);

            if checked_add.is_none_or(|end| end > buffer_byte_length) {
                return vm.throw_completion(ErrorKind::RangeError, ErrorType::InvalidLength, &[&vm.names.DataView]);
            }
        }

        // 10. Let O be ? OrdinaryCreateFromConstructor(NewTarget, "%DataView.prototype%", « [[DataView]], [[ViewedArrayBuffer]], [[ByteLength]], [[ByteOffset]] »).
        let data_view =
            ordinary_create_from_constructor_of(vm, realm, new_target, Intrinsics::data_view_prototype, |prototype| {
                DataView::new(vm, buffer, view_byte_length, offset, prototype)
            })?;

        // 11. If IsDetachedBuffer(buffer) is true, throw a TypeError exception.
        if buffer.is_detached() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::DetachedArrayBuffer, &[]);
        }

        // 12. Set bufferByteLength to ArrayBufferByteLength(buffer, seq-cst).
        buffer_byte_length = array_buffer_byte_length(&buffer, Order::SeqCst);

        // 13. If offset > bufferByteLength, throw a RangeError exception.
        if offset > buffer_byte_length {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::DataViewOutOfRangeByteOffset,
                &[&offset, &buffer_byte_length],
            );
        }

        // 14. If byteLength is not undefined, then
        if !byte_length.is_undefined() {
            // a. If offset + viewByteLength > bufferByteLength, throw a RangeError exception.
            let checked_add = offset.checked_add(view_byte_length.length() as usize);

            if checked_add.is_none_or(|end| end > buffer_byte_length) {
                return vm.throw_completion(ErrorKind::RangeError, ErrorType::InvalidLength, &[&vm.names.DataView]);
            }
        }

        // 15. Set O.[[ViewedArrayBuffer]] to buffer.
        // 16. Set O.[[ByteLength]] to viewByteLength.
        // 17. Set O.[[ByteOffset]] to offset.

        // 18. Return O.
        Ok(data_view.upcast())
    }
}
