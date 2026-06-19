/*
 * Copyright (c) 2024, Ben Jilks <benjyjilks@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Error.h>
#include <AK/StringBuilder.h>
#include <AK/Utf8View.h>
#include <LibTextCodec/Decoder.h>
#include <LibTextCodec/Encoder.h>
#include <RustFFI.h>

namespace TextCodec {

namespace {

class RustEncoder final : public Encoder {
public:
    explicit RustEncoder(StringView encoding)
        : m_encoding(encoding)
    {
    }

    virtual ErrorOr<void> process(Utf8View input, Function<ErrorOr<void>(u8)> on_byte, Function<ErrorOr<void>(u32)> on_error) override;

private:
    StringView m_encoding;
};

class UTF8Encoder final : public Encoder {
public:
    virtual ErrorOr<void> process(Utf8View, Function<ErrorOr<void>(u8)> on_byte, Function<ErrorOr<void>(u32)> on_error) override;
};

UTF8Encoder s_utf8_encoder;

RustEncoder s_gb18030_encoder { "gb18030"sv };
RustEncoder s_gbk_encoder { "GBK"sv };
RustEncoder s_big5_encoder { "Big5"sv };
RustEncoder s_euc_jp_encoder { "EUC-JP"sv };
RustEncoder s_iso_2022_jp_encoder { "ISO-2022-JP"sv };
RustEncoder s_shift_jis_encoder { "Shift_JIS"sv };
RustEncoder s_euc_kr_encoder { "EUC-KR"sv };
RustEncoder s_ibm866_encoder { "IBM866"sv };
RustEncoder s_latin2_encoder { "ISO-8859-2"sv };
RustEncoder s_latin3_encoder { "ISO-8859-3"sv };
RustEncoder s_latin4_encoder { "ISO-8859-4"sv };
RustEncoder s_latin_cyrillic_encoder { "ISO-8859-5"sv };
RustEncoder s_latin_arabic_encoder { "ISO-8859-6"sv };
RustEncoder s_latin_greek_encoder { "ISO-8859-7"sv };
RustEncoder s_latin_hebrew_encoder { "ISO-8859-8"sv };
RustEncoder s_latin6_encoder { "ISO-8859-10"sv };
RustEncoder s_latin7_encoder { "ISO-8859-13"sv };
RustEncoder s_latin8_encoder { "ISO-8859-14"sv };
RustEncoder s_latin9_encoder { "ISO-8859-15"sv };
RustEncoder s_latin10_encoder { "ISO-8859-16"sv };
RustEncoder s_centraleurope_encoder { "windows-1250"sv };
RustEncoder s_cyrillic_encoder { "windows-1251"sv };
RustEncoder s_hebrew_encoder { "windows-1255"sv };
RustEncoder s_koi8r_encoder { "KOI8-R"sv };
RustEncoder s_koi8u_encoder { "KOI8-U"sv };
RustEncoder s_mac_roman_encoder { "macintosh"sv };
RustEncoder s_windows874_encoder { "windows-874"sv };
RustEncoder s_windows1252_encoder { "windows-1252"sv };
RustEncoder s_windows1253_encoder { "windows-1253"sv };
RustEncoder s_turkish_encoder { "windows-1254"sv };
RustEncoder s_windows1256_encoder { "windows-1256"sv };
RustEncoder s_windows1257_encoder { "windows-1257"sv };
RustEncoder s_windows1258_encoder { "windows-1258"sv };
RustEncoder s_mac_cyrillic_encoder { "x-mac-cyrillic"sv };

struct EncodeContext {
    Function<ErrorOr<void>(u8)>* on_byte { nullptr };
    Function<ErrorOr<void>(u32)>* on_error { nullptr };
    ErrorOr<void> result {};
};

static void append_encoded_bytes(void* context, u8 const* data, size_t length)
{
    auto& encode_context = *static_cast<EncodeContext*>(context);
    if (encode_context.result.is_error())
        return;

    for (size_t i = 0; i < length; ++i) {
        encode_context.result = (*encode_context.on_byte)(data[i]);
        if (encode_context.result.is_error())
            return;
    }
}

static void report_unmappable_code_point(void* context, u32 code_point)
{
    auto& encode_context = *static_cast<EncodeContext*>(context);
    if (encode_context.result.is_error())
        return;

    encode_context.result = (*encode_context.on_error)(code_point);
}

ErrorOr<void> rust_encode(StringView encoding, Utf8View input, Function<ErrorOr<void>(u8)> on_byte, Function<ErrorOr<void>(u32)> on_error)
{
    auto input_view = StringView { input.bytes(), input.byte_length() };
    EncodeContext context {
        .on_byte = &on_byte,
        .on_error = &on_error,
    };
    auto succeeded = FFI::textcodec_rust_encode_from_utf8(
        reinterpret_cast<u8 const*>(encoding.characters_without_null_termination()),
        encoding.length(),
        reinterpret_cast<u8 const*>(input_view.characters_without_null_termination()),
        input_view.length(),
        &context,
        append_encoded_bytes,
        report_unmappable_code_point);
    if (!succeeded)
        return Error::from_errno(EINVAL);
    TRY(context.result);
    return {};
}

}

Optional<Encoder&> encoder_for_exact_name(StringView encoding)
{
    if (encoding.equals_ignoring_ascii_case("utf-8"sv))
        return s_utf8_encoder;
    if (encoding.equals_ignoring_ascii_case("big5"sv))
        return s_big5_encoder;
    if (encoding.equals_ignoring_ascii_case("euc-jp"sv))
        return s_euc_jp_encoder;
    if (encoding.equals_ignoring_ascii_case("iso-2022-jp"sv))
        return s_iso_2022_jp_encoder;
    if (encoding.equals_ignoring_ascii_case("shift_jis"sv))
        return s_shift_jis_encoder;
    if (encoding.equals_ignoring_ascii_case("euc-kr"sv))
        return s_euc_kr_encoder;
    if (encoding.equals_ignoring_ascii_case("gb18030"sv))
        return s_gb18030_encoder;
    if (encoding.equals_ignoring_ascii_case("gbk"sv))
        return s_gbk_encoder;
    if (encoding.equals_ignoring_ascii_case("ibm866"sv))
        return s_ibm866_encoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-2"sv))
        return s_latin2_encoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-3"sv))
        return s_latin3_encoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-4"sv))
        return s_latin4_encoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-5"sv))
        return s_latin_cyrillic_encoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-6"sv))
        return s_latin_arabic_encoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-7"sv))
        return s_latin_greek_encoder;
    if (encoding.is_one_of_ignoring_ascii_case("iso-8859-8"sv, "iso-8859-8-i"sv))
        return s_latin_hebrew_encoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-10"sv))
        return s_latin6_encoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-13"sv))
        return s_latin7_encoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-14"sv))
        return s_latin8_encoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-15"sv))
        return s_latin9_encoder;
    if (encoding.equals_ignoring_ascii_case("iso-8859-16"sv))
        return s_latin10_encoder;
    if (encoding.equals_ignoring_ascii_case("koi8-r"sv))
        return s_koi8r_encoder;
    if (encoding.equals_ignoring_ascii_case("koi8-u"sv))
        return s_koi8u_encoder;
    if (encoding.equals_ignoring_ascii_case("macintosh"sv))
        return s_mac_roman_encoder;
    if (encoding.equals_ignoring_ascii_case("windows-874"sv))
        return s_windows874_encoder;
    if (encoding.equals_ignoring_ascii_case("windows-1250"sv))
        return s_centraleurope_encoder;
    if (encoding.equals_ignoring_ascii_case("windows-1251"sv))
        return s_cyrillic_encoder;
    if (encoding.equals_ignoring_ascii_case("windows-1252"sv))
        return s_windows1252_encoder;
    if (encoding.equals_ignoring_ascii_case("windows-1253"sv))
        return s_windows1253_encoder;
    if (encoding.equals_ignoring_ascii_case("windows-1254"sv))
        return s_turkish_encoder;
    if (encoding.equals_ignoring_ascii_case("windows-1255"sv))
        return s_hebrew_encoder;
    if (encoding.equals_ignoring_ascii_case("windows-1256"sv))
        return s_windows1256_encoder;
    if (encoding.equals_ignoring_ascii_case("windows-1257"sv))
        return s_windows1257_encoder;
    if (encoding.equals_ignoring_ascii_case("windows-1258"sv))
        return s_windows1258_encoder;
    if (encoding.equals_ignoring_ascii_case("x-mac-cyrillic"sv))
        return s_mac_cyrillic_encoder;
    dbgln("TextCodec: No encoder implemented for encoding '{}'", encoding);
    return {};
}

Optional<Encoder&> encoder_for(StringView label)
{
    auto encoding = get_standardized_encoding(label);
    return encoding.has_value() ? encoder_for_exact_name(encoding.value()) : Optional<Encoder&> {};
}

ErrorOr<void> RustEncoder::process(Utf8View input, Function<ErrorOr<void>(u8)> on_byte, Function<ErrorOr<void>(u32)> on_error)
{
    return rust_encode(m_encoding, input, move(on_byte), move(on_error));
}

ErrorOr<void> UTF8Encoder::process(Utf8View input, Function<ErrorOr<void>(u8)> on_byte, Function<ErrorOr<void>(u32)>)
{
    ReadonlyBytes bytes { input.bytes(), input.byte_length() };
    for (auto byte : bytes)
        TRY(on_byte(byte));
    return {};
}

// https://infra.spec.whatwg.org/#isomorphic-encode
ByteString isomorphic_encode(StringView input)
{
    // To isomorphic encode an isomorphic string input: return a byte sequence whose length is equal to input’s code
    // point length and whose bytes have the same values as the values of input’s code points, in the same order.
    // NB: This is essentially spec-speak for "Encode as ISO-8859-1 / Latin-1".
    StringBuilder builder(input.length());

    for (auto code_point : Utf8View { input }) {
        // VERIFY(code_point <= 0xFF);
        if (code_point > 0xFF)
            dbgln("FIXME: Trying to isomorphic encode a string with code points > U+00FF.");

        builder.append(static_cast<u8>(code_point));
    }

    return builder.to_byte_string();
}

}
