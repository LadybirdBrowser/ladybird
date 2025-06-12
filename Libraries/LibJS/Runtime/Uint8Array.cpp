/*
 * Copyright (c) 2024-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <AK/StringUtils.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/Uint8Array.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS {

void Uint8ArrayConstructorHelpers::initialize(Realm& realm, Object& constructor)
{
    auto& vm = constructor.vm();

    static constexpr u8 attr = Attribute::Writable | Attribute::Configurable;
    constructor.define_native_function(realm, vm.names.fromBase64, from_base64, 1, attr);
    constructor.define_native_function(realm, vm.names.fromHex, from_hex, 1, attr);
}

void Uint8ArrayPrototypeHelpers::initialize(Realm& realm, Object& prototype)
{
    auto& vm = prototype.vm();

    static constexpr u8 attr = Attribute::Writable | Attribute::Configurable;
    prototype.define_native_function(realm, vm.names.toBase64, to_base64, 0, attr);
    prototype.define_native_function(realm, vm.names.toHex, to_hex, 0, attr);
    prototype.define_native_function(realm, vm.names.setFromBase64, set_from_base64, 1, attr);
    prototype.define_native_function(realm, vm.names.setFromHex, set_from_hex, 1, attr);
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

static ThrowCompletionOr<AK::LastChunkHandling> parse_last_chunk_handling(VM& vm, Object& options)
{
    // Let lastChunkHandling be ? Get(opts, "lastChunkHandling").
    auto last_chunk_handling = TRY(options.get(vm.names.lastChunkHandling));

    // If lastChunkHandling is undefined, set lastChunkHandling to "loose".
    if (last_chunk_handling.is_undefined())
        return AK::LastChunkHandling::Loose;

    // If lastChunkHandling is not one of "loose", "strict", or "stop-before-partial", throw a TypeError exception.
    if (last_chunk_handling.is_string()) {
        if (last_chunk_handling.as_string().utf8_string_view() == "loose"sv)
            return AK::LastChunkHandling::Loose;
        if (last_chunk_handling.as_string().utf8_string_view() == "strict"sv)
            return AK::LastChunkHandling::Strict;
        if (last_chunk_handling.as_string().utf8_string_view() == "stop-before-partial"sv)
            return AK::LastChunkHandling::StopBeforePartial;
    }

    return vm.throw_completion<TypeError>(ErrorType::OptionIsNotValidValue, last_chunk_handling, "lastChunkHandling"sv);
}

// 1 Uint8Array.prototype.toBase64 ( [ options ] ), https://tc39.es/proposal-arraybuffer-base64/spec/#sec-uint8array.prototype.tobase64
JS_DEFINE_NATIVE_FUNCTION(Uint8ArrayPrototypeHelpers::to_base64)
{
    auto options_value = vm.argument(0);

    // 1. Let O be the this value.
    // 2. Perform ? ValidateUint8Array(O).
    auto typed_array = TRY(validate_uint8_array(vm));

    // 3. Let opts be ? GetOptionsObject(options).
    auto options = TRY(get_options_object(vm, options_value));

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

// 2 Uint8Array.prototype.toHex ( ), https://tc39.es/proposal-arraybuffer-base64/spec/#sec-uint8array.prototype.tobase64
JS_DEFINE_NATIVE_FUNCTION(Uint8ArrayPrototypeHelpers::to_hex)
{
    // 1. Let O be the this value.
    // 2. Perform ? ValidateUint8Array(O).
    auto typed_array = TRY(validate_uint8_array(vm));

    // 3. Let toEncode be ? GetUint8ArrayBytes(O).
    auto to_encode = TRY(get_uint8_array_bytes(vm, typed_array));

    // 4. Let out be the empty String.
    StringBuilder out;

    // 5. For each byte byte of toEncode, do
    for (auto byte : to_encode.bytes()) {
        // a. Let hex be Number::toString(𝔽(byte), 16).
        // b. Set hex to StringPad(hex, 2, "0", START).
        // c. Set out to the string-concatenation of out and hex.
        out.appendff("{:02x}", byte);
    }

    // 6. Return out.
    return PrimitiveString::create(vm, MUST(out.to_string()));
}

// 3 Uint8Array.fromBase64 ( string [ , options ] ), https://tc39.es/proposal-arraybuffer-base64/spec/#sec-uint8array.frombase64
JS_DEFINE_NATIVE_FUNCTION(Uint8ArrayConstructorHelpers::from_base64)
{
    auto& realm = *vm.current_realm();

    auto string_value = vm.argument(0);
    auto options_value = vm.argument(1);

    // 1. If string is not a String, throw a TypeError exception.
    if (!string_value.is_string())
        return vm.throw_completion<TypeError>(ErrorType::NotAString, string_value);

    // 2. Let opts be ? GetOptionsObject(options).
    auto options = TRY(get_options_object(vm, options_value));

    // 3. Let alphabet be ? Get(opts, "alphabet").
    // 4. If alphabet is undefined, set alphabet to "base64".
    // 5. If alphabet is neither "base64" nor "base64url", throw a TypeError exception.
    auto alphabet = TRY(parse_alphabet(vm, *options));

    // 6. Let lastChunkHandling be ? Get(opts, "lastChunkHandling").
    // 7. If lastChunkHandling is undefined, set lastChunkHandling to "loose".
    // 8. If lastChunkHandling is not one of "loose", "strict", or "stop-before-partial", throw a TypeError exception.
    auto last_chunk_handling = TRY(parse_last_chunk_handling(vm, *options));

    // 9. Let result be FromBase64(string, alphabet, lastChunkHandling).
    auto result = JS::from_base64(vm, string_value.as_string().utf8_string_view(), alphabet, last_chunk_handling);

    // 10. If result.[[Error]] is not none, then
    if (result.error.has_value()) {
        // a. Throw result.[[Error]].
        return result.error.release_value();
    }

    // 11. Let resultLength be the length of result.[[Bytes]].
    auto result_length = result.bytes.size();

    // 12. Let ta be ? AllocateTypedArray("Uint8Array", %Uint8Array%, "%Uint8Array.prototype%", resultLength).
    auto typed_array = TRY(Uint8Array::create(realm, result_length));

    // 13. Set the value at each index of ta.[[ViewedArrayBuffer]].[[ArrayBufferData]] to the value at the corresponding
    //     index of result.[[Bytes]].
    auto& array_buffer_data = typed_array->viewed_array_buffer()->buffer();

    for (size_t index = 0; index < result_length; ++index)
        array_buffer_data[index] = result.bytes[index];

    // 14. Return ta.
    return typed_array;
}

// 4 Uint8Array.prototype.setFromBase64 ( string [ , options ] ), https://tc39.es/proposal-arraybuffer-base64/spec/#sec-uint8array.prototype.setfrombase64
JS_DEFINE_NATIVE_FUNCTION(Uint8ArrayPrototypeHelpers::set_from_base64)
{
    auto& realm = *vm.current_realm();

    auto string_value = vm.argument(0);
    auto options_value = vm.argument(1);

    // 1. Let into be the this value.
    // 2. Perform ? ValidateUint8Array(into).
    auto into = TRY(validate_uint8_array(vm));

    // 3. If string is not a String, throw a TypeError exception.
    if (!string_value.is_string())
        return vm.throw_completion<TypeError>(ErrorType::NotAString, string_value);

    // 4. Let opts be ? GetOptionsObject(options).
    auto options = TRY(get_options_object(vm, options_value));

    // 5. Let alphabet be ? Get(opts, "alphabet").
    // 6. If alphabet is undefined, set alphabet to "base64".
    // 7. If alphabet is neither "base64" nor "base64url", throw a TypeError exception.
    auto alphabet = TRY(parse_alphabet(vm, *options));

    // 8. Let lastChunkHandling be ? Get(opts, "lastChunkHandling").
    // 9. If lastChunkHandling is undefined, set lastChunkHandling to "loose".
    // 10. If lastChunkHandling is not one of "loose", "strict", or "stop-before-partial", throw a TypeError exception.
    auto last_chunk_handling = TRY(parse_last_chunk_handling(vm, *options));

    // 11. Let taRecord be MakeTypedArrayWithBufferWitnessRecord(into, seq-cst).
    auto typed_array_record = make_typed_array_with_buffer_witness_record(into, ArrayBuffer::Order::SeqCst);

    // 12. If IsTypedArrayOutOfBounds(taRecord) is true, throw a TypeError exception.
    if (is_typed_array_out_of_bounds(typed_array_record))
        return vm.throw_completion<TypeError>(ErrorType::BufferOutOfBounds, "TypedArray"sv);

    // 13. Let byteLength be TypedArrayLength(taRecord).
    auto byte_length = typed_array_length(typed_array_record);

    // 14. Let result be FromBase64(string, alphabet, lastChunkHandling, byteLength).
    auto result = JS::from_base64(vm, string_value.as_string().utf8_string_view(), alphabet, last_chunk_handling, byte_length);

    // 15. Let bytes be result.[[Bytes]].
    auto bytes = move(result.bytes);

    // 16. Let written be the length of bytes.
    auto written = bytes.size();

    // 17. NOTE: FromBase64 does not invoke any user code, so the ArrayBuffer backing into cannot have been detached or shrunk.
    // 18. Assert: written ≤ byteLength.
    VERIFY(written <= byte_length);

    // 19. Perform SetUint8ArrayBytes(into, bytes).
    set_uint8_array_bytes(into, bytes);

    // 20. If result.[[Error]] is not none, then
    if (result.error.has_value()) {
        // a. Throw result.[[Error]].
        return result.error.release_value();
    }

    // 21. Let resultObject be OrdinaryObjectCreate(%Object.prototype%).
    auto result_object = Object::create(realm, realm.intrinsics().object_prototype());

    // 22. Perform ! CreateDataPropertyOrThrow(resultObject, "read", 𝔽(result.[[Read]])).
    MUST(result_object->create_data_property(vm.names.read, Value { result.read }));

    // 23. Perform ! CreateDataPropertyOrThrow(resultObject, "written", 𝔽(written)).
    MUST(result_object->create_data_property(vm.names.written, Value { written }));

    // 24. Return resultObject.
    return result_object;
}

// 5 Uint8Array.fromHex ( string ), https://tc39.es/proposal-arraybuffer-base64/spec/#sec-uint8array.fromhex
JS_DEFINE_NATIVE_FUNCTION(Uint8ArrayConstructorHelpers::from_hex)
{
    auto& realm = *vm.current_realm();

    auto string_value = vm.argument(0);

    // 1. If string is not a String, throw a TypeError exception.
    if (!string_value.is_string())
        return vm.throw_completion<TypeError>(ErrorType::NotAString, string_value);

    // 2. Let result be FromHex(string).
    auto result = JS::from_hex(vm, string_value.as_string().utf8_string_view());

    // 3. If result.[[Error]] is not none, then
    if (result.error.has_value()) {
        // a. Throw result.[[Error]].
        return result.error.release_value();
    }

    // 4. Let resultLength be the length of result.[[Bytes]].
    auto result_length = result.bytes.size();

    // 5. Let ta be ? AllocateTypedArray("Uint8Array", %Uint8Array%, "%Uint8Array.prototype%", resultLength).
    auto typed_array = TRY(Uint8Array::create(realm, result_length));

    // 6. Set the value at each index of ta.[[ViewedArrayBuffer]].[[ArrayBufferData]] to the value at the corresponding
    //    index of result.[[Bytes]].
    auto& array_buffer_data = typed_array->viewed_array_buffer()->buffer();

    for (size_t index = 0; index < result_length; ++index)
        array_buffer_data[index] = result.bytes[index];

    // 7. Return ta.
    return typed_array;
}

// 6 Uint8Array.prototype.setFromHex ( string ), https://tc39.es/proposal-arraybuffer-base64/spec/#sec-uint8array.prototype.setfromhex
JS_DEFINE_NATIVE_FUNCTION(Uint8ArrayPrototypeHelpers::set_from_hex)
{
    auto& realm = *vm.current_realm();

    auto string_value = vm.argument(0);

    // 1. Let into be the this value.
    // 2. Perform ? ValidateUint8Array(into).
    auto into = TRY(validate_uint8_array(vm));

    // 3. If string is not a String, throw a TypeError exception.
    if (!string_value.is_string())
        return vm.throw_completion<TypeError>(ErrorType::NotAString, string_value);

    // 4. Let taRecord be MakeTypedArrayWithBufferWitnessRecord(into, seq-cst).
    auto typed_array_record = make_typed_array_with_buffer_witness_record(into, ArrayBuffer::Order::SeqCst);

    // 5. If IsTypedArrayOutOfBounds(taRecord) is true, throw a TypeError exception.
    if (is_typed_array_out_of_bounds(typed_array_record))
        return vm.throw_completion<TypeError>(ErrorType::BufferOutOfBounds, "TypedArray"sv);

    // 6. Let byteLength be TypedArrayLength(taRecord).
    auto byte_length = typed_array_length(typed_array_record);

    // 7. Let result be FromHex(string, byteLength).
    auto result = JS::from_hex(vm, string_value.as_string().utf8_string_view(), byte_length);

    // 8. Let bytes be result.[[Bytes]].
    auto bytes = move(result.bytes);

    // 9. Let written be the length of bytes.
    auto written = bytes.size();

    // 10. NOTE: FromHex does not invoke any user code, so the ArrayBuffer backing into cannot have been detached or shrunk.
    // 11. Assert: written ≤ byteLength.
    VERIFY(written <= byte_length);

    // 12. Perform SetUint8ArrayBytes(into, bytes).
    set_uint8_array_bytes(into, bytes);

    // 13. If result.[[Error]] is not none, then
    if (result.error.has_value()) {
        // a. Throw result.[[Error]].
        return result.error.release_value();
    }

    // 14. Let resultObject be OrdinaryObjectCreate(%Object.prototype%).
    auto result_object = Object::create(realm, realm.intrinsics().object_prototype());

    // 15. Perform ! CreateDataPropertyOrThrow(resultObject, "read", 𝔽(result.[[Read]])).
    MUST(result_object->create_data_property(vm.names.read, Value { result.read }));

    // 16. Perform ! CreateDataPropertyOrThrow(resultObject, "written", 𝔽(written)).
    MUST(result_object->create_data_property(vm.names.written, Value { written }));

    // 17. Return resultObject.
    return result_object;
}

// 7 ValidateUint8Array ( ta ), https://tc39.es/proposal-arraybuffer-base64/spec/#sec-validateuint8array
ThrowCompletionOr<GC::Ref<TypedArrayBase>> validate_uint8_array(VM& vm)
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

// 9 SetUint8ArrayBytes ( into, bytes ), https://tc39.es/proposal-arraybuffer-base64/spec/#sec-writeuint8arraybytes
void set_uint8_array_bytes(TypedArrayBase& into, ReadonlyBytes bytes)
{
    // 1. Let offset be into.[[ByteOffset]].
    auto offset = into.byte_offset();

    // 2. Let len be the length of bytes.
    auto length = bytes.size();

    // 3. Let index be 0.
    // 4. Repeat, while index < len,
    for (u32 index = 0; index < length; ++index) {
        // a. Let byte be bytes[index].
        auto byte = bytes[index];

        // b. Let byteIndexInBuffer be index + offset.
        auto byte_index_in_buffer = index + offset;

        // c. Perform SetValueInBuffer(into.[[ViewedArrayBuffer]], byteIndexInBuffer, uint8, 𝔽(byte), true, unordered).
        into.set_value_in_buffer(byte_index_in_buffer, Value { byte }, ArrayBuffer::Order::Unordered);

        // d. Set index to index + 1.
    }
}

// 10.3 FromBase64 ( string, alphabet, lastChunkHandling [ , maxLength ] ), https://tc39.es/proposal-arraybuffer-base64/spec/#sec-frombase64
DecodeResult from_base64(VM& vm, StringView string, Alphabet alphabet, AK::LastChunkHandling last_chunk_handling, Optional<size_t> max_length)
{
    auto output = MUST(ByteBuffer::create_uninitialized(max_length.value_or_lazy_evaluated([&]() {
        return AK::size_required_to_decode_base64(string);
    })));

    auto result = alphabet == Alphabet::Base64
        ? AK::decode_base64_into(string, output, last_chunk_handling)
        : AK::decode_base64url_into(string, output, last_chunk_handling);

    if (result.is_error()) {
        auto error = vm.throw_completion<SyntaxError>(result.error().error.string_literal());
        return { .read = result.error().valid_input_bytes, .bytes = move(output), .error = move(error) };
    }

    return { .read = result.value(), .bytes = move(output), .error = {} };
}

// 10.4 FromHex ( string [ , maxLength ] ), https://tc39.es/proposal-arraybuffer-base64/spec/#sec-fromhex
DecodeResult from_hex(VM& vm, StringView string, Optional<size_t> max_length)
{
    // 1. If maxLength is not present, let maxLength be 2**53 - 1.
    if (!max_length.has_value())
        max_length = MAX_ARRAY_LIKE_INDEX;

    // 2. Let length be the length of string.
    auto length = string.length();

    // 3. Let bytes be « ».
    ByteBuffer bytes;

    // 4. Let read be 0.
    size_t read = 0;

    // 5. If length modulo 2 is not 0, then
    if (length % 2 != 0) {
        // a. Let error be a new SyntaxError exception.
        auto error = vm.throw_completion<SyntaxError>("Hex string must have an even length"sv);

        // b. Return the Record { [[Read]]: read, [[Bytes]]: bytes, [[Error]]: error }.
        return { .read = read, .bytes = move(bytes), .error = move(error) };
    }

    // 6. Repeat, while read < length and the length of bytes < maxLength,
    while (read < length && bytes.size() < *max_length) {
        // a. Let hexits be the substring of string from read to read + 2.
        auto hexits = string.substring_view(read, 2);

        // d. Let byte be the integer value represented by hexits in base-16 notation, using the letters A-F and a-f
        //    for digits with values 10 through 15.
        // NOTE: We do this early so that we don't have to effectively parse hexits twice.
        auto byte = AK::StringUtils::convert_to_uint_from_hex<u8>(hexits, AK::TrimWhitespace::No);

        // b. If hexits contains any code units which are not in "0123456789abcdefABCDEF", then
        if (!byte.has_value()) {
            // i. Let error be a new SyntaxError exception.
            auto error = vm.throw_completion<SyntaxError>("Hex string must only contain hex characters"sv);

            // ii. Return the Record { [[Read]]: read, [[Bytes]]: bytes, [[Error]]: error }.
            return { .read = read, .bytes = move(bytes), .error = move(error) };
        }

        // c. Set read to read + 2.
        read += 2;

        // e. Append byte to bytes.
        bytes.append(*byte);
    }

    // 7. Return the Record { [[Read]]: read, [[Bytes]]: bytes, [[Error]]: none }.
    return { .read = read, .bytes = move(bytes), .error = {} };
}

}
