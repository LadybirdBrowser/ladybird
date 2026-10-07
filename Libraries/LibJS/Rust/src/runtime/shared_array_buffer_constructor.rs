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
use crate::runtime::array_buffer::{allocate_shared_array_buffer, get_array_buffer_max_byte_length_option};
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::{ErrorKind, RangeError};
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::native_function::{NativeFunction, define_native_function_class, raw_native};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;

#[repr(C)]
#[derive(Trace)]
pub struct SharedArrayBufferConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    SharedArrayBufferConstructor,
    initialize: SharedArrayBufferConstructor::initialize,
    call: SharedArrayBufferConstructor::call,
    construct: SharedArrayBufferConstructor::construct
);

impl SharedArrayBufferConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<SharedArrayBufferConstructor> {
        realm.create_object(
            vm,
            SharedArrayBufferConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.SharedArrayBuffer.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 25.2.4.1 SharedArrayBuffer.prototype, https://tc39.es/ecma262/#sec-sharedarraybuffer.prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.intrinsics().shared_array_buffer_prototype(vm)),
            PropertyAttributes::new(0),
        );

        // 25.2.5.7 SharedArrayBuffer.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-sharedarraybuffer.prototype.toString
        object.define_native_accessor(
            vm,
            realm,
            &PropertyKey::from(vm.well_known_symbols().species),
            raw_native!(SharedArrayBufferConstructor::symbol_species_getter),
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

    // 25.2.3.1 SharedArrayBuffer ( length [ , options ] ), https://tc39.es/ecma262/#sec-sharedarraybuffer-length
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, throw a TypeError exception.
        vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::ConstructorWithoutNew,
            &[&vm.names.SharedArrayBuffer],
        )
    }

    // 25.2.3.1 SharedArrayBuffer ( length [ , options ] ), https://tc39.es/ecma262/#sec-sharedarraybuffer-length
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let length = vm.argument(0);
        let options = vm.argument(1);

        // 2. Let byteLength be ? ToIndex(length).
        let byte_length = match length.to_index(vm) {
            Ok(byte_length) => byte_length,
            Err(error) => {
                if error.value().is_object() && error.value().as_object().is::<RangeError>() {
                    // Re-throw more specific RangeError
                    return vm.throw_completion(
                        ErrorKind::RangeError,
                        ErrorType::InvalidLength,
                        &[&"shared array buffer"],
                    );
                }
                return Err(error);
            }
        };

        // 3. Let requestedMaxByteLength be ? GetArrayBufferMaxByteLengthOption(options).
        let requested_max_byte_length = get_array_buffer_max_byte_length_option(vm, options)?;

        // 4. Return ? AllocateSharedArrayBuffer(NewTarget, byteLength, requestedMaxByteLength).
        Ok(allocate_shared_array_buffer(vm, new_target, byte_length as usize, requested_max_byte_length)?.upcast())
    }

    // 25.2.4.2 get SharedArrayBuffer [ @@species ], https://tc39.es/ecma262/#sec-sharedarraybuffer-@@species
    #[allow(clippy::unnecessary_wraps, reason = "native functions return a completion")]
    fn symbol_species_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Return the this value.
        Ok(vm.this_value())
    }
}
