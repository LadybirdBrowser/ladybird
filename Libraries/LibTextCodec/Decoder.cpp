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

class RustDecoder final : public Decoder {
public:
    explicit RustDecoder(StringView encoding)
        : m_encoding(encoding)
    {
    }

    virtual ErrorOr<String> to_utf8(StringView input, IgnoreBOM, ErrorMode) override;
    virtual ErrorOr<size_t> length_in_utf16_code_units(StringView input) override;

private:
    virtual ErrorOr<void> process(StringView input, Function<ErrorOr<void>(u32)> on_code_point) override;

    StringView m_encoding;
};

class UTF8Decoder final : public Decoder {
public:
    virtual ErrorOr<void> process(StringView, Function<ErrorOr<void>(u32)> on_code_point) override;
    virtual ErrorOr<String> to_utf8(StringView, IgnoreBOM, ErrorMode) override;
    virtual ErrorOr<size_t> length_in_utf16_code_units(StringView) override;
};

class UTF16BEDecoder final : public Decoder {
public:
    virtual ErrorOr<String> to_utf8(StringView, IgnoreBOM, ErrorMode) override;
    virtual ErrorOr<size_t> length_in_utf16_code_units(StringView) override;

private:
    virtual ErrorOr<void> process(StringView, Function<ErrorOr<void>(u32)>) override;
};

class UTF16LEDecoder final : public Decoder {
public:
    virtual ErrorOr<String> to_utf8(StringView, IgnoreBOM, ErrorMode) override;
    virtual ErrorOr<size_t> length_in_utf16_code_units(StringView) override;

private:
    virtual ErrorOr<void> process(StringView, Function<ErrorOr<void>(u32)>) override;
};

class Latin1Decoder final : public Decoder {
public:
    virtual ErrorOr<void> process(StringView, Function<ErrorOr<void>(u32)> on_code_point) override;
    virtual ErrorOr<size_t> length_in_utf16_code_units(StringView) override;
};

UTF8Decoder s_utf8_decoder;
UTF16BEDecoder s_utf16be_decoder;
UTF16LEDecoder s_utf16le_decoder;
Latin1Decoder s_latin1_decoder;

RustDecoder s_gb18030_decoder { "gb18030"sv };
RustDecoder s_big5_decoder { "Big5"sv };
RustDecoder s_euc_jp_decoder { "EUC-JP"sv };
RustDecoder s_iso_2022_jp_decoder { "ISO-2022-JP"sv };
RustDecoder s_shift_jis_decoder { "Shift_JIS"sv };
RustDecoder s_euc_kr_decoder { "EUC-KR"sv };
RustDecoder s_ibm866_decoder { "IBM866"sv };
RustDecoder s_latin2_decoder { "ISO-8859-2"sv };
RustDecoder s_latin3_decoder { "ISO-8859-3"sv };
RustDecoder s_latin4_decoder { "ISO-8859-4"sv };
RustDecoder s_latin_cyrillic_decoder { "ISO-8859-5"sv };
RustDecoder s_latin_arabic_decoder { "ISO-8859-6"sv };
RustDecoder s_latin_greek_decoder { "ISO-8859-7"sv };
RustDecoder s_latin_hebrew_decoder { "ISO-8859-8"sv };
RustDecoder s_latin6_decoder { "ISO-8859-10"sv };
RustDecoder s_latin7_decoder { "ISO-8859-13"sv };
RustDecoder s_latin8_decoder { "ISO-8859-14"sv };
RustDecoder s_latin9_decoder { "ISO-8859-15"sv };
RustDecoder s_latin10_decoder { "ISO-8859-16"sv };
RustDecoder s_centraleurope_decoder { "windows-1250"sv };
RustDecoder s_cyrillic_decoder { "windows-1251"sv };
RustDecoder s_hebrew_decoder { "windows-1255"sv };
RustDecoder s_koi8r_decoder { "KOI8-R"sv };
RustDecoder s_koi8u_decoder { "KOI8-U"sv };
RustDecoder s_mac_roman_decoder { "macintosh"sv };
RustDecoder s_windows874_decoder { "windows-874"sv };
RustDecoder s_windows1252_decoder { "windows-1252"sv };
RustDecoder s_windows1253_decoder { "windows-1253"sv };
RustDecoder s_turkish_decoder { "windows-1254"sv };
RustDecoder s_windows1256_decoder { "windows-1256"sv };
RustDecoder s_windows1257_decoder { "windows-1257"sv };
RustDecoder s_windows1258_decoder { "windows-1258"sv };
RustDecoder s_mac_cyrillic_decoder { "x-mac-cyrillic"sv };
RustDecoder s_x_user_defined_decoder { "x-user-defined"sv };
RustDecoder s_replacement_decoder { "replacement"sv };

