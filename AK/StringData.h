/*
 * Copyright (c) 2023, Dan Klishch <danilklishch@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/StringBuilder.h>

namespace AK::Detail {

static constexpr size_t MAX_SHORT_STRING_BYTE_COUNT = sizeof(StringData*) - sizeof(u8);

class StringData;

void did_destroy_fly_string_data(Badge<StringData>, StringData const&);

class StringData final : public RefCounted<StringData> {
public:
    static ErrorOr<NonnullRefPtr<StringData>> create_uninitialized(size_t byte_count, u8*& buffer)
    {
        VERIFY(byte_count);

        auto capacity = allocation_size_for_string_data(byte_count);
        void* slot = malloc(capacity);
        if (!slot)
            return Error::from_errno(ENOMEM);

        auto new_string_data = adopt_ref(*new (slot) StringData(byte_count, capacity));
        buffer = const_cast<u8*>(new_string_data->bytes().data());
        return new_string_data;
    }

    static NonnullRefPtr<StringData> create_from_string_builder(StringBuilder& builder)
    {
        auto byte_count = builder.length();
        VERIFY(byte_count > MAX_SHORT_STRING_BYTE_COUNT);

        auto buffer = builder.leak_buffer_for_string_construction({});
        VERIFY(buffer.has_value()); // We should only arrive here if the buffer is outlined.

        return adopt_ref(*new (buffer->buffer.data()) StringData(byte_count, buffer->capacity));
    }

    static ErrorOr<NonnullRefPtr<StringData>> create_substring(StringData const& superstring, size_t start, size_t byte_count)
    {
        // Strings of MAX_SHORT_STRING_BYTE_COUNT bytes or less should be handled by the String short string optimization.
        VERIFY(byte_count > MAX_SHORT_STRING_BYTE_COUNT);

        auto capacity = sizeof(StringData) + sizeof(StringData::SubstringData);
        void* slot = malloc(capacity);
        if (!slot)
            return Error::from_errno(ENOMEM);

        return adopt_ref(*new (slot) StringData(superstring, start, byte_count, capacity));
    }

    struct SubstringData {
        StringData const* superstring { nullptr };
        u32 start_offset { 0 };
    };

    void operator delete(void* ptr)
    {
        free(ptr);
    }

    ~StringData()
    {
        if (m_substring)
            substring_data().superstring->unref();
        if (m_is_fly_string)
            Detail::did_destroy_fly_string_data({}, *this);
    }

    SubstringData const& substring_data() const
    {
        return *reinterpret_cast<SubstringData const*>(m_bytes_or_substring_data);
    }

    // NOTE: There is no guarantee about null-termination.
    ReadonlyBytes bytes() const
    {
        if (m_substring) {
            auto const& data = substring_data();
            return data.superstring->bytes().slice(data.start_offset, m_byte_count);
        }
        return { &m_bytes_or_substring_data[0], m_byte_count };
    }

    StringView bytes_as_string_view() const { return { bytes() }; }

    bool operator==(StringData const& other) const
    {
        return bytes_as_string_view() == other.bytes_as_string_view();
    }

    unsigned hash() const
    {
        if (!m_has_hash)
            compute_hash();
        return m_hash;
    }

    bool is_fly_string() const { return m_is_fly_string; }
    void set_fly_string(bool is_fly_string) const { m_is_fly_string = is_fly_string; }

    size_t byte_count() const { return m_byte_count; }

private:
    static constexpr size_t allocation_size_for_string_data(size_t length)
    {
        return sizeof(StringData) + (sizeof(char) * length);
    }

    StringData(size_t byte_count, size_t capacity)
        : m_byte_count(byte_count)
        , m_capacity(capacity)
    {
    }

    StringData(StringData const& superstring, size_t start, size_t byte_count, size_t capacity)
        : m_byte_count(byte_count)
        , m_capacity(capacity)
        , m_substring(true)
    {
        auto& data = const_cast<SubstringData&>(substring_data());
        data.start_offset = start;
        data.superstring = &superstring;
        superstring.ref();
    }

    void compute_hash() const
    {
        auto bytes = this->bytes();
        if (bytes.size() == 0)
            m_hash = 0;
        else
            m_hash = string_hash(reinterpret_cast<char const*>(bytes.data()), bytes.size());
        m_has_hash = true;
    }

    u32 m_byte_count { 0 };
    u32 m_capacity { 0 };

    mutable unsigned m_hash { 0 };
    mutable bool m_has_hash { false };
    bool m_substring { false };
    mutable bool m_is_fly_string { false };

    alignas(SubstringData) u8 m_bytes_or_substring_data[0];
};

}
