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
#include <AK/kmalloc.h>

#include <simdutf.h>

namespace AK {

StringBuilder::Buffer::Buffer(Buffer const& other)
{
    MUST(try_resize(other.size()));
    if (other.size() != 0)
        __builtin_memcpy(data(), other.data(), other.size());
}

StringBuilder::Buffer::Buffer(Buffer&& other)
{
    move_from(move(other));
}

StringBuilder::Buffer::~Buffer()
{
    clear();
}

auto StringBuilder::Buffer::operator=(Buffer const& other) -> Buffer&
{
    if (this != &other) {
        if (m_size > other.size())
            trim(other.size(), true);
        else
            MUST(try_resize(other.size()));

        if (other.size() != 0)
            __builtin_memcpy(data(), other.data(), other.size());
    }

    return *this;
}

auto StringBuilder::Buffer::operator=(Buffer&& other) -> Buffer&
{
    if (this != &other) {
        clear();
        move_from(move(other));
    }

    return *this;
}

u8* StringBuilder::Buffer::data()
{
    return m_inline ? m_inline_buffer : m_outline_buffer;
}

u8 const* StringBuilder::Buffer::data() const
{
    return m_inline ? m_inline_buffer : m_outline_buffer;
}

Bytes StringBuilder::Buffer::span()
{
    return { data(), size() };
}

ReadonlyBytes StringBuilder::Buffer::span() const
{
    return { data(), size() };
}

void* StringBuilder::Buffer::end_pointer()
{
    return data() + m_size;
}

void StringBuilder::Buffer::clear()
{
    if (!m_inline) {
        kfree(m_outline_buffer);
        m_inline = true;
    }
    m_size = 0;
}

void StringBuilder::Buffer::resize(size_t new_size)
{
    MUST(try_resize(new_size));
}

void StringBuilder::Buffer::set_size(size_t new_size)
{
    ASSERT(new_size <= capacity());
    m_size = new_size;
}

void StringBuilder::Buffer::ensure_capacity(size_t new_capacity)
{
    MUST(try_ensure_capacity(new_capacity));
}

ErrorOr<void> StringBuilder::Buffer::try_resize(size_t new_size)
{
    if (new_size <= m_size) {
        trim(new_size, false);
        return {};
    }

    TRY(try_ensure_capacity(new_size));
    set_size(new_size);
    return {};
}

ErrorOr<void> StringBuilder::Buffer::try_ensure_capacity(size_t new_capacity)
{
    if (new_capacity <= capacity())
        return {};
    return try_ensure_capacity_slowpath(new_capacity);
}

ErrorOr<void> StringBuilder::Buffer::try_append(char byte)
{
    auto old_size = size();
    Checked<size_t> new_size = old_size;
    new_size += 1;
    VERIFY(!new_size.has_overflow());

    TRY(try_resize(new_size.value()));
    data()[old_size] = static_cast<u8>(byte);
    return {};
}

ErrorOr<void> StringBuilder::Buffer::try_append(ReadonlyBytes bytes)
{
    return try_append(bytes.data(), bytes.size());
}

ErrorOr<void> StringBuilder::Buffer::try_append(void const* data, size_t data_size)
{
    if (data_size == 0)
        return {};
    VERIFY(data != nullptr);

    auto old_size = size();
    Checked<size_t> new_size = old_size;
    new_size += data_size;
    VERIFY(!new_size.has_overflow());

    TRY(try_resize(new_size.value()));
    __builtin_memcpy(this->data() + old_size, data, data_size);
    return {};
}

void StringBuilder::Buffer::append(char byte)
{
    MUST(try_append(byte));
}

void StringBuilder::Buffer::append(void const* data, size_t data_size)
{
    MUST(try_append(data, data_size));
}

auto StringBuilder::Buffer::leak_outline_buffer() -> Optional<OutlineBuffer>
{
    if (m_inline)
        return {};

    auto* outline_buffer = m_outline_buffer;
    auto size = m_size;
    auto outline_capacity = m_outline_capacity;

    m_inline = true;
    m_size = 0;

    return OutlineBuffer { Bytes { outline_buffer, size }, outline_capacity };
}

void StringBuilder::Buffer::move_from(Buffer&& other)
{
    m_size = other.m_size;
    m_inline = other.m_inline;

    if (other.m_inline) {
        VERIFY(other.m_size <= inline_capacity);
        if (other.m_size != 0)
            __builtin_memcpy(m_inline_buffer, other.m_inline_buffer, other.m_size);
    } else {
        m_outline_buffer = other.m_outline_buffer;
        m_outline_capacity = other.m_outline_capacity;
    }

    other.m_size = 0;
    other.m_inline = true;
}

void StringBuilder::Buffer::trim(size_t size, bool may_discard_existing_data)
{
    VERIFY(size <= m_size);
    if (!m_inline && size <= inline_capacity)
        shrink_into_inline_buffer(size, may_discard_existing_data);
    m_size = size;
}

void StringBuilder::Buffer::shrink_into_inline_buffer(size_t size, bool may_discard_existing_data)
{
    auto* outline_buffer = m_outline_buffer;
    if (!may_discard_existing_data)
        __builtin_memcpy(m_inline_buffer, outline_buffer, size);
    kfree(outline_buffer);
    m_inline = true;
}

ErrorOr<void> StringBuilder::Buffer::try_ensure_capacity_slowpath(size_t new_capacity)
{
    new_capacity = max(new_capacity, (capacity() * 3) / 2);
    new_capacity = kmalloc_good_size(new_capacity);

    if (m_inline) {
        auto* new_buffer = static_cast<u8*>(kmalloc(HeapPartition::String, new_capacity));
        if (!new_buffer)
            return Error::from_errno(ENOMEM);

        __builtin_memcpy(new_buffer, data(), m_size);
        m_outline_buffer = new_buffer;
    } else {
        auto* new_buffer = static_cast<u8*>(krealloc(HeapPartition::String, m_outline_buffer, new_capacity));
        if (!new_buffer)
            return Error::from_errno(ENOMEM);

        m_outline_buffer = new_buffer;
    }

    m_outline_capacity = new_capacity;
    m_inline = false;
    return {};
}

static constexpr size_t string_builder_prefix_size()
{
    return sizeof(Detail::StringData);
}

void StringBuilder::initialize_buffer(size_t capacity)
{
    auto prefix_size = string_builder_prefix_size();
    if (capacity > StringBuilder::inline_capacity)
        m_buffer.ensure_capacity(prefix_size + capacity);
    m_buffer.resize(prefix_size);
}

StringBuilder::StringBuilder()
{
    static constexpr auto prefix_size = string_builder_prefix_size();
    static_assert(inline_capacity > prefix_size);

    initialize_buffer(inline_capacity);
}

StringBuilder::StringBuilder(size_t initial_capacity)
{
    initialize_buffer(initial_capacity);
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

size_t StringBuilder::length() const
{
    return m_buffer.size() - string_builder_prefix_size();
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

void StringBuilder::append_ascii_without_validation(ReadonlyBytes string)
{
    MUST(try_append_ascii_without_validation(string));
}

ErrorOr<void> StringBuilder::try_append_ascii_without_validation(ReadonlyBytes string)
{
    if (string.is_empty())
        return {};

    TRY(will_append(string.size()));
    TRY(m_buffer.try_append(string));
    return {};
}

ErrorOr<void> StringBuilder::try_append(char ch)
{
    TRY(will_append(1));
    TRY(m_buffer.try_append(ch));
    return {};
}

ErrorOr<void> StringBuilder::try_append_code_unit(char16_t ch)
{
    return try_append_code_point(ch);
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

ErrorOr<void> StringBuilder::try_append_repeated(Utf16View const& string, size_t n)
{
    if (string.is_empty())
        return {};

    if (string.has_ascii_storage()) {
        TRY(will_append(string.length_in_code_units() * n));
    } else {
        auto utf8_length = simdutf::utf8_length_from_utf16(string.utf16_span().data(), string.length_in_code_units());
        TRY(will_append(utf8_length * n));
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
    return m_buffer.data() + string_builder_prefix_size();
}

u8 const* StringBuilder::data() const
{
    return m_buffer.data() + string_builder_prefix_size();
}

StringView StringBuilder::string_view() const
{
    return m_buffer.span().slice(string_builder_prefix_size());
}

void StringBuilder::clear()
{
    m_buffer.resize(string_builder_prefix_size());
}

ErrorOr<void> StringBuilder::try_append_code_point(u32 code_point)
{
    if (!is_unicode(code_point)) {
        TRY(try_append_code_point(UnicodeUtils::REPLACEMENT_CODE_POINT));
        return {};
    }

    TRY(AK::UnicodeUtils::try_code_point_to_utf8(code_point, [this](char c) { return try_append(c); }));
    return {};
}

void StringBuilder::append_code_point(u32 code_point)
{
    if (!is_unicode(code_point)) {
        append_code_point(UnicodeUtils::REPLACEMENT_CODE_POINT);
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
    if (auto buffer = m_buffer.leak_outline_buffer(); buffer.has_value()) {
        clear();
        return buffer;
    }

    return {};
}

}
