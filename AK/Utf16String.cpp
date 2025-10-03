/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Stream.h>
#include <AK/StringNumber.h>
#include <AK/Utf16String.h>
#include <AK/Utf32View.h>

#include <simdutf.h>

namespace AK {

static_assert(sizeof(Detail::ShortString) == sizeof(Detail::Utf16StringData*));

Utf16String Utf16String::from_utf8_with_replacement_character(StringView utf8_string, WithBOMHandling with_bom_handling)
{
    if (auto bytes = utf8_string.bytes(); with_bom_handling == WithBOMHandling::Yes && bytes.starts_with({ { 0xEF, 0xBB, 0xBF } }))
        utf8_string = utf8_string.substring_view(3);

    Utf8View utf8_view { utf8_string };

    if (utf8_view.validate(AllowLonelySurrogates::No))
        return Utf16String::from_utf8_without_validation(utf8_string);

    StringBuilder builder(StringBuilder::Mode::UTF16);

    for (auto code_point : utf8_view) {
        if (is_unicode_surrogate(code_point))
            builder.append_code_point(UnicodeUtils::REPLACEMENT_CODE_POINT);
        else
            builder.append_code_point(code_point);
    }

    return builder.to_utf16_string();
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

    return Utf16String { Detail::Utf16StringData::from_utf8(utf8_string, Detail::Utf16StringData::AllowASCIIStorage::Yes) };
}

Utf16String Utf16String::from_utf16(Utf16View const& utf16_string)
{
    if (utf16_string.length_in_code_units() <= Detail::MAX_SHORT_STRING_BYTE_COUNT && utf16_string.is_ascii()) {
        Utf16String string;
        string.m_value.short_ascii_string = Detail::ShortString::create_with_byte_count(utf16_string.length_in_code_units());

        if (utf16_string.has_ascii_storage()) {
            auto result = utf16_string.bytes().copy_to(string.m_value.short_ascii_string.storage);
            VERIFY(result == utf16_string.length_in_code_units());
        } else {
            auto result = simdutf::convert_utf16_to_utf8(utf16_string.utf16_span().data(), utf16_string.length_in_code_units(), reinterpret_cast<char*>(string.m_value.short_ascii_string.storage));
            VERIFY(result == utf16_string.length_in_code_units());
        }

        return string;
    }

    return Utf16String { Detail::Utf16StringData::from_utf16(utf16_string) };
}

Utf16String Utf16String::from_utf32(Utf32View const& utf32_string)
{
    if (utf32_string.length() <= Detail::MAX_SHORT_STRING_BYTE_COUNT && utf32_string.is_ascii()) {
        Utf16String string;
        string.m_value.short_ascii_string = Detail::ShortString::create_with_byte_count(utf32_string.length());

        auto result = simdutf::convert_utf32_to_utf8(reinterpret_cast<char32_t const*>(utf32_string.code_points()), utf32_string.length(), reinterpret_cast<char*>(string.m_value.short_ascii_string.storage));
        VERIFY(result == utf32_string.length());

        return string;
    }

    return Utf16String { Detail::Utf16StringData::from_utf32(utf32_string) };
}

Utf16String Utf16String::from_string_builder(Badge<StringBuilder>, StringBuilder& builder)
{
    auto view = builder.utf16_string_view();

    if (view.length_in_code_units() <= Detail::MAX_SHORT_STRING_BYTE_COUNT && view.has_ascii_storage()) {
        Utf16String string;
        string.m_value.short_ascii_string = Detail::ShortString::create_with_byte_count(view.length_in_code_units());

        auto result = view.bytes().copy_to(string.m_value.short_ascii_string.storage);
        VERIFY(result == view.length_in_code_units());

        return string;
    }

    return Utf16String { Detail::Utf16StringData::from_string_builder(builder) };
}

ErrorOr<Utf16String> Utf16String::from_ipc_stream(Stream& stream, size_t length_in_code_units, bool is_ascii)
{
    if (is_ascii && length_in_code_units <= Detail::MAX_SHORT_STRING_BYTE_COUNT) {
        Utf16String string;
        string.m_value.short_ascii_string = Detail::ShortString::create_with_byte_count(length_in_code_units);

        Bytes bytes { string.m_value.short_ascii_string.storage, length_in_code_units };
        TRY(stream.read_until_filled(bytes));

        if (!StringView { bytes }.is_ascii())
            return Error::from_string_literal("Stream contains invalid ASCII data");

        return string;
    }

    return Utf16String { TRY(Detail::Utf16StringData::from_ipc_stream(stream, length_in_code_units, is_ascii)) };
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

    StringBuilder builder(StringBuilder::Mode::UTF16);
    builder.append_repeated({ code_units.data(), length_in_code_units }, count);
    return builder.to_utf16_string();
}

Utf16String Utf16String::to_well_formed() const
{
    if (utf16_view().validate())
        return *this;
    return Utf16String { Detail::Utf16StringData::to_well_formed(*this) };
}

String Utf16String::to_well_formed_utf8() const
{
    if (utf16_view().validate())
        return to_utf8(AllowLonelySurrogates::No);
    return to_well_formed().to_utf8(AllowLonelySurrogates::No);
}

ErrorOr<void> Formatter<Utf16String>::format(FormatBuilder& builder, Utf16String const& utf16_string)
{
    if (utf16_string.has_long_utf16_storage())
        return builder.builder().try_append(utf16_string.utf16_view());
    return builder.put_string(utf16_string.ascii_view());
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