struct DecodeContext {
    StringBuilder builder;
    ErrorOr<void> result {};
};

static void append_decoded_bytes(void* context, u8 const* data, size_t length)
{
    auto& decode_context = *static_cast<DecodeContext*>(context);
    if (decode_context.result.is_error())
        return;
    decode_context.result = decode_context.builder.try_append(StringView { data, length });
}

ErrorOr<String> rust_decode_to_utf8(StringView encoding, StringView input, IgnoreBOM ignore_bom, ErrorMode error_mode)
{
    DecodeContext context { .builder = StringBuilder(input.length()) };
    auto succeeded = FFI::textcodec_rust_decode_to_utf8(
        reinterpret_cast<u8 const*>(encoding.characters_without_null_termination()),
        encoding.length(),
        reinterpret_cast<u8 const*>(input.characters_without_null_termination()),
        input.length(),
        ignore_bom == IgnoreBOM::No,
        error_mode == ErrorMode::Fatal,
        &context,
        append_decoded_bytes);
    if (!succeeded)
        return Error::from_string_literal("Failed to decode input");
    TRY(context.result);
    return context.builder.to_string_without_validation();
}

ErrorOr<void> rust_process(StringView encoding, StringView input, IgnoreBOM ignore_bom, Function<ErrorOr<void>(u32)> on_code_point)
{
    auto utf8 = TRY(rust_decode_to_utf8(encoding, input, ignore_bom, ErrorMode::Replacement));
    for (auto code_point : Utf8View { utf8 })
        TRY(on_code_point(code_point));
    return {};
}

ErrorOr<size_t> rust_length_in_utf16_code_units(StringView encoding, StringView input, IgnoreBOM ignore_bom)
{
    auto utf8 = TRY(rust_decode_to_utf8(encoding, input, ignore_bom, ErrorMode::Replacement));
    size_t length = 0;
    for (auto code_point : Utf8View { utf8 })
        length += code_point <= 0xffff ? 1 : 2;
    return length;
}

Optional<StringView> get_static_encoding_name_from_rust(StringView label)
{
    u8 const* encoding_name = nullptr;
    size_t encoding_name_length = 0;
    auto succeeded = FFI::textcodec_rust_get_standardized_encoding(
        reinterpret_cast<u8 const*>(label.characters_without_null_termination()),
        label.length(),
        &encoding_name,
        &encoding_name_length);
    if (!succeeded)
        return {};
    return StringView { encoding_name, encoding_name_length };
}

ErrorOr<String> rust_streaming_decode_to_utf8(FFI::TextCodecRustStreamingDecoder* decoder, ReadonlyBytes input, bool last, ErrorMode error_mode)
{
    DecodeContext context { .builder = StringBuilder(input.size()) };
    auto succeeded = FFI::textcodec_rust_streaming_decoder_decode_to_utf8(
        decoder,
        input.data(),
        input.size(),
        last,
        error_mode == ErrorMode::Fatal,
        &context,
        append_decoded_bytes);
    if (!succeeded)
        return Error::from_string_literal("Failed to decode input");
    TRY(context.result);
    return context.builder.to_string_without_validation();
}

}

Optional<Decoder&> decoder_for(StringView label)
{
    auto encoding = get_standardized_encoding(label);
    return encoding.has_value() ? decoder_for_exact_name(encoding.value()) : Optional<Decoder&> {};
}

