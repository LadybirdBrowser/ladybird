/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16String.h>
#include <AK/Utf32View.h>

#include <simdutf.h>

namespace AK {

static_assert(sizeof(Detail::ShortString) == sizeof(Detail::Utf16StringData*));

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

Utf16String Utf16String::from_utf16_without_validation(Utf16View const& utf16_string)
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

ErrorOr<void> Formatter<Utf16String>::format(FormatBuilder& builder, Utf16String const& utf16_string)
{
    if (utf16_string.has_long_utf16_storage())
        return builder.builder().try_append(utf16_string.utf16_view());
    return builder.put_string(utf16_string.ascii_view());
}

}
