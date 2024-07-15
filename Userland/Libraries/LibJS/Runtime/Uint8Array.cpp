/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/Uint8Array.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS {

void Uint8ArrayPrototypeHelpers::initialize(Realm& realm, Object& prototype)
{
    auto& vm = prototype.vm();

    static constexpr u8 attr = Attribute::Writable | Attribute::Configurable;
    prototype.define_native_function(realm, vm.names.toBase64, to_base64, 0, attr);
}

static ThrowCompletionOr<Alphabet> parse_alphabet(VM& vm, Object& options)
{
    // Let alphabet be ? Get(opts, "alphabet").
    auto alphabet = TRY(options.get(vm.names.alphabet));

    // If alphabet is undefined, set alphabet to "base64".
    if (alphabet.is_undefined())
        return Alphabet::Base64;

    // If alphabet is neither "base64" nor "base64url", throw a TypeError exception.
    if (alphabet.is_string()) {
        if (alphabet.as_string().utf8_string_view() == "base64"sv)
            return Alphabet::Base64;
        if (alphabet.as_string().utf8_string_view() == "base64url"sv)
            return Alphabet::Base64URL;
    }

    return vm.throw_completion<TypeError>(ErrorType::OptionIsNotValidValue, alphabet, "alphabet"sv);
}

// 1 Uint8Array.prototype.toBase64 ( [ options ] ), https://tc39.es/proposal-arraybuffer-base64/spec/#sec-uint8array.prototype.tobase64
JS_DEFINE_NATIVE_FUNCTION(Uint8ArrayPrototypeHelpers::to_base64)
{
    auto options_value = vm.argument(0);

    // 1. Let O be the this value.
    // 2. Perform ? ValidateUint8Array(O).
    auto typed_array = TRY(validate_uint8_array(vm));

    // 3. Let opts be ? GetOptionsObject(options).
    auto* options = TRY(Temporal::get_options_object(vm, options_value));

    // 4. Let alphabet be ? Get(opts, "alphabet").
    // 5. If alphabet is undefined, set alphabet to "base64".
    // 6. If alphabet is neither "base64" nor "base64url", throw a TypeError exception.
    auto alphabet = TRY(parse_alphabet(vm, *options));

    // 7. Let omitPadding be ToBoolean(? Get(opts, "omitPadding")).
    auto omit_padding_value = TRY(options->get(vm.names.omitPadding)).to_boolean();
    auto omit_padding = omit_padding_value ? AK::OmitPadding::Yes : AK::OmitPadding::No;

    // 8. Let toEncode be ? GetUint8ArrayBytes(O).
    auto to_encode = TRY(get_uint8_array_bytes(vm, typed_array));

    String out_ascii;

    // 9. If alphabet is "base64", then
    if (alphabet == Alphabet::Base64) {
        // a. Let outAscii be the sequence of code points which results from encoding toEncode according to the base64
        //    encoding specified in section 4 of RFC 4648. Padding is included if and only if omitPadding is false.
        out_ascii = MUST(encode_base64(to_encode, omit_padding));
    }
    // 10. Else,
    else {
        // a. Assert: alphabet is "base64url".
        // b. Let outAscii be the sequence of code points which results from encoding toEncode according to the base64url
        //    encoding specified in section 5 of RFC 4648. Padding is included if and only if omitPadding is false.
        out_ascii = MUST(encode_base64url(to_encode, omit_padding));
    }

    // 11. Return CodePointsToString(outAscii).
    return PrimitiveString::create(vm, move(out_ascii));
}

// 7 ValidateUint8Array ( ta ), https://tc39.es/proposal-arraybuffer-base64/spec/#sec-validateuint8array
ThrowCompletionOr<NonnullGCPtr<TypedArrayBase>> validate_uint8_array(VM& vm)
{
    auto this_object = TRY(vm.this_value().to_object(vm));

    // 1. Perform ? RequireInternalSlot(ta, [[TypedArrayName]]).
    if (!this_object->is_typed_array())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObjectOfType, "Uint8Array");

    auto& typed_array = static_cast<TypedArrayBase&>(*this_object.ptr());

    // 2. If ta.[[TypedArrayName]] is not "Uint8Array", throw a TypeError exception.
    if (typed_array.kind() != TypedArrayBase::Kind::Uint8Array)
        return vm.throw_completion<TypeError>(ErrorType::NotAnObjectOfType, "Uint8Array");

    // 3. Return UNUSED.
    return typed_array;
}

// 8 GetUint8ArrayBytes ( ta ), https://tc39.es/proposal-arraybuffer-base64/spec/#sec-getuint8arraybytes
ThrowCompletionOr<ByteBuffer> get_uint8_array_bytes(VM& vm, TypedArrayBase const& typed_array)
{
    // 1. Let buffer be ta.[[ViewedArrayBuffer]].
    // 2. Let taRecord be MakeTypedArrayWithBufferWitnessRecord(ta, SEQ-CST).
    auto typed_array_record = make_typed_array_with_buffer_witness_record(typed_array, ArrayBuffer::Order::SeqCst);

    // 3. If IsTypedArrayOutOfBounds(taRecord) is true, throw a TypeError exception.
    if (is_typed_array_out_of_bounds(typed_array_record))
        return vm.throw_completion<TypeError>(ErrorType::BufferOutOfBounds, "TypedArray"sv);

    // 4. Let len be TypedArrayLength(taRecord).
    auto length = typed_array_length(typed_array_record);

    // 5. Let byteOffset be ta.[[ByteOffset]].
    auto byte_offset = typed_array.byte_offset();

    // 6. Let bytes be a new empty List.
    ByteBuffer bytes;

    // 7. Let index be 0.
    // 8. Repeat, while index < len,
    for (u32 index = 0; index < length; ++index) {
        // a. Let byteIndex be byteOffset + index.
        auto byte_index = byte_offset + index;

        // b. Let byte be ℝ(GetValueFromBuffer(buffer, byteIndex, UINT8, true, UNORDERED)).
        auto byte = typed_array.get_value_from_buffer(byte_index, ArrayBuffer::Order::Unordered);

        // c. Append byte to bytes.
        bytes.append(MUST(byte.to_u8(vm)));

        // d. Set index to index + 1.
    }

    // 9. Return bytes.
    return bytes;
}

}