Optional<Decoder&> decoder_for_exact_name(StringView encoding)
{
    if (encoding.equals_ignoring_ascii_case("iso-8859-1"sv))
        return s_latin1_decoder;
    if (encoding.equals_ignoring_ascii_case("windows-1252"sv))
        return s_windows1252_decoder;
    if (encoding.equals_ignoring_ascii_case("utf-8"sv))
        return s_utf8_decoder;
    if (encoding.equals_ignoring_ascii_case("utf-16be"sv))
        return s_utf16be_decoder;
    if (encoding.equals_ignoring_ascii_case("utf-16le"sv))
        return s_utf16le_decoder;
    if (encoding.equals_ignoring_ascii_case("big5"sv))
        return s_big5_decoder;
    if (encoding.equals_ignoring_ascii_case("euc-jp"sv))
        return s_euc_jp_decoder;
    if (encoding.equals_ignoring_ascii_case("euc-kr"sv))
        return s_euc_kr_decoder;
    if (encoding.equals_ignoring_ascii_case("gbk"sv))
        return s_gb18030_decoder;
    if (encoding.equals_ignoring_ascii_case("gb18030"sv))
        return s_gb18030_decoder;
    if (encoding.equals_ignoring_ascii_case("ibm866"sv))
        return s_ibm866_decoder;
    if (encoding.equals_ignoring_ascii_case("iso-2022-jp"sv))
        return s_iso_2022_jp_decoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-2"sv))
        return s_latin2_decoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-3"sv))
        return s_latin3_decoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-4"sv))
        return s_latin4_decoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-5"sv))
        return s_latin_cyrillic_decoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-6"sv))
        return s_latin_arabic_decoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-7"sv))
        return s_latin_greek_decoder;
    if (encoding.is_one_of_ignoring_ascii_case("iso-8859-8"sv, "iso-8859-8-i"sv))
        return s_latin_hebrew_decoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-10"sv))
        return s_latin6_decoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-13"sv))
        return s_latin7_decoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-14"sv))
        return s_latin8_decoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-15"sv))
        return s_latin9_decoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-16"sv))
        return s_latin10_decoder;
    if (encoding.equals_ignoring_ascii_case("koi8-r"sv))
        return s_koi8r_decoder;
    if (encoding.equals_ignoring_ascii_case("koi8-u"sv))
        return s_koi8u_decoder;
    if (encoding.equals_ignoring_ascii_case("macintosh"sv))
        return s_mac_roman_decoder;
    if (encoding.equals_ignoring_ascii_case("replacement"sv))
        return s_replacement_decoder;
    if (encoding.equals_ignoring_ascii_case("shift_jis"sv))
        return s_shift_jis_decoder;
    if (encoding.equals_ignoring_ascii_case("windows-874"sv))
        return s_windows874_decoder;
    if (encoding.equals_ignoring_ascii_case("windows-1250"sv))
        return s_centraleurope_decoder;
    if (encoding.equals_ignoring_ascii_case("windows-1251"sv))
        return s_cyrillic_decoder;
    if (encoding.equals_ignoring_ascii_case("windows-1253"sv))
        return s_windows1253_decoder;
    if (encoding.equals_ignoring_ascii_case("windows-1254"sv))
        return s_turkish_decoder;
    if (encoding.equals_ignoring_ascii_case("windows-1255"sv))
        return s_hebrew_decoder;
    if (encoding.equals_ignoring_ascii_case("windows-1256"sv))
        return s_windows1256_decoder;
    if (encoding.equals_ignoring_ascii_case("windows-1257"sv))
        return s_windows1257_decoder;
    if (encoding.equals_ignoring_ascii_case("windows-1258"sv))
        return s_windows1258_decoder;
    if (encoding.equals_ignoring_ascii_case("x-mac-cyrillic"sv))
        return s_mac_cyrillic_decoder;
    if (encoding.equals_ignoring_ascii_case("x-user-defined"sv))
        return s_x_user_defined_decoder;

    dbgln("TextCodec: No decoder implemented for encoding '{}'", encoding);
    return {};
}

Optional<StringView> get_standardized_encoding(StringView encoding)
{
    auto standardized_encoding = get_static_encoding_name_from_rust(encoding);
    if (!standardized_encoding.has_value())
        dbgln("TextCodec: Unrecognized encoding: {}", encoding);
    return standardized_encoding;
}

