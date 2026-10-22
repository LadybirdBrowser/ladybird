/*
 * Copyright (c) 2024, Ben Jilks <benjyjilks@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Error.h>
#include <AK/StringBuilder.h>
#include <AK/Utf16View.h>
#include <AK/Utf8View.h>
#include <LibTextCodec/Decoder.h>
#include <LibTextCodec/Encoder.h>
#include <RustFFI.h>

namespace TextCodec {

namespace {

struct EncodeContext {
    Function<ErrorOr<void>(ReadonlyBytes)>& on_bytes;
    Function<ErrorOr<void>(u32)>& on_error;
    ErrorOr<void> result {};
};

void append_encoded_bytes(void* context, u8 const* data, size_t length)
{
    auto& encode_context = *static_cast<EncodeContext*>(context);
    if (encode_context.result.is_error())
        return;
    encode_context.result = encode_context.on_bytes({ data, length });
}

void report_unmappable_code_point(void* context, u32 code_point)
{
    auto& encode_context = *static_cast<EncodeContext*>(context);
    if (encode_context.result.is_error())
        return;
    encode_context.result = encode_context.on_error(code_point);
}

}

Optional<Encoder> encoder_for(StringView label)
{
    auto encoding = get_standardized_encoding(label);
    return encoding.has_value() ? Encoder { get_output_encoding(*encoding) } : Optional<Encoder> {};
}

ErrorOr<void> Encoder::process(Utf8View input, Function<ErrorOr<void>(ReadonlyBytes)> on_bytes, Function<ErrorOr<void>(u32)> on_error) const
{
    if (m_output_encoding == "UTF-8"sv)
        return on_bytes({ input.bytes(), input.byte_length() });

    EncodeContext context { .on_bytes = on_bytes, .on_error = on_error };
    auto succeeded = FFI::textcodec_rust_encode_from_utf8(
        reinterpret_cast<u8 const*>(m_output_encoding.characters_without_null_termination()),
        m_output_encoding.length(),
        input.bytes(),
        input.byte_length(),
        &context,
        append_encoded_bytes,
        report_unmappable_code_point);
    if (!succeeded)
        return Error::from_errno(EINVAL);
    return move(context.result);
}

// https://infra.spec.whatwg.org/#isomorphic-encode
ByteString isomorphic_encode(StringView input)
{
    // To isomorphic encode an isomorphic string input: return a byte sequence whose length is equal to input’s code
    // point length and whose bytes have the same values as the values of input’s code points, in the same order.
    // NB: This is essentially spec-speak for "Encode as ISO-8859-1 / Latin-1".
    StringBuilder builder(input.length());

    for (auto code_point : Utf8View { input }) {
        VERIFY(code_point <= 0xFF);
        builder.append(static_cast<u8>(code_point));
    }

    return builder.to_byte_string();
}

// https://infra.spec.whatwg.org/#isomorphic-encode
ByteString isomorphic_encode(Utf16View input)
{
    // To isomorphic encode an isomorphic string input: return a byte sequence whose length is equal to input’s code
    // point length and whose bytes have the same values as the values of input’s code points, in the same order.
    // NB: This is essentially spec-speak for "Encode as ISO-8859-1 / Latin-1".
    StringBuilder builder(input.length_in_code_units());

    for (auto code_point : input) {
        VERIFY(code_point <= 0xFF);
        builder.append(static_cast<u8>(code_point));
    }

    return builder.to_byte_string();
}

}
