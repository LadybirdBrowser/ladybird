/*
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/SharedArrayBufferConstructor.h>
#include <LibJS/Runtime/TypedArray.h>

namespace JS {

GC_DEFINE_ALLOCATOR(SharedArrayBufferConstructor);

SharedArrayBufferConstructor::SharedArrayBufferConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.SharedArrayBuffer.as_string(), realm.intrinsics().function_prototype())
{
}

void SharedArrayBufferConstructor::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    // 25.2.4.1 SharedArrayBuffer.prototype, https://tc39.es/ecma262/#sec-sharedarraybuffer.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().shared_array_buffer_prototype(), 0);

    // 25.2.5.7 SharedArrayBuffer.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-sharedarraybuffer.prototype.toString
    define_native_accessor(realm, vm.well_known_symbol_species(), symbol_species_getter, {}, Attribute::Configurable);

    define_direct_property(vm.names.length, Value(1), Attribute::Configurable);
}

// 25.2.3.1 SharedArrayBuffer ( length [ , options ] ), https://tc39.es/ecma262/#sec-sharedarraybuffer-length
ThrowCompletionOr<Value> SharedArrayBufferConstructor::call()
{
    auto& vm = this->vm();

    // 1. If NewTarget is undefined, throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, vm.names.SharedArrayBuffer);
}

// 25.2.3.1 SharedArrayBuffer ( length [ , options ] ), https://tc39.es/ecma262/#sec-sharedarraybuffer-length
ThrowCompletionOr<GC::Ref<Object>> SharedArrayBufferConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    auto length = vm.argument(0);
    auto options = vm.argument(1);

    // 2. Let byteLength be ? ToIndex(length).
    auto byte_length_or_error = length.to_index(vm);

    if (byte_length_or_error.is_error()) {
        auto error = byte_length_or_error.release_error();
        if (error.value().is<RangeError>()) {
            // Re-throw more specific RangeError
            return vm.throw_completion<RangeError>(ErrorType::InvalidLength, "shared array buffer");
        }
        return error;
    }

    // 3. Let requestedMaxByteLength be ? GetArrayBufferMaxByteLengthOption(options).
    auto requested_max_byte_length = TRY(get_array_buffer_max_byte_length_option(vm, options));

    // 4. Return ? AllocateSharedArrayBuffer(NewTarget, byteLength, requestedMaxByteLength).
    return *TRY(allocate_shared_array_buffer(vm, new_target, byte_length_or_error.release_value(), requested_max_byte_length));
}

// 25.2.4.2 get SharedArrayBuffer [ @@species ], https://tc39.es/ecma262/#sec-sharedarraybuffer-@@species
JS_DEFINE_NATIVE_FUNCTION(SharedArrayBufferConstructor::symbol_species_getter)
{
    // 1. Return the this value.
    return vm.this_value();
}

}
