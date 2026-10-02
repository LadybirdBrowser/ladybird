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
use crate::runtime::array_buffer::{allocate_array_buffer, get_array_buffer_max_byte_length_option};
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::data_view::DataView;
use crate::runtime::error::{ErrorKind, RangeError};
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::native_function::{NativeFunction, define_native_function_class, raw_native};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;

#[repr(C)]
#[derive(Trace)]
pub struct ArrayBufferConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    ArrayBufferConstructor,
    initialize: ArrayBufferConstructor::initialize,
    call: ArrayBufferConstructor::call,
    construct: ArrayBufferConstructor::construct
);

impl ArrayBufferConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<ArrayBufferConstructor> {
        realm.create_object(
            vm,
            ArrayBufferConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.ArrayBuffer.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 25.1.5.2 ArrayBuffer.prototype, https://tc39.es/ecma262/#sec-arraybuffer.prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.intrinsics().array_buffer_prototype(vm)),
            PropertyAttributes::new(0),
        );

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &vm.names.isView,
            raw_native!(ArrayBufferConstructor::is_view),
            1,
            attr,
            None,
        );

        // 25.1.6.7 ArrayBuffer.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-arraybuffer.prototype-@@tostringtag
        object.define_native_accessor(
            vm,
            realm,
            &PropertyKey::from(vm.well_known_symbols().species),
            raw_native!(ArrayBufferConstructor::symbol_species_getter),
            None,
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_i32(1),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 25.1.4.1 ArrayBuffer ( length [ , options ] ), https://tc39.es/ecma262/#sec-arraybuffer-length
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, throw a TypeError exception.
        vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::ConstructorWithoutNew,
            &[&vm.names.ArrayBuffer],
        )
    }

    // 25.1.4.1 ArrayBuffer ( length [ , options ] ), https://tc39.es/ecma262/#sec-arraybuffer-length
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let length = vm.argument(0);
        let options = vm.argument(1);

        // 2. Let byteLength be ? ToIndex(length).
        let byte_length = match length.to_index(vm) {
            Ok(byte_length) => byte_length,
            Err(error) => {
                if error.value().is_object() && error.value().as_object().is::<RangeError>() {
                    // Re-throw more specific RangeError
                    return vm.throw_completion(ErrorKind::RangeError, ErrorType::InvalidLength, &[&"array buffer"]);
                }
                return Err(error);
            }
        };

        // 3. Let requestedMaxByteLength be ? GetArrayBufferMaxByteLengthOption(options).
        let requested_max_byte_length = get_array_buffer_max_byte_length_option(vm, options)?;

        // 3. Return ? AllocateArrayBuffer(NewTarget, byteLength, requestedMaxByteLength).
        Ok(allocate_array_buffer(vm, new_target, byte_length as usize, requested_max_byte_length)?.upcast())
    }

    // 25.1.5.1 ArrayBuffer.isView ( arg ), https://tc39.es/ecma262/#sec-arraybuffer.isview
    #[allow(clippy::unnecessary_wraps, reason = "native functions return a completion")]
    fn is_view(vm: &Vm) -> ThrowCompletionOr<Value> {
        let arg = vm.argument(0);

        // 1. If arg is not an Object, return false.
        if !arg.is_object() {
            return Ok(Value::FALSE);
        }
        let object = arg.as_object();

        // 2. If arg has a [[ViewedArrayBuffer]] internal slot, return true.
        if object.is_typed_array() {
            return Ok(Value::TRUE);
        }
        if object.is::<DataView>() {
            return Ok(Value::TRUE);
        }

        // 3. Return false.
        Ok(Value::FALSE)
    }

    // 25.1.5.3 get ArrayBuffer [ @@species ], https://tc39.es/ecma262/#sec-get-arraybuffer-@@species
    #[allow(clippy::unnecessary_wraps, reason = "native functions return a completion")]
    fn symbol_species_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Return the this value.
        Ok(vm.this_value())
    }
}
