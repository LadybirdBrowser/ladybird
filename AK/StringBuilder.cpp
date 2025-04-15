/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Liav A. <liavalb@hotmail.co.il>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Checked.h>
#include <AK/FlyString.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <AK/StringData.h>
#include <AK/StringView.h>
#include <AK/UnicodeUtils.h>
#include <AK/Utf16View.h>
#include <AK/Utf32View.h>

#include <simdutf.h>

namespace AK {

static constexpr auto STRING_BASE_PREFIX_SIZE = sizeof(Detail::StringData);

static ErrorOr<StringBuilder::Buffer> create_buffer(size_t capacity)
{
    StringBuilder::Buffer buffer;

    if (capacity > StringBuilder::inline_capacity)
        TRY(buffer.try_ensure_capacity(STRING_BASE_PREFIX_SIZE + capacity));

    TRY(buffer.try_resize(STRING_BASE_PREFIX_SIZE));
    return buffer;
}

ErrorOr<StringBuilder> StringBuilder::create(size_t initial_capacity)
{
    auto buffer = TRY(create_buffer(initial_capacity));
    return StringBuilder { move(buffer) };
}

StringBuilder::StringBuilder()
    : StringBuilder(inline_capacity)
{
}

StringBuilder::StringBuilder(size_t initial_capacity)
    : m_buffer(MUST(create_buffer(initial_capacity)))
{
}

StringBuilder::StringBuilder(Buffer buffer)
    : m_buffer(move(buffer))
{
}

inline ErrorOr<void> StringBuilder::will_append(size_t size)
{
    Checked<size_t> needed_capacity = m_buffer.size();
    needed_capacity += size;
    VERIFY(!needed_capacity.has_overflow());
    // Prefer to completely use the existing capacity first
    if (needed_capacity <= m_buffer.capacity())
        return {};
    Checked<size_t> expanded_capacity = needed_capacity;
    expanded_capacity *= 2;
    VERIFY(!expanded_capacity.has_overflow());
    TRY(m_buffer.try_ensure_capacity(expanded_capacity.value()));
    return {};
}

size_t StringBuilder::length() const
{
    return m_buffer.size() - STRING_BASE_PREFIX_SIZE;
}

bool StringBuilder::is_empty() const
{
    return length() == 0;
}

void StringBuilder::trim(size_t count)
{
    auto decrease_count = min(m_buffer.size(), count);
    m_buffer.resize(m_buffer.size() - decrease_count);
}

ErrorOr<void> StringBuilder::try_append(StringView string)
{
    if (string.is_empty())
        return {};
    TRY(will_append(string.length()));
    TRY(m_buffer.try_append(string.characters_without_null_termination(), string.length()));
    return {};
}

ErrorOr<void> StringBuilder::try_append(char ch)
{
    TRY(will_append(1));
    TRY(m_buffer.try_append(ch));
    return {};
}

ErrorOr<void> StringBuilder::try_append_repeated(char ch, size_t n)
{
    TRY(will_append(n));
    for (size_t i = 0; i < n; ++i)
        TRY(try_append(ch));
    return {};
}

ErrorOr<void> StringBuilder::try_append_repeated(StringView string, size_t n)
{
    if (string.is_empty())
        return {};
    TRY(will_append(string.length() * n));
    for (size_t i = 0; i < n; ++i)
        TRY(try_append(string));
    return {};
}

void StringBuilder::append(StringView string)
{
    MUST(try_append(string));
}

ErrorOr<void> StringBuilder::try_append(char const* characters, size_t length)
{
    return try_append(StringView { characters, length });
}

void StringBuilder::append(char const* characters, size_t length)
{
    MUST(try_append(characters, length));
}

void StringBuilder::append(char ch)
{
    MUST(try_append(ch));
}

void StringBuilder::append_repeated(char ch, size_t n)
{
    MUST(try_append_repeated(ch, n));
}

void StringBuilder::append_repeated(StringView string, size_t n)
{
    MUST(try_append_repeated(string, n));
}

ErrorOr<ByteBuffer> StringBuilder::to_byte_buffer() const
{
    return ByteBuffer::copy(data(), length());
}

ByteString StringBuilder::to_byte_string() const
{
    if (is_empty())
        return ByteString::empty();
    return ByteString((char const*)data(), length());
}

ErrorOr<String> StringBuilder::to_string()
{
    if (m_buffer.is_inline())
        return String::from_utf8(string_view());
    return String::from_string_builder({}, *this);
}

String StringBuilder::to_string_without_validation()
{
    if (m_buffer.is_inline())
        return String::from_utf8_without_validation(string_view().bytes());
    return String::from_string_builder_without_validation({}, *this);
}

FlyString StringBuilder::to_fly_string_without_validation() const
{
    return FlyString::from_utf8_without_validation(string_view().bytes());
}

ErrorOr<FlyString> StringBuilder::to_fly_string() const
{
    return FlyString::from_utf8(string_view());
}

u8* StringBuilder::data()
{
    return m_buffer.data() + STRING_BASE_PREFIX_SIZE;
}

u8 const* StringBuilder::data() const
{
    return m_buffer.data() + STRING_BASE_PREFIX_SIZE;
}

StringView StringBuilder::string_view() const
{
    return m_buffer.span().slice(STRING_BASE_PREFIX_SIZE);
}

void StringBuilder::clear()
{
    m_buffer.resize(STRING_BASE_PREFIX_SIZE);
}

ErrorOr<void> StringBuilder::try_append_code_point(u32 code_point)
{
    auto nwritten = TRY(AK::UnicodeUtils::try_code_point_to_utf8(code_point, [this](char c) { return try_append(c); }));
    if (nwritten < 0) {
        TRY(try_append(0xef));
        TRY(try_append(0xbf));
        TRY(try_append(0xbd));
    }
    return {};
}

void StringBuilder::append_code_point(u32 code_point)
{
    if (code_point <= 0x7f) {
        m_buffer.append(static_cast<char>(code_point));
    } else if (code_point <= 0x07ff) {
        (void)will_append(2);
        m_buffer.append(static_cast<char>((((code_point >> 6) & 0x1f) | 0xc0)));
        m_buffer.append(static_cast<char>((((code_point >> 0) & 0x3f) | 0x80)));
    } else if (code_point <= 0xffff) {
        (void)will_append(3);
        m_buffer.append(static_cast<char>((((code_point >> 12) & 0x0f) | 0xe0)));
        m_buffer.append(static_cast<char>((((code_point >> 6) & 0x3f) | 0x80)));
        m_buffer.append(static_cast<char>((((code_point >> 0) & 0x3f) | 0x80)));
    } else if (code_point <= 0x10ffff) {
        (void)will_append(4);
        m_buffer.append(static_cast<char>((((code_point >> 18) & 0x07) | 0xf0)));
        m_buffer.append(static_cast<char>((((code_point >> 12) & 0x3f) | 0x80)));
        m_buffer.append(static_cast<char>((((code_point >> 6) & 0x3f) | 0x80)));
        m_buffer.append(static_cast<char>((((code_point >> 0) & 0x3f) | 0x80)));
    } else {
        (void)will_append(3);
        m_buffer.append(0xef);
        m_buffer.append(0xbf);
        m_buffer.append(0xbd);
    }
}

ErrorOr<void> StringBuilder::try_append(Utf16View const& utf16_view)
{
    if (utf16_view.is_empty())
        return {};

    auto maximum_utf8_length = UnicodeUtils::maximum_utf8_length_from_utf16(utf16_view.span());

    // Possibly over-allocate a little to ensure we don't have to allocate later.
    TRY(will_append(maximum_utf8_length));

    Utf16View remaining_view = utf16_view;
    for (;;) {
        auto uninitialized_data_pointer = static_cast<char*>(m_buffer.end_pointer());

        // Fast path.
        auto result = simdutf::convert_utf16_to_utf8_with_errors(remaining_view.char_data(), remaining_view.length_in_code_units(), uninitialized_data_pointer);
        if (result.error == simdutf::SUCCESS) {
            auto bytes_just_written = result.count;
            m_buffer.set_size(m_buffer.size() + bytes_just_written);
            break;
        }

        // Slow path. Found unmatched surrogate code unit.
        auto first_invalid_code_unit = result.count;
        ASSERT(first_invalid_code_unit < remaining_view.length_in_code_units());

        // Unfortunately, `simdutf` does not tell us how many bytes it just wrote in case of an error, so we have to calculate it ourselves.
        auto bytes_just_written = simdutf::utf8_length_from_utf16(remaining_view.char_data(), first_invalid_code_unit);

        do {
            auto code_unit = remaining_view.code_unit_at(first_invalid_code_unit++);

            // Invalid surrogate code units are U+D800 - U+DFFF, so they are always encoded using 3 bytes.
            ASSERT(code_unit >= 0xD800 && code_unit <= 0xDFFF);
            ASSERT(m_buffer.size() + bytes_just_written + 3 < m_buffer.capacity());
            uninitialized_data_pointer[bytes_just_written++] = (((code_unit >> 12) & 0x0f) | 0xe0);
            uninitialized_data_pointer[bytes_just_written++] = (((code_unit >> 6) & 0x3f) | 0x80);
            uninitialized_data_pointer[bytes_just_written++] = (((code_unit >> 0) & 0x3f) | 0x80);
        } while (first_invalid_code_unit < remaining_view.length_in_code_units() && Utf16View::is_low_surrogate(remaining_view.data()[first_invalid_code_unit]));

        // Code unit might no longer be invalid, retry on the remaining data.
        m_buffer.set_size(m_buffer.size() + bytes_just_written);
        remaining_view = remaining_view.substring_view(first_invalid_code_unit);
    }

    return {};
}

void StringBuilder::append(Utf16View const& utf16_view)
{
    MUST(try_append(utf16_view));
}

ErrorOr<void> StringBuilder::try_append(Utf32View const& utf32_view)
{
    for (size_t i = 0; i < utf32_view.length(); ++i) {
        auto code_point = utf32_view.code_points()[i];
        TRY(try_append_code_point(code_point));
    }
    return {};
}

void StringBuilder::append(Utf32View const& utf32_view)
{
    MUST(try_append(utf32_view));
}

void StringBuilder::append_as_lowercase(char ch)
{
    if (ch >= 'A' && ch <= 'Z')
        append(ch + 0x20);
    else
        append(ch);
}

void StringBuilder::append_escaped_for_json(StringView string)
{
    MUST(try_append_escaped_for_json(string));
}

ErrorOr<void> StringBuilder::try_append_escaped_for_json(StringView string)
{
    for (auto ch : string) {
        switch (ch) {
        case '\b':
            TRY(try_append("\\b"sv));
            break;
        case '\n':
            TRY(try_append("\\n"sv));
            break;
        case '\t':
            TRY(try_append("\\t"sv));
            break;
        case '\"':
            TRY(try_append("\\\""sv));
            break;
        case '\\':
            TRY(try_append("\\\\"sv));
            break;
        default:
            if (ch >= 0 && ch <= 0x1f)
                TRY(try_appendff("\\u{:04x}", ch));
            else
                TRY(try_append(ch));
        }
    }
    return {};
}

auto StringBuilder::leak_buffer_for_string_construction(Badge<Detail::StringData>) -> Optional<Buffer::OutlineBuffer>
{
    if (auto buffer = m_buffer.leak_outline_buffer({}); buffer.has_value()) {
        clear();
        return buffer;
    }

    return {};
}

}
