/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/Types.h>

#include <simdutf.h>

namespace AK {

size_t size_required_to_decode_base64(StringView input)
{
    return simdutf::maximal_binary_length_from_base64(input.characters_without_null_termination(), input.length());
}

size_t size_required_to_decode_base64(Utf16View input)
{
    if (input.has_ascii_storage())
        return size_required_to_decode_base64(StringView { input.bytes() });
    return simdutf::maximal_binary_length_from_base64(input.utf16_span().data(), input.length_in_code_units());
}

static constexpr simdutf::last_chunk_handling_options to_simdutf_last_chunk_handling(LastChunkHandling last_chunk_handling)
{
    switch (last_chunk_handling) {
    case LastChunkHandling::Loose:
        return simdutf::last_chunk_handling_options::loose;
    case LastChunkHandling::Strict:
        return simdutf::last_chunk_handling_options::strict;
    case LastChunkHandling::StopBeforePartial:
        return simdutf::last_chunk_handling_options::stop_before_partial;
    }

    VERIFY_NOT_REACHED();
}

static Optional<InvalidBase64> base64_error_from_result(simdutf::error_code error_code, size_t valid_input_bytes)
{
    if (error_code == simdutf::SUCCESS || error_code == simdutf::OUTPUT_BUFFER_TOO_SMALL)
        return {};

    struct DecodeError {
        Base64DecodeError decode_error;
        Error error;
    };

    auto [decode_error, error] = [&]() -> DecodeError {
        switch (error_code) {
        case simdutf::BASE64_EXTRA_BITS:
            return { Base64DecodeError::ExtraBits, Error::from_string_literal("Extra bits found at end of chunk") };
        case simdutf::BASE64_INPUT_REMAINDER:
            return { Base64DecodeError::InputRemainder, Error::from_string_literal("Invalid trailing data") };
        case simdutf::INVALID_BASE64_CHARACTER:
            return { Base64DecodeError::InvalidCharacter, Error::from_string_literal("Invalid base64 character") };
        default:
            return { Base64DecodeError::InvalidData, Error::from_string_literal("Invalid base64-encoded data") };
        }
    }();

    return InvalidBase64 { .decode_error = decode_error, .error = move(error), .valid_input_bytes = valid_input_bytes };
}

template<typename CodeUnit>
static ErrorOr<size_t, InvalidBase64> decode_base64_into_span(ReadonlySpan<CodeUnit> input, ByteBuffer& output, LastChunkHandling last_chunk_handling, simdutf::base64_options options)
{
    static constexpr auto decode_up_to_bad_character = true;
    auto output_length = output.size();

    auto result = simdutf::base64_to_binary_safe(
        input.data(),
        input.size(),
        reinterpret_cast<char*>(output.data()),
        output_length,
        options,
        to_simdutf_last_chunk_handling(last_chunk_handling),
        decode_up_to_bad_character);

    VERIFY(output_length <= output.size());
    output.resize(output_length);

    if (auto error = base64_error_from_result(result.error, result.count); error.has_value())
        return error.release_value();

    return result.count;
}

static ErrorOr<size_t, InvalidBase64> decode_base64_into_impl(StringView input, ByteBuffer& output, LastChunkHandling last_chunk_handling, simdutf::base64_options options)
{
    return decode_base64_into_span(ReadonlySpan<char> { input.characters_without_null_termination(), input.length() }, output, last_chunk_handling, options);
}

static ErrorOr<size_t, InvalidBase64> decode_base64_into_impl(Utf16View input, ByteBuffer& output, LastChunkHandling last_chunk_handling, simdutf::base64_options options)
{
    if (input.has_ascii_storage())
        return decode_base64_into_span(input.ascii_span(), output, last_chunk_handling, options);
    return decode_base64_into_span(input.utf16_span(), output, last_chunk_handling, options);
}

static ErrorOr<ByteBuffer> decode_base64_impl(StringView input, LastChunkHandling last_chunk_handling, simdutf::base64_options options)
{
    ByteBuffer output;
    TRY(output.try_resize(size_required_to_decode_base64(input)));

    auto result = simdutf::base64_to_binary_details(
        input.characters_without_null_termination(),
        input.length(),
        reinterpret_cast<char*>(output.data()),
        options,
        to_simdutf_last_chunk_handling(last_chunk_handling));

    if (auto error = base64_error_from_result(result.error, result.input_count); error.has_value())
        return error.release_value().error;

    VERIFY(result.output_count <= output.size());
    output.resize(result.output_count);

    return output;
}

static ErrorOr<String> encode_base64_impl(ReadonlyBytes input, simdutf::base64_options options)
{
    if (input.is_empty())
        return String {};

    u8* buffer = nullptr;
    auto output = TRY(AK::Detail::StringData::create_uninitialized(
        simdutf::base64_length_from_binary(input.size(), options), buffer));

    simdutf::binary_to_base64(
        reinterpret_cast<char const*>(input.data()),
        input.size(),
        reinterpret_cast<char*>(buffer),
        options);

    return String { move(output) };
}

static Utf16String encode_base64_to_utf16_impl(ReadonlyBytes input, simdutf::base64_options options)
{
    if (input.is_empty())
        return {};

    return Utf16String::create_uninitialized_ascii(
        simdutf::base64_length_from_binary(input.size(), options),
        [&](Bytes buffer) {
            simdutf::binary_to_base64(
                reinterpret_cast<char const*>(input.data()),
                input.size(),
                reinterpret_cast<char*>(buffer.data()),
                options);
        });
}

ErrorOr<ByteBuffer> decode_base64(StringView input, LastChunkHandling last_chunk_handling)
{
    return decode_base64_impl(input, last_chunk_handling, simdutf::base64_default);
}

ErrorOr<ByteBuffer> decode_base64url(StringView input, LastChunkHandling last_chunk_handling)
{
    return decode_base64_impl(input, last_chunk_handling, simdutf::base64_url);
}

ErrorOr<size_t, InvalidBase64> decode_base64_into(StringView input, ByteBuffer& output, LastChunkHandling last_chunk_handling)
{
    return decode_base64_into_impl(input, output, last_chunk_handling, simdutf::base64_default);
}

ErrorOr<size_t, InvalidBase64> decode_base64url_into(StringView input, ByteBuffer& output, LastChunkHandling last_chunk_handling)
{
    return decode_base64_into_impl(input, output, last_chunk_handling, simdutf::base64_url);
}

ErrorOr<size_t, InvalidBase64> decode_base64_into(Utf16View input, ByteBuffer& output, LastChunkHandling last_chunk_handling)
{
    return decode_base64_into_impl(input, output, last_chunk_handling, simdutf::base64_default);
}

ErrorOr<size_t, InvalidBase64> decode_base64url_into(Utf16View input, ByteBuffer& output, LastChunkHandling last_chunk_handling)
{
    return decode_base64_into_impl(input, output, last_chunk_handling, simdutf::base64_url);
}

ErrorOr<String> encode_base64(ReadonlyBytes input, OmitPadding omit_padding)
{
    auto options = omit_padding == OmitPadding::Yes
        ? simdutf::base64_default_no_padding
        : simdutf::base64_default;

    return encode_base64_impl(input, options);
}

ErrorOr<String> encode_base64url(ReadonlyBytes input, OmitPadding omit_padding)
{
    auto options = omit_padding == OmitPadding::Yes
        ? simdutf::base64_url
        : simdutf::base64_url_with_padding;

    return encode_base64_impl(input, options);
}

ErrorOr<Utf16String> encode_base64_to_utf16(ReadonlyBytes input, OmitPadding omit_padding)
{
    auto options = omit_padding == OmitPadding::Yes
        ? simdutf::base64_default_no_padding
        : simdutf::base64_default;

    return encode_base64_to_utf16_impl(input, options);
}

ErrorOr<Utf16String> encode_base64url_to_utf16(ReadonlyBytes input, OmitPadding omit_padding)
{
    auto options = omit_padding == OmitPadding::Yes
        ? simdutf::base64_url
        : simdutf::base64_url_with_padding;

    return encode_base64_to_utf16_impl(input, options);
}

}
