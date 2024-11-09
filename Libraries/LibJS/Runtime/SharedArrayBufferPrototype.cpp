/*
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Function.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/SharedArrayBufferConstructor.h>
#include <LibJS/Runtime/SharedArrayBufferPrototype.h>

namespace JS {

JS_DEFINE_ALLOCATOR(SharedArrayBufferPrototype);

SharedArrayBufferPrototype::SharedArrayBufferPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void SharedArrayBufferPrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();

    Base::initialize(realm);
    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_accessor(realm, vm.names.byteLength, byte_length_getter, {}, Attribute::Configurable);
    define_native_function(realm, vm.names.slice, slice, 2, attr);

    // 25.2.5.7 SharedArrayBuffer.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-sharedarraybuffer.prototype.toString
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, vm.names.SharedArrayBuffer.as_string()), Attribute::Configurable);
}

// 25.2.5.1 get SharedArrayBuffer.prototype.byteLength, https://tc39.es/ecma262/#sec-get-sharedarraybuffer.prototype.bytelength
JS_DEFINE_NATIVE_FUNCTION(SharedArrayBufferPrototype::byte_length_getter)
{
    // 1. Let O be the this value.
    // 2. Perform ? RequireInternalSlot(O, [[ArrayBufferData]]).
    auto array_buffer_object = TRY(typed_this_value(vm));

    // 3. If IsSharedArrayBuffer(O) is false, throw a TypeError exception.
    if (!array_buffer_object->is_shared_array_buffer())
        return vm.throw_completion<TypeError>(ErrorType::NotASharedArrayBuffer);

    // 4. Let length be O.[[ArrayBufferByteLength]].
    // 5. Return 𝔽(length).
    return Value(array_buffer_object->byte_length());
}

// 25.2.5.6 SharedArrayBuffer.prototype.slice ( start, end ), https://tc39.es/ecma262/#sec-sharedarraybuffer.prototype.slice
JS_DEFINE_NATIVE_FUNCTION(SharedArrayBufferPrototype::slice)
{
    auto& realm = *vm.current_realm();

    auto start = vm.argument(0);
    auto end = vm.argument(1);

    // 1. Let O be the this value.
    // 2. Perform ? RequireInternalSlot(O, [[ArrayBufferData]]).
    auto array_buffer_object = TRY(typed_this_value(vm));

    // 3. If IsSharedArrayBuffer(O) is false, throw a TypeError exception.
    if (!array_buffer_object->is_shared_array_buffer())
        return vm.throw_completion<TypeError>(ErrorType::NotASharedArrayBuffer);

    // 4. Let len be O.[[ArrayBufferByteLength]].
    auto length = array_buffer_object->byte_length();

    // 5. Let relativeStart be ? ToIntegerOrInfinity(start).
    auto relative_start = TRY(start.to_integer_or_infinity(vm));

    double first;
    // 6. If relativeStart is -∞, let first be 0.
    if (Value(relative_start).is_negative_infinity())
        first = 0;
    // 7. Else if relativeStart < 0, let first be max(len + relativeStart, 0).
    else if (relative_start < 0)
        first = max(length + relative_start, 0.0);
    // 8. Else, let first be min(relativeStart, len).
    else
        first = min(relative_start, (double)length);

    // 9. If end is undefined, let relativeEnd be len; else let relativeEnd be ? ToIntegerOrInfinity(end).
    auto relative_end = end.is_undefined() ? length : TRY(end.to_integer_or_infinity(vm));

    double final;
    // 10. If relativeEnd is -∞, let final be 0.
    if (Value(relative_end).is_negative_infinity())
        final = 0;
    // 11. Else if relativeEnd < 0, let final be max(len + relativeEnd, 0).
    else if (relative_end < 0)
        final = max(length + relative_end, 0.0);
    // 12. Else, let final be min(relativeEnd, len).
    else
        final = min(relative_end, (double)length);

    // 13. Let newLen be max(final - first, 0).
    auto new_length = max(final - first, 0.0);

    // 14. Let ctor be ? SpeciesConstructor(O, %SharedArrayBuffer%).
    auto* constructor = TRY(species_constructor(vm, array_buffer_object, realm.intrinsics().shared_array_buffer_constructor()));

    // 15. Let new be ? Construct(ctor, « 𝔽(newLen) »).
    auto new_array_buffer = TRY(construct(vm, *constructor, Value(new_length)));

    // 16. Perform ? RequireInternalSlot(new, [[ArrayBufferData]]).
    if (!is<ArrayBuffer>(new_array_buffer.ptr()))
        return vm.throw_completion<TypeError>(ErrorType::SpeciesConstructorDidNotCreate, "an ArrayBuffer");
    auto* new_array_buffer_object = static_cast<ArrayBuffer*>(new_array_buffer.ptr());

    // 17. If IsSharedArrayBuffer(new) is true, throw a TypeError exception.
    if (!new_array_buffer_object->is_shared_array_buffer())
        return vm.throw_completion<TypeError>(ErrorType::NotASharedArrayBuffer);

    // 18. If new.[[ArrayBufferData]] is O.[[ArrayBufferData]], throw a TypeError exception.
    if (new_array_buffer == array_buffer_object)
        return vm.throw_completion<TypeError>(ErrorType::SpeciesConstructorReturned, "same ArrayBuffer instance");

    // 19. If new.[[ArrayBufferByteLength]] < newLen, throw a TypeError exception.
    if (new_array_buffer_object->byte_length() < new_length)
        return vm.throw_completion<TypeError>(ErrorType::SpeciesConstructorReturned, "an ArrayBuffer smaller than requested");

    // 20. Let fromBuf be O.[[ArrayBufferData]].
    auto& from_buf = array_buffer_object->buffer();

    // 21. Let toBuf be new.[[ArrayBufferData]].
    auto& to_buf = new_array_buffer_object->buffer();

    // 22. Perform CopyDataBlockBytes(toBuf, 0, fromBuf, first, newLen).
    copy_data_block_bytes(to_buf, 0, from_buf, first, new_length);

    // 23. Return new.
    return new_array_buffer_object;
}

}
