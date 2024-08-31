/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/StringBuilder.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
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
}

void Uint8ArrayPrototypeHelpers::initialize(Realm& realm, Object& prototype)
{
    auto& vm = prototype.vm();

    static constexpr u8 attr = Attribute::Writable | Attribute::Configurable;
    prototype.define_native_function(realm, vm.names.toBase64, to_base64, 0, attr);
    prototype.define_native_function(realm, vm.names.toHex, to_hex, 0, attr);
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

static ThrowCompletionOr<LastChunkHandling> parse_last_chunk_handling(VM& vm, Object& options)
{
    // Let lastChunkHandling be ? Get(opts, "lastChunkHandling").
    auto last_chunk_handling = TRY(options.get(vm.names.lastChunkHandling));

    // If lastChunkHandling is undefined, set lastChunkHandling to "loose".
    if (last_chunk_handling.is_undefined())
        return LastChunkHandling::Loose;

    // If lastChunkHandling is not one of "loose", "strict", or "stop-before-partial", throw a TypeError exception.
    if (last_chunk_handling.is_string()) {
        if (last_chunk_handling.as_string().utf8_string_view() == "loose"sv)
            return LastChunkHandling::Loose;
        if (last_chunk_handling.as_string().utf8_string_view() == "strict"sv)
            return LastChunkHandling::Strict;
        if (last_chunk_handling.as_string().utf8_string_view() == "stop-before-partial"sv)
            return LastChunkHandling::StopBeforePartial;
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
    auto* options = TRY(Temporal::get_options_object(vm, options_value));

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

// 10.1 SkipAsciiWhitespace ( string, index ), https://tc39.es/proposal-arraybuffer-base64/spec/#sec-skipasciiwhitespace
static size_t skip_ascii_whitespace(StringView string, size_t index)
{
    // 1. Let length be the length of string.
    auto length = string.length();

    // 2. Repeat, while index < length,
    while (index < length) {
        // a. Let char be the code unit at index index of string.
        auto ch = string[index];

        // b. If char is neither 0x0009 (TAB), 0x000A (LF), 0x000C (FF), 0x000D (CR), nor 0x0020 (SPACE), then
        if (ch != '\t' && ch != '\n' && ch != '\f' && ch != '\r' && ch != ' ') {
            // i. Return index.
            return index;
        }

        // c. Set index to index + 1.
        ++index;
    }

    // 3. Return index.
    return index;
}

// 10.2 DecodeBase64Chunk ( chunk [ , throwOnExtraBits ] ), https://tc39.es/proposal-arraybuffer-base64/spec/#sec-frombase64
static ThrowCompletionOr<ByteBuffer> decode_base64_chunk(VM& vm, StringBuilder& chunk, Optional<bool> throw_on_extra_bits = {})
{
    // 1. Let chunkLength be the length of chunk.
    auto chunk_length = chunk.length();

    // 2. If chunkLength is 2, then
    if (chunk_length == 2) {
        // a. Set chunk to the string-concatenation of chunk and "AA".
        chunk.append("AA"sv);
    }
    // 3. Else if chunkLength is 3, then
    else if (chunk_length == 3) {
        // a. Set chunk to the string-concatenation of chunk and "A".
        chunk.append("A"sv);
    }
    // 4. Else,
    else {
        // a. Assert: chunkLength is 4.
        VERIFY(chunk_length == 4);
    }

    // 5. Let byteSequence be the unique sequence of 3 bytes resulting from decoding chunk as base64 (such that applying
    //    the base64 encoding specified in section 4 of RFC 4648 to byteSequence would produce chunk).
    // 6. Let bytes be a List whose elements are the elements of byteSequence, in order.
    auto bytes = MUST(decode_base64(chunk.string_view()));

    // 7. If chunkLength is 2, then
    if (chunk_length == 2) {
        // a. Assert: throwOnExtraBits is present.
        VERIFY(throw_on_extra_bits.has_value());

        // b. If throwOnExtraBits is true and bytes[1] ≠ 0, then
        if (*throw_on_extra_bits && bytes[1] != 0) {
            // i. Throw a SyntaxError exception.
            return vm.throw_completion<SyntaxError>("Extra bits found at end of chunk"sv);
        }

        // c. Return « bytes[0] ».
        return MUST(bytes.slice(0, 1));
    }

    // 8. Else if chunkLength is 3, then
    if (chunk_length == 3) {
        // a. Assert: throwOnExtraBits is present.
        VERIFY(throw_on_extra_bits.has_value());

        // b. If throwOnExtraBits is true and bytes[2] ≠ 0, then
        if (*throw_on_extra_bits && bytes[2] != 0) {
            // i. Throw a SyntaxError exception.
            return vm.throw_completion<SyntaxError>("Extra bits found at end of chunk"sv);
        }

        // c. Return « bytes[0], bytes[1] ».
        return MUST(bytes.slice(0, 2));
    }

    // 9. Else,
    //     a. Return bytes.
    return bytes;
}

// 10.3 FromBase64 ( string, alphabet, lastChunkHandling [ , maxLength ] ), https://tc39.es/proposal-arraybuffer-base64/spec/#sec-frombase64
DecodeResult from_base64(VM& vm, StringView string, Alphabet alphabet, LastChunkHandling last_chunk_handling, Optional<size_t> max_length)
{
    // FIXME: We can only use simdutf when the last-chunk-handling parameter is "loose". Upstream is planning to implement
    //        the remaining options. When that is complete, we should be able to remove the slow implementation below. See:
    //        https://github.com/simdutf/simdutf/issues/440
    if (last_chunk_handling == LastChunkHandling::Loose) {
        auto output = MUST(ByteBuffer::create_uninitialized(max_length.value_or_lazy_evaluated([&]() {
            return AK::size_required_to_decode_base64(string);
        })));

        auto result = alphabet == Alphabet::Base64
            ? AK::decode_base64_into(string, output)
            : AK::decode_base64url_into(string, output);

        if (result.is_error()) {
            auto error = vm.throw_completion<SyntaxError>(result.error().error.string_literal());
            return { .read = result.error().valid_input_bytes, .bytes = move(output), .error = move(error) };
        }

        return { .read = result.value(), .bytes = move(output), .error = {} };
    }

    // 1. If maxLength is not present, then
    if (!max_length.has_value()) {
        // a. Let maxLength be 2**53 - 1.
        max_length = MAX_ARRAY_LIKE_INDEX;

        // b. NOTE: Because the input is a string, the length of strings is limited to 2**53 - 1 characters, and the
        //    output requires no more bytes than the input has characters, this limit can never be reached. However, it
        //    is editorially convenient to use a finite value here.
    }

    // 2. NOTE: The order of validation and decoding in the algorithm below is not observable. Implementations are
    //    encouraged to perform them in whatever order is most efficient, possibly interleaving validation with decoding,
    //    as long as the behaviour is observably equivalent.

    // 3. If maxLength is 0, then
    if (max_length == 0uz) {
        // a. Return the Record { [[Read]]: 0, [[Bytes]]: « », [[Error]]: none }.
        return { .read = 0, .bytes = {}, .error = {} };
    }

    // 4. Let read be 0.
    size_t read = 0;

    // 5. Let bytes be « ».
    ByteBuffer bytes;

    // 6. Let chunk be the empty String.
    StringBuilder chunk;

    // 7. Let chunkLength be 0.
    size_t chunk_length = 0;

    // 8. Let index be 0.
    size_t index = 0;

    // 9. Let length be the length of string.
    auto length = string.length();

    // 10. Repeat,
    while (true) {
        // a. Set index to SkipAsciiWhitespace(string, index).
        index = skip_ascii_whitespace(string, index);

        // b. If index = length, then
        if (index == length) {
            // i. If chunkLength > 0, then
            if (chunk_length > 0) {
                // 1. If lastChunkHandling is "stop-before-partial", then
                if (last_chunk_handling == LastChunkHandling::StopBeforePartial) {
                    // a. Return the Record { [[Read]]: read, [[Bytes]]: bytes, [[Error]]: none }.
                    return { .read = read, .bytes = move(bytes), .error = {} };
                }
                // 2. Else if lastChunkHandling is "loose", then
                else if (last_chunk_handling == LastChunkHandling::Loose) {
                    VERIFY_NOT_REACHED();
                }
                // 3. Else,
                else {
                    // a. Assert: lastChunkHandling is "strict".
                    VERIFY(last_chunk_handling == LastChunkHandling::Strict);

                    // b. Let error be a new SyntaxError exception.
                    auto error = vm.throw_completion<SyntaxError>("Invalid trailing data"sv);

                    // c. Return the Record { [[Read]]: read, [[Bytes]]: bytes, [[Error]]: error }.
                    return { .read = read, .bytes = move(bytes), .error = move(error) };
                }
            }

            // ii. Return the Record { [[Read]]: length, [[Bytes]]: bytes, [[Error]]: none }.
            return { .read = length, .bytes = move(bytes), .error = {} };
        }

        // c. Let char be the substring of string from index to index + 1.
        auto ch = string[index];

        // d. Set index to index + 1.
        ++index;

        // e. If char is "=", then
        if (ch == '=') {
            // i. If chunkLength < 2, then
            if (chunk_length < 2) {
                // 1. Let error be a new SyntaxError exception.
                auto error = vm.throw_completion<SyntaxError>("Unexpected padding character"sv);

                // 2. Return the Record { [[Read]]: read, [[Bytes]]: bytes, [[Error]]: error }.
                return { .read = read, .bytes = move(bytes), .error = move(error) };
            }

            // ii. Set index to SkipAsciiWhitespace(string, index).
            index = skip_ascii_whitespace(string, index);

            // iii. If chunkLength = 2, then
            if (chunk_length == 2) {
                // 1. If index = length, then
                if (index == length) {
                    // a. If lastChunkHandling is "stop-before-partial", then
                    if (last_chunk_handling == LastChunkHandling::StopBeforePartial) {
                        // i. Return the Record { [[Read]]: read, [[Bytes]]: bytes, [[Error]]: none }.
                        return { .read = read, .bytes = move(bytes), .error = {} };
                    }

                    // b. Let error be a new SyntaxError exception.
                    auto error = vm.throw_completion<SyntaxError>("Incomplete number of padding characters"sv);

                    // c. Return the Record { [[Read]]: read, [[Bytes]]: bytes, [[Error]]: error }.
                    return { .read = read, .bytes = move(bytes), .error = move(error) };
                }

                // 2. Set char to the substring of string from index to index + 1.
                ch = string[index];

                // 3. If char is "=", then
                if (ch == '=') {
                    // a. Set index to SkipAsciiWhitespace(string, index + 1).
                    index = skip_ascii_whitespace(string, index + 1);
                }
            }

            // iv. If index < length, then
            if (index < length) {
                // 1. Let error be a new SyntaxError exception.
                auto error = vm.throw_completion<SyntaxError>("Unexpected padding character"sv);

                // 2. Return the Record { [[Read]]: read, [[Bytes]]: bytes, [[Error]]: error }.
                return { .read = read, .bytes = move(bytes), .error = move(error) };
            }

            // v. If lastChunkHandling is "strict", let throwOnExtraBits be true.
            // vi. Else, let throwOnExtraBits be false.
            auto throw_on_extra_bits = last_chunk_handling == LastChunkHandling::Strict;

            // vii. Let decodeResult be Completion(DecodeBase64Chunk(chunk, throwOnExtraBits)).
            auto decode_result = decode_base64_chunk(vm, chunk, throw_on_extra_bits);

            // viii. If decodeResult is an abrupt completion, then
            if (decode_result.is_error()) {
                // 1. Let error be decodeResult.[[Value]].
                auto error = decode_result.release_error();

                // 2. Return the Record { [[Read]]: read, [[Bytes]]: bytes, [[Error]]: error }.
                return { .read = read, .bytes = move(bytes), .error = move(error) };
            }

            // ix. Set bytes to the list-concatenation of bytes and ! decodeResult.
            bytes.append(decode_result.release_value());

            // x. Return the Record { [[Read]]: length, [[Bytes]]: bytes, [[Error]]: none }.
            return { .read = length, .bytes = move(bytes), .error = {} };
        }

        // f. If alphabet is "base64url", then
        if (alphabet == Alphabet::Base64URL) {
            // i. If char is either "+" or "/", then
            if (ch == '+' || ch == '/') {
                // 1. Let error be a new SyntaxError exception.
                auto error = vm.throw_completion<SyntaxError>(MUST(String::formatted("Invalid character '{}'"sv, ch)));

                // 2. Return the Record { [[Read]]: read, [[Bytes]]: bytes, [[Error]]: error }.
                return { .read = read, .bytes = move(bytes), .error = move(error) };
            }
            // ii. Else if char is "-", then
            else if (ch == '-') {
                // 1. Set char to "+".
                ch = '+';
            }
            // iii. Else if char is "_", then
            else if (ch == '-') {
                // 1. Set char to "/".
                ch = '/';
            }
        }

        // g. If the sole code unit of char is not an element of the standard base64 alphabet, then
        static constexpr auto standard_base64_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"sv;

        if (!standard_base64_alphabet.contains(ch)) {
            // i. Let error be a new SyntaxError exception.
            auto error = vm.throw_completion<SyntaxError>(MUST(String::formatted("Invalid character '{}'"sv, ch)));

            // ii. Return the Record { [[Read]]: read, [[Bytes]]: bytes, [[Error]]: error }.
            return { .read = read, .bytes = move(bytes), .error = move(error) };
        }

        // h. Let remaining be maxLength - the length of bytes.
        auto remaining = *max_length - bytes.size();

        // i. If remaining = 1 and chunkLength = 2, or if remaining = 2 and chunkLength = 3, then
        if ((remaining == 1 && chunk_length == 2) || (remaining == 2 && chunk_length == 3)) {
            // i. Return the Record { [[Read]]: read, [[Bytes]]: bytes, [[Error]]: none }.
            return { .read = read, .bytes = move(bytes), .error = {} };
        }

        // j. Set chunk to the string-concatenation of chunk and char.
        chunk.append(ch);

        // k. Set chunkLength to the length of chunk.
        chunk_length = chunk.length();

        // l. If chunkLength = 4, then
        if (chunk_length == 4) {
            // i. Set bytes to the list-concatenation of bytes and ! DecodeBase64Chunk(chunk).
            bytes.append(MUST(decode_base64_chunk(vm, chunk)));

            // ii. Set chunk to the empty String.
            chunk.clear();

            // iii. Set chunkLength to 0.
            chunk_length = 0;

            // iv. Set read to index.
            read = index;

            // v. If the length of bytes = maxLength, then
            if (bytes.size() == max_length) {
                // 1. Return the Record { [[Read]]: read, [[Bytes]]: bytes, [[Error]]: none }.
                return { .read = read, .bytes = move(bytes), .error = {} };
            }
        }
    }
}

}