// https://encoding.spec.whatwg.org/#bom-sniff
Optional<Decoder&> bom_sniff_to_decoder(StringView input)
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

    auto bytes = input.bytes();
    if (bytes.size() < 2)
        return {};

    auto first_byte = bytes[0];

    switch (first_byte) {
    case 0xEF: // UTF-8
        if (bytes.size() < 3)
            return {};
        if (bytes[1] == 0xBB && bytes[2] == 0xBF)
            return s_utf8_decoder;
        return {};
    case 0xFE: // UTF-16BE
        if (bytes[1] == 0xFF)
            return s_utf16be_decoder;
        return {};
    case 0xFF: // UTF-16LE
        if (bytes[1] == 0xFE)
            return s_utf16le_decoder;
        return {};
    }

    return {};
}

// https://encoding.spec.whatwg.org/#decode
ErrorOr<String> convert_input_to_utf8_using_given_decoder_unless_there_is_a_byte_order_mark(Decoder& fallback_decoder, StringView input)
{
    Decoder* actual_decoder = &fallback_decoder;

    // 1. Let BOMEncoding be the result of BOM sniffing ioQueue.
    // 2. If BOMEncoding is non-null:
    if (auto unicode_decoder = bom_sniff_to_decoder(input); unicode_decoder.has_value()) {
        // 1. Set encoding to BOMEncoding.
        actual_decoder = &unicode_decoder.value();

        // 2. Read three bytes from ioQueue, if BOMEncoding is UTF-8; otherwise read two bytes. (Do nothing with those bytes.)
        // FIXME: I imagine this will be pretty slow for large inputs, as it's regenerating the input without the first 2/3 bytes.
        input = input.substring_view(&unicode_decoder.value() == &s_utf8_decoder ? 3 : 2);
    }

    VERIFY(actual_decoder);

    // 3. Process a queue with an instance of encoding’s decoder, ioQueue, output, and "replacement".
    // FIXME: This isn't the exact same as the spec, which is written in terms of I/O queues.
    auto output = TRY(actual_decoder->to_utf8(input, IgnoreBOM::No, ErrorMode::Replacement));

    // 4. Return output.
    return output;
}

ErrorOr<Utf16String> convert_input_to_utf16_using_given_decoder_unless_there_is_a_byte_order_mark(Decoder& fallback_decoder, StringView input)
{
    Decoder* actual_decoder = &fallback_decoder;

    if (auto unicode_decoder = bom_sniff_to_decoder(input); unicode_decoder.has_value()) {
        actual_decoder = &unicode_decoder.value();
        input = input.substring_view(&unicode_decoder.value() == &s_utf8_decoder ? 3 : 2);
    }

    VERIFY(actual_decoder);
    return actual_decoder->to_utf16(input);
}

