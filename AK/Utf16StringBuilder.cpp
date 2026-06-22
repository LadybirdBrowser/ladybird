/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/Checked.h>
#include <AK/UnicodeUtils.h>
#include <AK/Utf16String.h>
#include <AK/Utf16StringBuilder.h>
#include <AK/Utf16StringData.h>
#include <AK/kmalloc.h>

namespace AK {

Utf16StringBuilder::Buffer::~Buffer()
{
    clear();
}

u8* Utf16StringBuilder::Buffer::data()
{
    return m_inline ? m_inline_buffer : m_outline_buffer;
}

u8 const* Utf16StringBuilder::Buffer::data() const
{
    return m_inline ? m_inline_buffer : m_outline_buffer;
}

Bytes Utf16StringBuilder::Buffer::span()
{
    return { data(), size() };
}

ReadonlyBytes Utf16StringBuilder::Buffer::span() const
{
    return { data(), size() };
}

void Utf16StringBuilder::Buffer::clear()
{
    if (!m_inline) {
        kfree(m_outline_buffer);
        m_inline = true;
    }
    m_size = 0;
}

void Utf16StringBuilder::Buffer::resize(size_t new_size)
{
    if (new_size <= m_size) {
        trim(new_size, false);
        return;
    }

    ensure_capacity(new_size);
    set_size(new_size);
}

void Utf16StringBuilder::Buffer::set_size(size_t new_size)
{
    ASSERT(new_size <= capacity());
    m_size = new_size;
}

void Utf16StringBuilder::Buffer::ensure_capacity(size_t new_capacity)
{
    if (new_capacity <= capacity())
        return;
    ensure_capacity_slowpath(new_capacity);
}

void Utf16StringBuilder::Buffer::append(void const* data, size_t data_size)
{
    if (data_size == 0)
        return;
    VERIFY(data != nullptr);

    auto old_size = size();
    Checked<size_t> new_size = old_size;
    new_size += data_size;
    VERIFY(!new_size.has_overflow());

    resize(new_size.value());
    __builtin_memcpy(this->data() + old_size, data, data_size);
}

auto Utf16StringBuilder::Buffer::leak_outline_buffer() -> Optional<OutlineBuffer>
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

void Utf16StringBuilder::Buffer::trim(size_t size, bool may_discard_existing_data)
{
    VERIFY(size <= m_size);
    if (!m_inline && size <= inline_capacity)
        shrink_into_inline_buffer(size, may_discard_existing_data);
    m_size = size;
}

void Utf16StringBuilder::Buffer::shrink_into_inline_buffer(size_t size, bool may_discard_existing_data)
{
    auto* outline_buffer = m_outline_buffer;
    if (!may_discard_existing_data)
        __builtin_memcpy(m_inline_buffer, outline_buffer, size);
    kfree(outline_buffer);
    m_inline = true;
}

void Utf16StringBuilder::Buffer::ensure_capacity_slowpath(size_t new_capacity)
{
    Checked<size_t> grown_capacity = capacity();
    grown_capacity *= 3;
    VERIFY(!grown_capacity.has_overflow());

    new_capacity = max(new_capacity, grown_capacity.value() / 2);
    new_capacity = kmalloc_good_size(new_capacity);

    if (m_inline) {
        auto* new_buffer = static_cast<u8*>(kmalloc(HeapPartition::String, new_capacity));
        VERIFY(new_buffer);

        __builtin_memcpy(new_buffer, data(), m_size);
        m_outline_buffer = new_buffer;
    } else {
        auto* new_buffer = static_cast<u8*>(krealloc(HeapPartition::String, m_outline_buffer, new_capacity));
        VERIFY(new_buffer);

        m_outline_buffer = new_buffer;
    }

    m_outline_capacity = new_capacity;
    m_inline = false;
}

static constexpr size_t string_builder_prefix_size()
{
    return Detail::Utf16StringData::offset_of_string_storage();
}

static size_t utf16_byte_count(size_t length_in_code_units)
{
    Checked<size_t> byte_count = length_in_code_units;
    byte_count *= sizeof(char16_t);
    VERIFY(!byte_count.has_overflow());
    return byte_count.value();
}

Utf16StringBuilder::Utf16StringBuilder()
{
    initialize_buffer(inline_capacity / sizeof(char16_t));
}

Utf16StringBuilder::Utf16StringBuilder(size_t initial_capacity_in_code_units)
{
    initialize_buffer(initial_capacity_in_code_units);
}

void Utf16StringBuilder::initialize_buffer(size_t capacity_in_code_units)
{
    auto prefix_size = string_builder_prefix_size();
    auto capacity_in_bytes = utf16_byte_count(capacity_in_code_units);

    if (capacity_in_bytes > inline_capacity) {
        Checked<size_t> capacity = prefix_size;
        capacity += capacity_in_bytes;
        VERIFY(!capacity.has_overflow());

        m_buffer.ensure_capacity(capacity.value());
    }
    m_buffer.resize(prefix_size);
}

void Utf16StringBuilder::will_append(size_t size_in_bytes)
{
    Checked<size_t> needed_capacity = m_buffer.size();
    needed_capacity += size_in_bytes;
    VERIFY(!needed_capacity.has_overflow());

    if (needed_capacity <= m_buffer.capacity())
        return;

    Checked<size_t> expanded_capacity = needed_capacity;
    expanded_capacity *= 2;
    VERIFY(!expanded_capacity.has_overflow());

    m_buffer.ensure_capacity(expanded_capacity.value());
}

void Utf16StringBuilder::ensure_storage_is_utf16()
{
    if (!m_is_ascii)
        return;

    auto ascii_length = length_in_bytes();
    m_is_ascii = false;
    if (ascii_length == 0)
        return;

    m_buffer.resize(m_buffer.size() + ascii_length);

    Bytes source { data(), ascii_length };
    Span<char16_t> target { reinterpret_cast<char16_t*>(data()), ascii_length };

    for (size_t i = ascii_length; i > 0; --i) {
        auto index = i - 1;

        auto code_unit = static_cast<char16_t>(source[index]);
        target[index] = code_unit;
    }
}

void Utf16StringBuilder::append(Utf16View const& view)
{
    if (view.is_empty())
        return;

    if (m_is_ascii && view.is_ascii()) {
        will_append(view.length_in_code_units());

        if (view.has_ascii_storage()) {
            m_buffer.append(view.ascii_span().data(), view.length_in_code_units());
        } else {
            for (auto code_unit : view.utf16_span()) {
                auto ascii_code_unit = static_cast<char>(code_unit);
                m_buffer.append(&ascii_code_unit, sizeof(ascii_code_unit));
            }
        }

        return;
    }

    ensure_storage_is_utf16();
    will_append(utf16_byte_count(view.length_in_code_units()));

    if (view.has_ascii_storage()) {
        for (auto code_unit : view.ascii_span()) {
            auto utf16_code_unit = static_cast<char16_t>(code_unit);
            m_buffer.append(&utf16_code_unit, sizeof(utf16_code_unit));
        }
    } else {
        m_buffer.append(view.utf16_span().data(), utf16_byte_count(view.length_in_code_units()));
    }
}

void Utf16StringBuilder::append_ascii(char code_unit)
{
    VERIFY(is_ascii(code_unit));

    if (m_is_ascii) {
        will_append(1);
        m_buffer.append(&code_unit, sizeof(code_unit));
    } else {
        auto utf16_code_unit = static_cast<char16_t>(code_unit);
        will_append(sizeof(utf16_code_unit));
        m_buffer.append(&utf16_code_unit, sizeof(utf16_code_unit));
    }
}

void Utf16StringBuilder::append_ascii(StringView string)
{
    VERIFY(string.is_ascii());
    if (string.is_empty())
        return;

    if (m_is_ascii) {
        will_append(string.length());
        m_buffer.append(string.characters_without_null_termination(), string.length());
        return;
    }

    will_append(utf16_byte_count(string.length()));
    for (auto code_unit : string) {
        auto utf16_code_unit = static_cast<char16_t>(code_unit);
        m_buffer.append(&utf16_code_unit, sizeof(utf16_code_unit));
    }
}

void Utf16StringBuilder::append_code_unit(char16_t code_unit)
{
    if (m_is_ascii && is_ascii(code_unit)) {
        append_ascii(static_cast<char>(code_unit));
        return;
    }

    ensure_storage_is_utf16();
    will_append(sizeof(code_unit));
    m_buffer.append(&code_unit, sizeof(code_unit));
}

void Utf16StringBuilder::append_code_point(u32 code_point)
{
    if (!is_unicode(code_point)) {
        append_code_point(UnicodeUtils::REPLACEMENT_CODE_POINT);
        return;
    }

    if (m_is_ascii && is_ascii(code_point)) {
        append_ascii(static_cast<char>(code_point));
        return;
    }

    ensure_storage_is_utf16();

    (void)UnicodeUtils::code_point_to_utf16(code_point, [this](char16_t code_unit) {
        will_append(sizeof(code_unit));
        m_buffer.append(&code_unit, sizeof(code_unit));
    });
}

void Utf16StringBuilder::append_repeated_ascii(char code_unit, size_t count)
{
    VERIFY(is_ascii(code_unit));

    if (m_is_ascii) {
        will_append(count);
        for (size_t i = 0; i < count; ++i)
            m_buffer.append(&code_unit, sizeof(code_unit));
        return;
    }

    auto utf16_code_unit = static_cast<char16_t>(code_unit);
    will_append(utf16_byte_count(count));
    for (size_t i = 0; i < count; ++i)
        m_buffer.append(&utf16_code_unit, sizeof(utf16_code_unit));
}

void Utf16StringBuilder::append_repeated(Utf16View const& view, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        append(view);
}

Utf16String Utf16StringBuilder::to_string()
{
    return Utf16String::from_string_builder({}, *this);
}

Utf16View Utf16StringBuilder::view() const
{
    auto data = m_buffer.span().slice(string_builder_prefix_size());

    if (m_is_ascii)
        return { reinterpret_cast<char const*>(data.data()), data.size() };
    return { reinterpret_cast<char16_t const*>(data.data()), data.size() / sizeof(char16_t) };
}

size_t Utf16StringBuilder::length_in_code_units() const
{
    auto length = length_in_bytes();
    if (m_is_ascii)
        return length;
    return length / sizeof(char16_t);
}

void Utf16StringBuilder::trim(size_t count_in_code_units)
{
    auto count_in_bytes = count_in_code_units;
    if (!m_is_ascii)
        count_in_bytes = utf16_byte_count(count_in_code_units);

    auto decrease_count = min(length_in_bytes(), count_in_bytes);
    m_buffer.resize(m_buffer.size() - decrease_count);
}

void Utf16StringBuilder::clear()
{
    m_buffer.resize(string_builder_prefix_size());
    m_is_ascii = true;
}

auto Utf16StringBuilder::leak_buffer_for_string_construction() -> Optional<Buffer::OutlineBuffer>
{
    if (auto buffer = m_buffer.leak_outline_buffer(); buffer.has_value()) {
        clear();
        return buffer;
    }

    return {};
}

u8* Utf16StringBuilder::data()
{
    return m_buffer.data() + string_builder_prefix_size();
}

u8 const* Utf16StringBuilder::data() const
{
    return m_buffer.data() + string_builder_prefix_size();
}

size_t Utf16StringBuilder::length_in_bytes() const
{
    return m_buffer.size() - string_builder_prefix_size();
}

}
