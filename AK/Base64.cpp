/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/Types.h>
#include <AK/Vector.h>

#include <simdutf.h>

namespace AK {

size_t size_required_to_decode_base64(StringView input)
{
    return simdutf::maximal_binary_length_from_base64(input.characters_without_null_termination(), input.length());
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

static ErrorOr<size_t, InvalidBase64> decode_base64_into_impl(StringView input, ByteBuffer& output, LastChunkHandling last_chunk_handling, simdutf::base64_options options)
{
    static constexpr auto decode_up_to_bad_character = true;
    auto output_length = output.size();

    auto result = simdutf::base64_to_binary_safe(
        input.characters_without_null_termination(),
        input.length(),
        reinterpret_cast<char*>(output.data()),
        output_length,
        options,
        to_simdutf_last_chunk_handling(last_chunk_handling),
        decode_up_to_bad_character);

    if (result.error != simdutf::SUCCESS && result.error != simdutf::OUTPUT_BUFFER_TOO_SMALL) {
        output.resize((result.count / 4) * 3);

        auto error = [&]() {
            switch (result.error) {
            case simdutf::BASE64_EXTRA_BITS:
                return Error::from_string_literal("Extra bits found at end of chunk");
            case simdutf::BASE64_INPUT_REMAINDER:
                return Error::from_string_literal("Invalid trailing data");
            case simdutf::INVALID_BASE64_CHARACTER:
                return Error::from_string_literal("Invalid base64 character");
            default:
                return Error::from_string_literal("Invalid base64-encoded data");
            }
        }();

        return InvalidBase64 { .error = move(error), .valid_input_bytes = result.count };
    }

    VERIFY(output_length <= output.size());
    output.resize(output_length);

    return result.count;
}

static ErrorOr<ByteBuffer> decode_base64_impl(StringView input, LastChunkHandling last_chunk_handling, simdutf::base64_options options)
{
    ByteBuffer output;
    TRY(output.try_resize(size_required_to_decode_base64(input)));

    if (auto result = decode_base64_into_impl(input, output, last_chunk_handling, options); result.is_error())
        return result.release_error().error;

    return output;
}

static ErrorOr<String> encode_base64_impl(StringView input, simdutf::base64_options options)
{
    Vector<u8> output;
    TRY(output.try_resize(simdutf::base64_length_from_binary(input.length(), options)));

    simdutf::binary_to_base64(
        input.characters_without_null_termination(),
        input.length(),
        reinterpret_cast<char*>(output.data()),
        options);

    return String::from_utf8_without_validation(output);
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

}
