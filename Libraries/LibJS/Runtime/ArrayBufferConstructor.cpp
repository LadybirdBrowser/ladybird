/*
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/ArrayBufferConstructor.h>
#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/TypedArray.h>

namespace JS {

JS_DEFINE_ALLOCATOR(ArrayBufferConstructor);

ArrayBufferConstructor::ArrayBufferConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.ArrayBuffer.as_string(), realm.intrinsics().function_prototype())
{
}

void ArrayBufferConstructor::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    // 25.1.5.2 ArrayBuffer.prototype, https://tc39.es/ecma262/#sec-arraybuffer.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().array_buffer_prototype(), 0);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.isView, is_view, 1, attr);

    // 25.1.6.7 ArrayBuffer.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-arraybuffer.prototype-@@tostringtag
    define_native_accessor(realm, vm.well_known_symbol_species(), symbol_species_getter, {}, Attribute::Configurable);

    define_direct_property(vm.names.length, Value(1), Attribute::Configurable);
}

// 25.1.4.1 ArrayBuffer ( length [ , options ] ), https://tc39.es/ecma262/#sec-arraybuffer-length
ThrowCompletionOr<Value> ArrayBufferConstructor::call()
{
    auto& vm = this->vm();

    // 1. If NewTarget is undefined, throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, vm.names.ArrayBuffer);
}

// 25.1.4.1 ArrayBuffer ( length [ , options ] ), https://tc39.es/ecma262/#sec-arraybuffer-length
ThrowCompletionOr<NonnullGCPtr<Object>> ArrayBufferConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    auto length = vm.argument(0);
    auto options = vm.argument(1);

    // 2. Let byteLength be ? ToIndex(length).
    auto byte_length_or_error = length.to_index(vm);

    if (byte_length_or_error.is_error()) {
        auto error = byte_length_or_error.release_error();
        if (error.value()->is_object() && is<RangeError>(error.value()->as_object())) {
            // Re-throw more specific RangeError
            return vm.throw_completion<RangeError>(ErrorType::InvalidLength, "array buffer");
        }
        return error;
    }

    // 3. Let requestedMaxByteLength be ? GetArrayBufferMaxByteLengthOption(options).
    auto requested_max_byte_length = TRY(get_array_buffer_max_byte_length_option(vm, options));

    // 3. Return ? AllocateArrayBuffer(NewTarget, byteLength, requestedMaxByteLength).
    return *TRY(allocate_array_buffer(vm, new_target, byte_length_or_error.release_value(), requested_max_byte_length));
}

// 25.1.5.1 ArrayBuffer.isView ( arg ), https://tc39.es/ecma262/#sec-arraybuffer.isview
JS_DEFINE_NATIVE_FUNCTION(ArrayBufferConstructor::is_view)
{
    auto arg = vm.argument(0);

    // 1. If arg is not an Object, return false.
    if (!arg.is_object())
        return Value(false);

    // 2. If arg has a [[ViewedArrayBuffer]] internal slot, return true.
    if (arg.as_object().is_typed_array())
        return Value(true);
    if (is<DataView>(arg.as_object()))
        return Value(true);

    // 3. Return false.
    return Value(false);
}

// 25.1.5.3 get ArrayBuffer [ @@species ], https://tc39.es/ecma262/#sec-get-arraybuffer-@@species
JS_DEFINE_NATIVE_FUNCTION(ArrayBufferConstructor::symbol_species_getter)
{
    // 1. Return the this value.
    return vm.this_value();
}

}