ErrorOr<size_t> convert_input_to_utf16_length_using_given_decoder_unless_there_is_a_byte_order_mark(Decoder& fallback_decoder, StringView input)
{
    Decoder* actual_decoder = &fallback_decoder;

    if (auto unicode_decoder = bom_sniff_to_decoder(input); unicode_decoder.has_value()) {
        actual_decoder = &unicode_decoder.value();
        input = input.substring_view(&unicode_decoder.value() == &s_utf8_decoder ? 3 : 2);
    }

    VERIFY(actual_decoder);
    return actual_decoder->length_in_utf16_code_units(input);
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

ErrorOr<String> Decoder::to_utf8(StringView input, IgnoreBOM, ErrorMode)
{
    StringBuilder builder(input.length());
    TRY(process(input, [&builder](u32 c) { return builder.try_append_code_point(c); }));
    return builder.to_string_without_validation();
}

ErrorOr<Utf16String> Decoder::to_utf16(StringView input)
{
    Utf16StringBuilder builder;
    TRY(process(input, [&builder](u32 c) -> ErrorOr<void> {
        builder.append_code_point(c);
        return {};
    }));
    return builder.to_string();
}

ErrorOr<size_t> Decoder::length_in_utf16_code_units(StringView input)
{
    size_t length = 0;
    TRY(process(input, [&](u32 code_point) -> ErrorOr<void> {
        length += code_point <= 0xffff ? 1 : 2;
        return {};
    }));
    return length;
}

ErrorOr<void> Decoder::process_code_points(StringView input, Function<ErrorOr<void>(u32)> on_code_point)
{
    return process(input, move(on_code_point));
}

ErrorOr<String> RustDecoder::to_utf8(StringView input, IgnoreBOM ignore_bom, ErrorMode error_mode)
{
    return rust_decode_to_utf8(m_encoding, input, ignore_bom, error_mode);
}

ErrorOr<size_t> RustDecoder::length_in_utf16_code_units(StringView input)
{
    return rust_length_in_utf16_code_units(m_encoding, input, IgnoreBOM::Yes);
}

ErrorOr<void> RustDecoder::process(StringView input, Function<ErrorOr<void>(u32)> on_code_point)
{
    return rust_process(m_encoding, input, IgnoreBOM::Yes, move(on_code_point));
}

ErrorOr<void> Latin1Decoder::process(StringView input, Function<ErrorOr<void>(u32)> on_code_point)
{
    for (u8 ch : input)
        TRY(on_code_point(ch));
    return {};
}

ErrorOr<size_t> Latin1Decoder::length_in_utf16_code_units(StringView input)
{
    return input.length();
}

StreamingDecoder::StreamingDecoder(StringView encoding, IgnoreBOM ignore_bom, ErrorMode error_mode)
    : m_error_mode(error_mode)
{
    m_decoder = FFI::textcodec_rust_streaming_decoder_new(
        reinterpret_cast<u8 const*>(encoding.characters_without_null_termination()),
        encoding.length(),
        ignore_bom == IgnoreBOM::No);
    VERIFY(m_decoder);
}

StreamingDecoder::~StreamingDecoder()
{
    FFI::textcodec_rust_streaming_decoder_free(static_cast<FFI::TextCodecRustStreamingDecoder*>(m_decoder));
}

ErrorOr<String> StreamingDecoder::to_utf8(ReadonlyBytes input)
{
    return rust_streaming_decode_to_utf8(static_cast<FFI::TextCodecRustStreamingDecoder*>(m_decoder), input, false, m_error_mode);
}

ErrorOr<String> StreamingDecoder::finish()
{
    return rust_streaming_decode_to_utf8(static_cast<FFI::TextCodecRustStreamingDecoder*>(m_decoder), {}, true, m_error_mode);
}

ErrorOr<void> UTF8Decoder::process(StringView input, Function<ErrorOr<void>(u32)> on_code_point)
{
    return rust_process("UTF-8"sv, input, IgnoreBOM::Yes, move(on_code_point));
}

ErrorOr<String> UTF8Decoder::to_utf8(StringView input, IgnoreBOM ignore_bom, ErrorMode error_mode)
{
    return rust_decode_to_utf8("UTF-8"sv, input, ignore_bom, error_mode);
}

ErrorOr<size_t> UTF8Decoder::length_in_utf16_code_units(StringView input)
{
    return rust_length_in_utf16_code_units("UTF-8"sv, input, IgnoreBOM::No);
}

ErrorOr<void> UTF16BEDecoder::process(StringView input, Function<ErrorOr<void>(u32)> on_code_point)
{
    return rust_process("UTF-16BE"sv, input, IgnoreBOM::No, move(on_code_point));
}

ErrorOr<String> UTF16BEDecoder::to_utf8(StringView input, IgnoreBOM ignore_bom, ErrorMode error_mode)
{
    return rust_decode_to_utf8("UTF-16BE"sv, input, ignore_bom, error_mode);
}

ErrorOr<size_t> UTF16BEDecoder::length_in_utf16_code_units(StringView input)
{
    return rust_length_in_utf16_code_units("UTF-16BE"sv, input, IgnoreBOM::No);
}

ErrorOr<void> UTF16LEDecoder::process(StringView input, Function<ErrorOr<void>(u32)> on_code_point)
{
    return rust_process("UTF-16LE"sv, input, IgnoreBOM::No, move(on_code_point));
}

ErrorOr<String> UTF16LEDecoder::to_utf8(StringView input, IgnoreBOM ignore_bom, ErrorMode error_mode)
{
    return rust_decode_to_utf8("UTF-16LE"sv, input, ignore_bom, error_mode);
}

ErrorOr<size_t> UTF16LEDecoder::length_in_utf16_code_units(StringView input)
{
    return rust_length_in_utf16_code_units("UTF-16LE"sv, input, IgnoreBOM::No);
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

}
