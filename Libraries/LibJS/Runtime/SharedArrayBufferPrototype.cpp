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

GC_DEFINE_ALLOCATOR(SharedArrayBufferPrototype);

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
    define_native_function(realm, vm.names.grow, grow, 1, attr);
    define_native_accessor(realm, vm.names.growable, growable_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.maxByteLength, max_byte_length, {}, Attribute::Configurable);
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

// 25.2.5.3 SharedArrayBuffer.prototype.grow ( newLength ), https://tc39.es/ecma262/#sec-sharedarraybuffer.prototype.grow
JS_DEFINE_NATIVE_FUNCTION(SharedArrayBufferPrototype::grow)
{
    auto new_length = vm.argument(0);

    // 1. Let O be the this value.
    auto array_buffer_object = TRY(typed_this_value(vm));

    // 2. Perform ? RequireInternalSlot(O, [[ArrayBufferMaxByteLength]]).
    if (array_buffer_object->is_fixed_length())
        return vm.throw_completion<TypeError>(ErrorType::FixedArrayBuffer);

    // 3. If IsSharedArrayBuffer(O) is false, throw a TypeError exception.
    if (!array_buffer_object->is_shared_array_buffer())
        return vm.throw_completion<TypeError>(ErrorType::NotASharedArrayBuffer);

    // 4. Let newByteLength be ? ToIndex(newLength).
    auto new_byte_length = TRY(new_length.to_index(vm));

    // 5. Let hostHandled be ? HostGrowSharedArrayBuffer(O, newByteLength).
    auto host_handled = TRY(vm.host_grow_shared_array_buffer(array_buffer_object, new_byte_length));

    // 6. If hostHandled is handled, return undefined.
    if (host_handled == HandledByHost::Handled)
        return js_undefined();

    // FIXME: 7. Let AR be the Agent Record of the surrounding agent.
    // FIXME: 8. Let isLittleEndian be AR.[[LittleEndian]].
    // FIXME: 9. Let byteLengthBlock be O.[[ArrayBufferByteLengthData]].
    // FIXME: 10. Let currentByteLengthRawBytes be GetRawBytesFromSharedBlock(byteLengthBlock, 0, biguint64, true, seq-cst).
    // FIXME: 11. Let newByteLengthRawBytes be NumericToRawBytes(biguint64, ℤ(newByteLength), isLittleEndian).
    // FIXME: 12. Repeat,
    // FIXME:         a. NOTE: This is a compare-and-exchange loop to ensure that parallel, racing grows of the same buffer are totally ordered, are not lost, and do not silently do nothing. The loop exits if it was able to attempt to grow uncontended.
    // FIXME:         b. Let currentByteLength be ℝ(RawBytesToNumeric(biguint64, currentByteLengthRawBytes, isLittleEndian)).
    auto current_byte_length = array_buffer_object->byte_length();

    //                c. If newByteLength = currentByteLength, return undefined.
    if (new_byte_length == current_byte_length)
        return js_undefined();

    //                d. If newByteLength < currentByteLength or newByteLength > O.[[ArrayBufferMaxByteLength]], throw a RangeError exception.
    if (new_byte_length < current_byte_length)
        return vm.throw_completion<RangeError>(ErrorType::ByteLengthLessThanPreviousByteLength, new_byte_length, current_byte_length);
    if (new_byte_length > array_buffer_object->max_byte_length())
        return vm.throw_completion<RangeError>(ErrorType::ByteLengthExceedsMaxByteLength, new_byte_length, array_buffer_object->max_byte_length());

    // FIXME:         e. Let byteLengthDelta be newByteLength - currentByteLength.
    // FIXME:         f. If it is impossible to create a new Shared Data Block value consisting of byteLengthDelta bytes, throw a RangeError exception.
    // FIXME:         g. NOTE: No new Shared Data Block is constructed and used here. The observable behaviour of growable SharedArrayBuffers is specified by allocating a max-sized Shared Data Block at construction time, and this step captures the requirement that implementations that run out of memory must throw a RangeError.
    // FIXME:         h. Let readByteLengthRawBytes be AtomicCompareExchangeInSharedBlock(byteLengthBlock, 0, 8, currentByteLengthRawBytes, newByteLengthRawBytes).
    // FIXME:         i. If ByteListEqual(readByteLengthRawBytes, currentByteLengthRawBytes) is true, return undefined.
    // FIXME:         j. Set currentByteLengthRawBytes to readByteLengthRawBytes.

    if (auto result = array_buffer_object->buffer().try_resize(new_byte_length, ByteBuffer::ZeroFillNewElements::Yes); result.is_error())
        return vm.throw_completion<RangeError>(ErrorType::NotEnoughMemoryToAllocate, new_byte_length);

    return js_undefined();
}

// 25.2.5.4 get SharedArrayBuffer.prototype.growable, https://tc39.es/ecma262/#sec-get-sharedarraybuffer.prototype.growable
JS_DEFINE_NATIVE_FUNCTION(SharedArrayBufferPrototype::growable_getter)
{
    // 1. Let O be the this value.
    // 2. Perform ? RequireInternalSlot(O, [[ArrayBufferData]]).
    auto array_buffer_object = TRY(typed_this_value(vm));

    // 3. If IsSharedArrayBuffer(O) is false, throw a TypeError exception.
    if (!array_buffer_object->is_shared_array_buffer())
        return vm.throw_completion<TypeError>(ErrorType::NotASharedArrayBuffer);

    // 4. If IsFixedLengthArrayBuffer(O) is false, return true; otherwise return false.
    return Value { !array_buffer_object->is_fixed_length() };
}

// 25.2.5.5 get SharedArrayBuffer.prototype.maxByteLength, https://tc39.es/ecma262/#sec-get-sharedarraybuffer.prototype.maxbytelength
JS_DEFINE_NATIVE_FUNCTION(SharedArrayBufferPrototype::max_byte_length)
{
    // 1. Let O be the this value.
    // 2. Perform ? RequireInternalSlot(O, [[ArrayBufferData]]).
    auto array_buffer_object = TRY(typed_this_value(vm));

    // 3. If IsSharedArrayBuffer(O) is false, throw a TypeError exception.
    if (!array_buffer_object->is_shared_array_buffer())
        return vm.throw_completion<TypeError>(ErrorType::NotASharedArrayBuffer);

    // 4. If IsFixedLengthArrayBuffer(O) is true, then
    //        a. Let length be O.[[ArrayBufferByteLength]].
    // 5. Else,
    //        a. Let length be O.[[ArrayBufferMaxByteLength]].
    auto length = array_buffer_object->is_fixed_length() ? array_buffer_object->byte_length() : array_buffer_object->max_byte_length();

    // 6. Return 𝔽(length).
    return Value { length };
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
    auto* new_array_buffer_object = as_if<ArrayBuffer>(*new_array_buffer);
    if (!new_array_buffer_object)
        return vm.throw_completion<TypeError>(ErrorType::SpeciesConstructorDidNotCreate, "an ArrayBuffer");

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
