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
#include <AK/Utf16String.h>
#include <AK/Utf16StringData.h>
#include <AK/Utf16View.h>
#include <AK/Utf32View.h>

#include <simdutf.h>

namespace AK {

static constexpr size_t string_builder_prefix_size(StringBuilder::Mode mode)
{
    switch (mode) {
    case StringBuilder::Mode::UTF8:
        return sizeof(Detail::StringData);
    case StringBuilder::Mode::UTF16:
        return Detail::Utf16StringData::offset_of_string_storage();
    }
    VERIFY_NOT_REACHED();
}

static ErrorOr<StringBuilder::Buffer> create_buffer(StringBuilder::Mode mode, size_t capacity)
{
    StringBuilder::Buffer buffer;
    auto prefix_size = string_builder_prefix_size(mode);

    if (capacity > StringBuilder::inline_capacity)
        TRY(buffer.try_ensure_capacity(prefix_size + capacity));

    TRY(buffer.try_resize(prefix_size));
    return buffer;
}

ErrorOr<StringBuilder> StringBuilder::create(size_t initial_capacity)
{
    auto buffer = TRY(create_buffer(DEFAULT_MODE, initial_capacity));
    return StringBuilder { move(buffer), DEFAULT_MODE };
}

StringBuilder::StringBuilder()
{
    static constexpr auto prefix_size = string_builder_prefix_size(DEFAULT_MODE);
    static_assert(inline_capacity > prefix_size);

    m_buffer.resize(prefix_size);
}

StringBuilder::StringBuilder(size_t initial_capacity)
    : m_buffer(MUST(create_buffer(DEFAULT_MODE, initial_capacity)))
{
}

StringBuilder::StringBuilder(Mode mode)
    : m_buffer(MUST(create_buffer(mode, inline_capacity)))
    , m_mode(mode)
{
}

StringBuilder::StringBuilder(Mode mode, size_t initial_capacity_in_code_units)
    : m_buffer(MUST(create_buffer(mode, initial_capacity_in_code_units * (mode == Mode::UTF8 ? 1 : 2))))
    , m_mode(mode)
{
}

StringBuilder::StringBuilder(Buffer buffer, Mode mode)
    : m_buffer(move(buffer))
    , m_mode(mode)
{
}

inline ErrorOr<void> StringBuilder::will_append(size_t size_in_bytes)
{
    Checked<size_t> needed_capacity = m_buffer.size();
    needed_capacity += size_in_bytes;
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

ErrorOr<void> StringBuilder::ensure_storage_is_utf16()
{
    if (!exchange(m_utf16_builder_is_ascii, false))
        return {};
    if (is_empty())
        return {};

    auto ascii_length = this->length();
    TRY(m_buffer.try_resize(m_buffer.size() + ascii_length));

    Bytes source { data(), ascii_length };
    Span<char16_t> target { reinterpret_cast<char16_t*>(data()), ascii_length };

    for (size_t i = ascii_length; i > 0; --i) {
        auto index = i - 1;

        auto ch = static_cast<char16_t>(source[index]);
        target.overwrite(index, &ch, sizeof(char16_t));
    }

    return {};
}

size_t StringBuilder::length() const
{
    return m_buffer.size() - string_builder_prefix_size(m_mode);
}

bool StringBuilder::is_empty() const
{
    return length() == 0;
}

void StringBuilder::trim(size_t count)
{
    if (m_mode == Mode::UTF16)
        count *= 2;

    auto decrease_count = min(m_buffer.size(), count);
    m_buffer.resize(m_buffer.size() - decrease_count);
}

ErrorOr<void> StringBuilder::try_append(StringView string)
{
    if (string.is_empty())
        return {};

    if (m_mode == Mode::UTF8 || (m_utf16_builder_is_ascii && string.is_ascii())) {
        TRY(will_append(string.length()));
        TRY(m_buffer.try_append(string.characters_without_null_termination(), string.length()));
    } else {
        TRY(ensure_storage_is_utf16());

        TRY(will_append(string.length() * 2));
        for (auto code_point : Utf8View { string })
            TRY(try_append_code_point(code_point));
    }

    return {};
}

void StringBuilder::append_ascii_without_validation(ReadonlyBytes string)
{
    MUST(try_append_ascii_without_validation(string));
}

ErrorOr<void> StringBuilder::try_append_ascii_without_validation(ReadonlyBytes string)
{
    if (string.is_empty())
        return {};

    if (m_mode == Mode::UTF8 || m_utf16_builder_is_ascii) {
        TRY(m_buffer.try_append(string));
    } else {
        if (m_mode == Mode::UTF16) {
            TRY(ensure_storage_is_utf16());
            TRY(will_append(string.size() * 2));
        } else {
            TRY(will_append(string.size()));
        }
        for (auto code_point : Utf8View { string })
            TRY(try_append_code_point(code_point));
    }

    return {};
}

ErrorOr<void> StringBuilder::try_append(char ch)
{
    if (m_mode == Mode::UTF8 || (m_utf16_builder_is_ascii && is_ascii(ch))) {
        TRY(will_append(1));
        TRY(m_buffer.try_append(ch));
    } else {
        TRY(ensure_storage_is_utf16());
        TRY(try_append_code_unit(ch));
    }

    return {};
}

ErrorOr<void> StringBuilder::try_append_code_unit(char16_t ch)
{
    if (m_mode == Mode::UTF8 || (m_utf16_builder_is_ascii && is_ascii(ch))) {
        TRY(try_append_code_point(ch));
    } else {
        TRY(ensure_storage_is_utf16());
        TRY(will_append(2));
        TRY(m_buffer.try_append(&ch, sizeof(ch)));
    }

    return {};
}

ErrorOr<void> StringBuilder::try_append_repeated(char ch, size_t n)
{
    auto append_as_utf8 = m_mode == Mode::UTF8 || (m_utf16_builder_is_ascii && is_ascii(ch));
    TRY(will_append(n * (append_as_utf8 ? 1 : 2)));

    for (size_t i = 0; i < n; ++i)
        TRY(try_append(ch));

    return {};
}

ErrorOr<void> StringBuilder::try_append_repeated(StringView string, size_t n)
{
    if (string.is_empty())
        return {};

    if (m_mode == Mode::UTF8 || (m_utf16_builder_is_ascii && string.is_ascii())) {
        TRY(will_append(string.length() * n));
    } else {
        auto utf16_length = simdutf::utf16_length_from_utf8(string.characters_without_null_termination(), string.length());
        TRY(will_append(utf16_length * n * 2));
    }

    for (size_t i = 0; i < n; ++i)
        TRY(try_append(string));

    return {};
}

ErrorOr<void> StringBuilder::try_append_repeated(Utf16View const& string, size_t n)
{
    if (string.is_empty())
        return {};

    if (m_mode == Mode::UTF8 || (m_utf16_builder_is_ascii && string.is_ascii())) {
        if (string.has_ascii_storage()) {
            TRY(will_append(string.length_in_code_units() * n));
        } else {
            auto utf8_length = simdutf::utf8_length_from_utf16(string.utf16_span().data(), string.length_in_code_units());
            TRY(will_append(utf8_length * n));
        }
    } else {
        TRY(will_append(string.length_in_code_units() * n * 2));
    }

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

void StringBuilder::append_code_unit(char16_t ch)
{
    MUST(try_append_code_unit(ch));
}

void StringBuilder::append_repeated(char ch, size_t n)
{
    MUST(try_append_repeated(ch, n));
}

void StringBuilder::append_repeated(StringView string, size_t n)
{
    MUST(try_append_repeated(string, n));
}

void StringBuilder::append_repeated(Utf16View const& string, size_t n)
{
    MUST(try_append_repeated(string, n));
}

ErrorOr<ByteBuffer> StringBuilder::to_byte_buffer() const
{
    return ByteBuffer::copy(data(), length());
}

ByteString StringBuilder::to_byte_string() const
{
    VERIFY(m_mode == Mode::UTF8);
    if (is_empty())
        return ByteString::empty();
    return ByteString((char const*)data(), length());
}

ErrorOr<String> StringBuilder::to_string()
{
    VERIFY(m_mode == Mode::UTF8);
    if (m_buffer.is_inline())
        return String::from_utf8(string_view());
    return String::from_string_builder({}, *this);
}

String StringBuilder::to_string_without_validation()
{
    VERIFY(m_mode == Mode::UTF8);
    if (m_buffer.is_inline())
        return String::from_utf8_without_validation(string_view().bytes());
    return String::from_string_builder_without_validation({}, *this);
}

FlyString StringBuilder::to_fly_string_without_validation() const
{
    VERIFY(m_mode == Mode::UTF8);
    return FlyString::from_utf8_without_validation(string_view().bytes());
}

ErrorOr<FlyString> StringBuilder::to_fly_string() const
{
    VERIFY(m_mode == Mode::UTF8);
    return FlyString::from_utf8(string_view());
}

Utf16String StringBuilder::to_utf16_string()
{
    VERIFY(m_mode == Mode::UTF16);
    return Utf16String::from_string_builder({}, *this);
}

u8* StringBuilder::data()
{
    return m_buffer.data() + string_builder_prefix_size(m_mode);
}

u8 const* StringBuilder::data() const
{
    return m_buffer.data() + string_builder_prefix_size(m_mode);
}

StringView StringBuilder::string_view() const
{
    VERIFY(m_mode == Mode::UTF8);
    return m_buffer.span().slice(string_builder_prefix_size(m_mode));
}

Utf16View StringBuilder::utf16_string_view() const
{
    VERIFY(m_mode == Mode::UTF16);
    auto view = m_buffer.span().slice(string_builder_prefix_size(m_mode));

    if (m_utf16_builder_is_ascii)
        return { reinterpret_cast<char const*>(view.data()), view.size() };
    return { reinterpret_cast<char16_t const*>(view.data()), view.size() / 2 };
}

void StringBuilder::clear()
{
    m_buffer.resize(string_builder_prefix_size(m_mode));
}

ErrorOr<void> StringBuilder::try_append_code_point(u32 code_point)
{
    if (!is_unicode(code_point)) {
        TRY(try_append_code_point(UnicodeUtils::REPLACEMENT_CODE_POINT));
        return {};
    }

    if (m_mode == Mode::UTF8 || (m_utf16_builder_is_ascii && is_ascii(code_point))) {
        TRY(AK::UnicodeUtils::try_code_point_to_utf8(code_point, [this](char c) { return try_append(c); }));
    } else {
        TRY(ensure_storage_is_utf16());

        TRY(AK::UnicodeUtils::try_code_point_to_utf16(code_point, [this](char16_t c) { return m_buffer.try_append(&c, sizeof(c)); }));
    }

    return {};
}

void StringBuilder::append_code_point(u32 code_point)
{
    if (!is_unicode(code_point)) {
        append_code_point(UnicodeUtils::REPLACEMENT_CODE_POINT);
        return;
    }

    auto append_as_utf8 = m_mode == Mode::UTF8 || (m_utf16_builder_is_ascii && is_ascii(code_point));

    if (!append_as_utf8) {
        MUST(ensure_storage_is_utf16());
        (void)(will_append(2));

        if (code_point < UnicodeUtils::FIRST_SUPPLEMENTARY_PLANE_CODE_POINT) {
            auto code_unit = static_cast<char16_t>(code_point);
            m_buffer.append(&code_unit, sizeof(code_unit));
            return;
        }

        (void)(will_append(2));
        code_point -= UnicodeUtils::FIRST_SUPPLEMENTARY_PLANE_CODE_POINT;

        auto code_unit = static_cast<u16>(UnicodeUtils::HIGH_SURROGATE_MIN | (code_point >> 10));
        m_buffer.append(&code_unit, sizeof(code_unit));

        code_unit = static_cast<u16>(UnicodeUtils::LOW_SURROGATE_MIN | (code_point & 0x3ff));
        m_buffer.append(&code_unit, sizeof(code_unit));

        return;
    }

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
    } else {
        (void)will_append(4);
        m_buffer.append(static_cast<char>((((code_point >> 18) & 0x07) | 0xf0)));
        m_buffer.append(static_cast<char>((((code_point >> 12) & 0x3f) | 0x80)));
        m_buffer.append(static_cast<char>((((code_point >> 6) & 0x3f) | 0x80)));
        m_buffer.append(static_cast<char>((((code_point >> 0) & 0x3f) | 0x80)));
    }
}

ErrorOr<void> StringBuilder::try_append(Utf16View const& utf16_view)
{
    if (utf16_view.is_empty())
        return {};
    if (utf16_view.has_ascii_storage())
        return try_append_ascii_without_validation(utf16_view.bytes());

    auto append_as_utf8 = m_mode == Mode::UTF8 || (m_utf16_builder_is_ascii && utf16_view.is_ascii());

    if (!append_as_utf8) {
        TRY(ensure_storage_is_utf16());
        TRY(will_append(utf16_view.length_in_code_units() * 2));

        for (size_t i = 0; i < utf16_view.length_in_code_units(); ++i)
            TRY(try_append_code_unit(utf16_view.code_unit_at(i)));

        return {};
    }

    auto remaining_view = utf16_view.utf16_span();
    auto maximum_utf8_length = UnicodeUtils::maximum_utf8_length_from_utf16(remaining_view);

    // Possibly over-allocate a little to ensure we don't have to allocate later.
    TRY(will_append(maximum_utf8_length));

    for (;;) {
        auto* uninitialized_data_pointer = static_cast<char*>(m_buffer.end_pointer());

        // Fast path.
        auto result = simdutf::convert_utf16_to_utf8_with_errors(remaining_view.data(), remaining_view.size(), uninitialized_data_pointer);
        if (result.error == simdutf::SUCCESS) {
            auto bytes_just_written = result.count;
            m_buffer.set_size(m_buffer.size() + bytes_just_written);
            break;
        }

        // Slow path. Found unmatched surrogate code unit.
        auto first_invalid_code_unit = result.count;
        ASSERT(first_invalid_code_unit < remaining_view.size());

        // Unfortunately, `simdutf` does not tell us how many bytes it just wrote in case of an error, so we have to calculate it ourselves.
        auto bytes_just_written = simdutf::utf8_length_from_utf16(remaining_view.data(), first_invalid_code_unit);

        do {
            auto code_unit = remaining_view[first_invalid_code_unit++];

            // Invalid surrogate code units are U+D800 - U+DFFF, so they are always encoded using 3 bytes.
            ASSERT(code_unit >= 0xD800 && code_unit <= 0xDFFF);
            ASSERT(m_buffer.size() + bytes_just_written + 3 < m_buffer.capacity());
            uninitialized_data_pointer[bytes_just_written++] = (((code_unit >> 12) & 0x0f) | 0xe0);
            uninitialized_data_pointer[bytes_just_written++] = (((code_unit >> 6) & 0x3f) | 0x80);
            uninitialized_data_pointer[bytes_just_written++] = (((code_unit >> 0) & 0x3f) | 0x80);
        } while (first_invalid_code_unit < remaining_view.size() && UnicodeUtils::is_utf16_low_surrogate(remaining_view.data()[first_invalid_code_unit]));

        // Code unit might no longer be invalid, retry on the remaining data.
        m_buffer.set_size(m_buffer.size() + bytes_just_written);
        remaining_view = remaining_view.slice(first_invalid_code_unit);
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

auto StringBuilder::leak_buffer_for_string_construction() -> Optional<Buffer::OutlineBuffer>
{
    if (auto buffer = m_buffer.leak_outline_buffer({}); buffer.has_value()) {
        clear();
        return buffer;
    }

    return {};
}

}
