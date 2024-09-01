/*
 * Copyright (c) 2020-2022, Andreas Kling <kling@serenityos.org>
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

static ErrorOr<ByteBuffer> decode_base64_impl(StringView input, simdutf::base64_options options)
{
    ByteBuffer output;
    TRY(output.try_resize(size_required_to_decode_base64(input)));

    auto result = simdutf::base64_to_binary(
        input.characters_without_null_termination(),
        input.length(),
        reinterpret_cast<char*>(output.data()),
        options);

    if (result.error != simdutf::SUCCESS)
        return Error::from_string_literal("Invalid base64-encoded data");

    output.resize(result.count);
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
