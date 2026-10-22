/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2024, Simon Wanner <simon@skyrising.xyz>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <AK/Utf16StringBuilder.h>
#include <AK/Utf8View.h>
#include <LibTextCodec/Decoder.h>
#include <RustFFI.h>

namespace TextCodec {

namespace {

struct DecodeContext {
    StringBuilder builder;
    ErrorOr<void> result {};
};

struct Utf16DecodeContext {
    Utf16StringBuilder builder;
};

void append_decoded_bytes(void* context, u8 const* data, size_t length)
{
    auto& decode_context = *static_cast<DecodeContext*>(context);
    if (decode_context.result.is_error())
        return;
    decode_context.result = decode_context.builder.try_append(StringView { data, length });
}

void append_decoded_utf16(void* context, u16 const* data, size_t length)
{
    auto& decode_context = *static_cast<Utf16DecodeContext*>(context);
    decode_context.builder.append(Utf16View { reinterpret_cast<char16_t const*>(data), length });
}

ErrorOr<Utf16String> rust_streaming_decode_to_utf16(FFI::TextCodecRustStreamingDecoder* decoder, ReadonlyBytes input, bool last)
{
    Utf16DecodeContext context { .builder = Utf16StringBuilder(input.size()) };
    auto succeeded = FFI::textcodec_rust_streaming_decoder_decode_to_utf16(
        decoder,
        input.data(),
        input.size(),
        last,
        &context,
        append_decoded_utf16);
    if (!succeeded)
        return Error::from_string_literal("Failed to decode input");
    return context.builder.to_string();
}

}

Optional<Decoder> decoder_for(StringView label)
{
    auto encoding = get_standardized_encoding(label);
    return encoding.has_value() ? Decoder { *encoding } : Optional<Decoder> {};
}

Optional<Decoder> decoder_for(Utf16View label)
{
    auto encoding = get_standardized_encoding(label);
    return encoding.has_value() ? Decoder { *encoding } : Optional<Decoder> {};
}

Optional<StringView> get_standardized_encoding(StringView encoding)
{
    u8 const* encoding_name = nullptr;
    size_t encoding_name_length = 0;
    auto succeeded = FFI::textcodec_rust_get_standardized_encoding(
        reinterpret_cast<u8 const*>(encoding.characters_without_null_termination()),
        encoding.length(),
        &encoding_name,
        &encoding_name_length);
    if (!succeeded) {
        // NB: Pages often give an empty label, such as <script charset="">. That is not worth logging.
        if (!encoding.trim_whitespace().is_empty())
            dbgln("TextCodec: Unrecognized encoding: {}", encoding);
        return {};
    }
    return StringView { encoding_name, encoding_name_length };
}

Optional<StringView> get_standardized_encoding(Utf16View encoding)
{
    if (!encoding.is_ascii()) {
        dbgln("TextCodec: Unrecognized encoding: {}", encoding);
        return {};
    }

    if (encoding.has_ascii_storage()) {
        auto ascii = encoding.ascii_span();
        return get_standardized_encoding(StringView { ascii.data(), ascii.size() });
    }

    StringBuilder builder;
    for (auto code_point : encoding)
        builder.append(static_cast<char>(code_point));
    auto ascii_string = builder.to_string_without_validation();
    return get_standardized_encoding(ascii_string.bytes_as_string_view());
}

ErrorOr<String> Decoder::to_utf8(ReadonlyBytes input, ErrorMode error_mode) const
{
    DecodeContext context { .builder = StringBuilder(input.size()) };
    auto succeeded = FFI::textcodec_rust_decode_to_utf8(
        reinterpret_cast<u8 const*>(m_encoding.characters_without_null_termination()),
        m_encoding.length(),
        input.data(),
        input.size(),
        error_mode == ErrorMode::Fatal,
        &context,
        append_decoded_bytes);
    if (!succeeded)
        return Error::from_string_literal("Failed to decode input");
    TRY(context.result);
    return context.builder.to_string_without_validation();
}

ErrorOr<Utf16String> Decoder::to_utf16(ReadonlyBytes input) const
{
    if (m_encoding == "UTF-8"sv && Utf8View { StringView { input } }.validate(AllowLonelySurrogates::No))
        return Utf16String::from_utf8_without_validation(StringView { input });

    Utf16DecodeContext context { .builder = Utf16StringBuilder(input.size()) };
    auto succeeded = FFI::textcodec_rust_decode_to_utf16(
        reinterpret_cast<u8 const*>(m_encoding.characters_without_null_termination()),
        m_encoding.length(),
        input.data(),
        input.size(),
        &context,
        append_decoded_utf16);
    if (!succeeded)
        return Error::from_string_literal("Failed to decode input");
    return context.builder.to_string();
}

ErrorOr<size_t> Decoder::length_in_utf16_code_units(ReadonlyBytes input) const
{
    size_t length = 0;
    auto succeeded = FFI::textcodec_rust_decoded_length_in_utf16_code_units(
        reinterpret_cast<u8 const*>(m_encoding.characters_without_null_termination()),
        m_encoding.length(),
        input.data(),
        input.size(),
        &length);
    if (!succeeded)
        return Error::from_string_literal("Failed to decode input");
    return length;
}

// https://encoding.spec.whatwg.org/#bom-sniff
static Optional<StringView> bom_sniff(ReadonlyBytes input)
{
    // 1. Let BOM be the result of peeking 3 bytes from ioQueue, converted to a byte sequence.
    // 2. For each of the rows in the table below, starting with the first one and going down,
    //    if BOM starts with the bytes given in the first column, then return the encoding given
    //    in the cell in the second column of that row. Otherwise, return null.

    // Byte Order Mark | Encoding
    // --------------------------
    // 0xEF 0xBB 0xBF  | UTF-8
    // 0xFE 0xFF       | UTF-16BE
    // 0xFF 0xFE       | UTF-16LE
    auto bom = StringView { input };
    if (bom.starts_with("\xEF\xBB\xBF"sv))
        return "UTF-8"sv;
    if (bom.starts_with("\xFE\xFF"sv))
        return "UTF-16BE"sv;
    if (bom.starts_with("\xFF\xFE"sv))
        return "UTF-16LE"sv;
    return {};
}

// https://encoding.spec.whatwg.org/#decode
static Decoder decoder_for_decode(Decoder fallback_decoder, ReadonlyBytes& input)
{
    // 1. Let BOMEncoding be the result of BOM sniffing ioQueue.
    // 2. If BOMEncoding is non-null:
    if (auto bom_encoding = bom_sniff(input); bom_encoding.has_value()) {
        // 1. Set encoding to BOMEncoding.
        // 2. Read three bytes from ioQueue, if BOMEncoding is UTF-8; otherwise read two bytes. (Do nothing with those bytes.)
        input = input.slice(*bom_encoding == "UTF-8"sv ? 3 : 2);
        return *decoder_for(*bom_encoding);
    }
    return fallback_decoder;
}

// https://encoding.spec.whatwg.org/#decode
ErrorOr<String> decode_to_utf8(ReadonlyBytes input, Decoder fallback_decoder, ErrorMode error_mode)
{
    auto decoder = decoder_for_decode(fallback_decoder, input);

    // 3. Process a queue with an instance of encoding’s decoder, ioQueue, output, and "replacement".
    // 4. Return output.
    // NB: XML documents decode with the fatal error mode instead.
    return decoder.to_utf8(input, error_mode);
}

// https://encoding.spec.whatwg.org/#decode
ErrorOr<Utf16String> decode_to_utf16(ReadonlyBytes input, Decoder fallback_decoder)
{
    auto decoder = decoder_for_decode(fallback_decoder, input);
    return decoder.to_utf16(input);
}

// https://encoding.spec.whatwg.org/#decode
ErrorOr<size_t> decoded_length_in_utf16_code_units(ReadonlyBytes input, Decoder fallback_decoder)
{
    auto decoder = decoder_for_decode(fallback_decoder, input);
    return decoder.length_in_utf16_code_units(input);
}

// https://encoding.spec.whatwg.org/#utf-8-decode
ErrorOr<Utf16String> utf8_decode_to_utf16(ReadonlyBytes input)
{
    // 1. Let buffer be the result of peeking three bytes from ioQueue, converted to a byte sequence.
    // 2. If buffer is 0xEF 0xBB 0xBF, then read three bytes from ioQueue. (Do nothing with those bytes.)
    if (StringView { input }.starts_with("\xEF\xBB\xBF"sv))
        input = input.slice(3);

    // 3. Process a queue with an instance of UTF-8’s decoder, ioQueue, output, and "replacement".
    // 4. Return output.
    return decoder_for("UTF-8"sv)->to_utf16(input);
}

// https://encoding.spec.whatwg.org/#get-an-output-encoding
StringView get_output_encoding(StringView encoding)
{
    // 1. If encoding is replacement or UTF-16BE/LE, then return UTF-8.
    if (encoding.is_one_of_ignoring_ascii_case("replacement"sv, "utf-16le"sv, "utf-16be"sv))
        return "UTF-8"sv;

    // 2. Return encoding.
    return encoding;
}

StreamingDecoder::StreamingDecoder(StringView encoding, IgnoreBOM ignore_bom, ErrorMode error_mode)
{
    m_decoder = FFI::textcodec_rust_streaming_decoder_new(
        reinterpret_cast<u8 const*>(encoding.characters_without_null_termination()),
        encoding.length(),
        error_mode == ErrorMode::Fatal,
        ignore_bom == IgnoreBOM::Yes);
    VERIFY(m_decoder);
}

StreamingDecoder::~StreamingDecoder()
{
    FFI::textcodec_rust_streaming_decoder_free(static_cast<FFI::TextCodecRustStreamingDecoder*>(m_decoder));
}

ErrorOr<Utf16String> StreamingDecoder::decode(ReadonlyBytes input, Flush flush)
{
    return rust_streaming_decode_to_utf16(static_cast<FFI::TextCodecRustStreamingDecoder*>(m_decoder), input, flush == Flush::Yes);
}

// https://infra.spec.whatwg.org/#isomorphic-decode
String isomorphic_decode(StringView input)
{
    // To isomorphic decode a byte sequence input, return a string whose code point length is equal to input’s length
    // and whose code points have the same values as the values of input’s bytes, in the same order.
    // NB: This is essentially spec-speak for "Decode as ISO-8859-1 / Latin-1".
    StringBuilder builder(input.length());

    for (auto byte : input.bytes())
        builder.append_code_point(byte);

    return builder.to_string_without_validation();
}

// https://infra.spec.whatwg.org/#isomorphic-decode
Utf16String isomorphic_decode_to_utf16(StringView input)
{
    // To isomorphic decode a byte sequence input, return a string whose code point length is equal to input’s length
    // and whose code points have the same values as the values of input’s bytes, in the same order.
    // NB: This is essentially spec-speak for "Decode as ISO-8859-1 / Latin-1".
    Utf16StringBuilder builder(input.length());

    for (auto byte : input.bytes())
        builder.append_code_point(byte);

    return builder.to_string();
}

}
