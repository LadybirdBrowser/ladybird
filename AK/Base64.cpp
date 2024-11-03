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

static ErrorOr<size_t, InvalidBase64> decode_base64_into_impl(StringView input, ByteBuffer& output, simdutf::base64_options options)
{
    size_t output_length = output.size();

    auto result = simdutf::base64_to_binary_safe(
        input.characters_without_null_termination(),
        input.length(),
        reinterpret_cast<char*>(output.data()),
        output_length,
        options);

    if (result.error != simdutf::SUCCESS && result.error != simdutf::OUTPUT_BUFFER_TOO_SMALL) {
        output.resize((result.count / 4) * 3);

        return InvalidBase64 {
            .error = Error::from_string_literal("Invalid base64-encoded data"),
            .valid_input_bytes = result.count,
        };
    }

    VERIFY(output_length <= output.size());
    output.resize(output_length);

    return result.error == simdutf::SUCCESS ? input.length() : result.count;
}

static ErrorOr<ByteBuffer> decode_base64_impl(StringView input, simdutf::base64_options options)
{
    ByteBuffer output;
    TRY(output.try_resize(size_required_to_decode_base64(input)));

    if (auto result = decode_base64_into_impl(input, output, options); result.is_error())
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

ErrorOr<ByteBuffer> decode_base64(StringView input)
{
    return decode_base64_impl(input, simdutf::base64_default);
}

ErrorOr<ByteBuffer> decode_base64url(StringView input)
{
    return decode_base64_impl(input, simdutf::base64_url);
}

ErrorOr<size_t, InvalidBase64> decode_base64_into(StringView input, ByteBuffer& output)
{
    return decode_base64_into_impl(input, output, simdutf::base64_default);
}

ErrorOr<size_t, InvalidBase64> decode_base64url_into(StringView input, ByteBuffer& output)
{
    return decode_base64_into_impl(input, output, simdutf::base64_url);
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
