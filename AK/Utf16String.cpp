/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/RefPtr.h>
#include <AK/Stream.h>
#include <AK/StringNumber.h>
#include <AK/Utf16String.h>
#include <AK/Utf16StringBuilder.h>

#include <simdutf.h>

namespace AK {

static_assert(sizeof(Detail::ShortString) == sizeof(Detail::Utf16StringData*));

Utf16String Utf16String::from_utf8_with_replacement_character(StringView utf8_string, WithBOMHandling with_bom_handling)
{
    if (auto bytes = utf8_string.bytes(); with_bom_handling == WithBOMHandling::Yes && bytes.starts_with({ { 0xEF, 0xBB, 0xBF } }))
        utf8_string = utf8_string.substring_view(3);

    if (utf8_string.length() <= Detail::MAX_SHORT_STRING_BYTE_COUNT && utf8_string.is_ascii())
        return from_utf8_without_validation(utf8_string);

    return Utf16String { Detail::Utf16StringData::from_utf8_with_replacement_character(utf8_string) };
}

Utf16String Utf16String::from_wtf8_with_replacement_character(StringView wtf8_string)
{
    // NB: This cannot use simdutf's replacement, which turns each byte of an encoded lone surrogate into U+FFFD. WTF-8
    //     decoding replaces the whole surrogate with a single U+FFFD.
    if (auto string = from_well_formed_utf8(wtf8_string); string.has_value())
        return string.release_value();

    Utf8View utf8_view { wtf8_string };

    Utf16StringBuilder builder;
    utf8_view.for_each_code_point_with_replacement_character(AllowLonelySurrogates::Yes, [&](u32 code_point) {
        builder.append_code_point(code_point);
    });

    return builder.to_string();
}

Utf16String Utf16String::from_utf8(StringView utf8_string)
{
    return MUST(try_from_utf8(utf8_string));
}

ErrorOr<Utf16String> Utf16String::try_from_utf8(StringView utf8_string)
{
    // NB: Converting well-formed UTF-8 already validates it, so only the remaining input (WTF-8 with lone surrogates)
    //     needs a separate validation pass.
    if (auto string = from_well_formed_utf8(utf8_string); string.has_value())
        return string.release_value();

    Utf8View utf8_view { utf8_string };
    if (!utf8_view.validate())
        return Error::from_string_literal("Input was not valid UTF-8");

    // NB: Utf16StringData::from_utf8() would try the conversion that just failed again before decoding code points.
    return Utf16String { Detail::Utf16StringData::create_from_code_point_iterable(utf8_view) };
}

Optional<Utf16String> Utf16String::from_well_formed_utf8(StringView utf8_string)
{
    if (utf8_string.length() <= Detail::MAX_SHORT_STRING_BYTE_COUNT && utf8_string.is_ascii())
        return from_utf8_without_validation(utf8_string);

    if (auto string = Detail::Utf16StringData::from_well_formed_utf8(utf8_string))
        return Utf16String { string.release_nonnull() };
    return {};
}

Utf16String Utf16String::from_ascii_without_validation(ReadonlyBytes ascii_string)
{
    if (ascii_string.size() <= Detail::MAX_SHORT_STRING_BYTE_COUNT) {
        Utf16String string;
        string.m_value.short_ascii_string = Detail::ShortString::create_with_byte_count(ascii_string.size());

        auto result = ascii_string.copy_to(string.m_value.short_ascii_string.storage);
        VERIFY(result == ascii_string.size());

        return string;
    }

    return Utf16String { Detail::Utf16StringData::from_ascii(ascii_string) };
}

Utf16String Utf16String::from_utf8_without_validation(StringView utf8_string)
{
    if (utf8_string.length() <= Detail::MAX_SHORT_STRING_BYTE_COUNT && utf8_string.is_ascii()) {
        Utf16String string;
        string.m_value.short_ascii_string = Detail::ShortString::create_with_byte_count(utf8_string.length());

        auto result = utf8_string.bytes().copy_to(string.m_value.short_ascii_string.storage);
        VERIFY(result == utf8_string.length());

        return string;
    }

    return Utf16String { Detail::Utf16StringData::from_utf8(utf8_string) };
}

Utf16String Utf16String::from_utf16(Utf16View const& utf16_string)
{
    if (utf16_string.length_in_code_units() <= Detail::MAX_SHORT_STRING_BYTE_COUNT && utf16_string.is_ascii()) {
        Utf16String string;
        string.m_value.short_ascii_string = Detail::ShortString::create_with_byte_count(utf16_string.length_in_code_units());

        if (utf16_string.has_latin1_storage()) {
            auto result = utf16_string.latin1_span().copy_to(string.m_value.short_ascii_string.storage);
            VERIFY(result == utf16_string.length_in_code_units());
        } else {
            auto result = simdutf::convert_utf16_to_utf8(utf16_string.utf16_span().data(), utf16_string.length_in_code_units(), reinterpret_cast<char*>(string.m_value.short_ascii_string.storage));
            VERIFY(result == utf16_string.length_in_code_units());
        }

        return string;
    }

    return Utf16String { Detail::Utf16StringData::from_utf16(utf16_string) };
}

Utf16String Utf16String::from_string_builder(Badge<Utf16StringBuilder>, Utf16StringBuilder& builder)
{
    auto view = builder.view();

    if (view.length_in_code_units() <= Detail::MAX_SHORT_STRING_BYTE_COUNT && view.has_latin1_storage() && view.is_ascii()) {
        Utf16String string;
        string.m_value.short_ascii_string = Detail::ShortString::create_with_byte_count(view.length_in_code_units());

        auto result = view.ascii_bytes().copy_to(string.m_value.short_ascii_string.storage);
        VERIFY(result == view.length_in_code_units());

        return string;
    }

    return Utf16String { Detail::Utf16StringData::from_string_builder(builder) };
}

ErrorOr<Utf16String> Utf16String::from_ipc_stream(Stream& stream, size_t length_in_code_units, bool has_latin1_storage)
{
    if (has_latin1_storage && length_in_code_units <= Detail::MAX_SHORT_STRING_BYTE_COUNT) {
        Array<u8, Detail::MAX_SHORT_STRING_BYTE_COUNT> buffer;
        auto bytes = buffer.span().trim(length_in_code_units);
        TRY(stream.read_until_filled(bytes));

        // NB: Only ASCII is stored in a short string.
        return from_utf16(Utf16View::from_latin1(bytes, false));
    }

    return Utf16String { TRY(Detail::Utf16StringData::from_ipc_stream(stream, length_in_code_units, has_latin1_storage)) };
}

Utf16String Utf16String::repeated(u32 code_point, size_t count)
{
    if (count <= Detail::MAX_SHORT_STRING_BYTE_COUNT && AK::is_ascii(code_point)) {
        Utf16String string;
        string.m_value.short_ascii_string = Detail::ShortString::create_with_byte_count(count);

        Bytes bytes { string.m_value.short_ascii_string.storage, count };
        bytes.fill(static_cast<u8>(code_point));

        return string;
    }

    Array<char16_t, 2> code_units;
    size_t length_in_code_units = 0;

    (void)UnicodeUtils::code_point_to_utf16(code_point, [&](auto code_unit) {
        code_units[length_in_code_units++] = code_unit;
    });

    Utf16StringBuilder builder;
    builder.append_repeated({ code_units.data(), length_in_code_units }, count);
    return builder.to_string();
}

Utf16String Utf16String::to_well_formed() const
{
    if (utf16_view().validate())
        return *this;
    return Utf16String { Detail::Utf16StringData::to_well_formed(*this) };
}

String Utf16String::to_well_formed_utf8() const
{
    return MUST(String::from_utf16_with_replacement_character(utf16_view()));
}

ErrorOr<void> Formatter<Utf16String>::format(FormatBuilder& builder, Utf16String const& utf16_string)
{
    // One-byte storage holds Latin-1, which is UTF-8 only when it's all ASCII.
    if (utf16_string.has_ascii_storage())
        return builder.put_string(utf16_string.ascii_view());
    return builder.put_string(utf16_string.utf16_view());
}

template<Integral T>
Utf16String Utf16String::number(T value)
{
    return create_string_from_number<Utf16String, T>(value);
}

template Utf16String Utf16String::number(char);
template Utf16String Utf16String::number(signed char);
template Utf16String Utf16String::number(unsigned char);
template Utf16String Utf16String::number(signed short);
template Utf16String Utf16String::number(unsigned short);
template Utf16String Utf16String::number(int);
template Utf16String Utf16String::number(unsigned int);
template Utf16String Utf16String::number(long);
template Utf16String Utf16String::number(unsigned long);
template Utf16String Utf16String::number(long long);
template Utf16String Utf16String::number(unsigned long long);

}
