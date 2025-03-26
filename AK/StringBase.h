/*
 * Copyright (c) 2023, Dan Klishch <danilklishch@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/Endian.h>
#include <AK/Forward.h>
#include <AK/StringData.h>

namespace AK::Detail {

struct ShortString {
    ReadonlyBytes bytes() const;
    size_t byte_count() const;

    // NOTE: This is the byte count shifted left 1 step and or'ed with a 1 (the SHORT_STRING_FLAG)
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    u8 byte_count_and_short_string_flag { 0 };
    u8 storage[MAX_SHORT_STRING_BYTE_COUNT] = { 0 };
#else
    u8 storage[MAX_SHORT_STRING_BYTE_COUNT] = { 0 };
    u8 byte_count_and_short_string_flag { 0 };
#endif
};

static_assert(sizeof(ShortString) == sizeof(StringData*));

class StringBase {
public:
    // Creates an empty (zero-length) String.
    constexpr StringBase()
        : StringBase(ShortString { .byte_count_and_short_string_flag = SHORT_STRING_FLAG })
    {
    }

    StringBase(StringBase const&);

    constexpr StringBase(StringBase&& other)
        : m_short_string(other.m_short_string)
    {
        other.m_short_string = ShortString {};
        other.m_short_string.byte_count_and_short_string_flag = SHORT_STRING_FLAG;
    }

    StringBase& operator=(StringBase&&);
    StringBase& operator=(StringBase const&);

    constexpr ~StringBase()
    {
        if (!is_constant_evaluated())
            destroy_string();
    }

    // NOTE: This is primarily interesting to unit tests.
    [[nodiscard]] constexpr bool is_short_string() const
    {
        return (m_short_string.byte_count_and_short_string_flag & SHORT_STRING_FLAG) != 0;
    }

    // Returns the underlying UTF-8 encoded bytes.
    // NOTE: There is no guarantee about null-termination.
    [[nodiscard]] ReadonlyBytes bytes() const;
    [[nodiscard]] u32 hash() const;
    [[nodiscard]] size_t byte_count() const;

    [[nodiscard]] bool operator==(StringBase const&) const;

    [[nodiscard]] ALWAYS_INLINE FlatPtr raw(Badge<FlyString>) const { return bit_cast<FlatPtr>(m_data); }

protected:
    bool is_invalid() const { return m_invalid_tag == UINTPTR_MAX; }

    template<typename Func>
    ErrorOr<void> replace_with_new_string(size_t byte_count, Func&& callback)
    {
        Bytes buffer = TRY(replace_with_uninitialized_buffer(byte_count));
        if (byte_count != 0)
            TRY(callback(buffer));
        return {};
    }

    template<typename Func>
    constexpr void replace_with_new_short_string(size_t byte_count, Func&& callback)
    {
        Bytes buffer = replace_with_uninitialized_short_string(byte_count);
        if (byte_count != 0)
            callback(buffer);
    }

    void replace_with_string_builder(StringBuilder&);

    // This is not a trivial operation with storage, so it does not belong here. Unfortunately, it
    // is impossible to implement it without access to StringData.
    ErrorOr<StringBase> substring_from_byte_offset_with_shared_superstring(size_t start, size_t byte_count) const;

private:
    friend class ::AK::String;
    friend class ::AK::FlyString;
    friend struct ::AK::Detail::ShortString;

    // NOTE: If the least significant bit of the pointer is set, this is a short string.
    static constexpr uintptr_t SHORT_STRING_FLAG = 1;
    static constexpr unsigned SHORT_STRING_BYTE_COUNT_SHIFT_COUNT = 2;

    explicit StringBase(NonnullRefPtr<Detail::StringData const>);

    explicit constexpr StringBase(nullptr_t)
        : m_invalid_tag(UINTPTR_MAX)
    {
    }

    explicit constexpr StringBase(ShortString short_string)
        : m_short_string(short_string)
    {
    }

    ErrorOr<Bytes> replace_with_uninitialized_buffer(size_t byte_count);

    constexpr Bytes replace_with_uninitialized_short_string(size_t byte_count)
    {
        VERIFY(is_short_string());
        VERIFY(byte_count <= MAX_SHORT_STRING_BYTE_COUNT);

        m_short_string = ShortString {};
        m_short_string.byte_count_and_short_string_flag = (byte_count << SHORT_STRING_BYTE_COUNT_SHIFT_COUNT) | SHORT_STRING_FLAG;
        return { m_short_string.storage, byte_count };
    }

    void destroy_string();

    union {
        ShortString m_short_string;
        Detail::StringData const* m_data { nullptr };
        uintptr_t m_invalid_tag;
    };
};

inline ReadonlyBytes ShortString::bytes() const
{
    return { storage, byte_count() };
}

inline size_t ShortString::byte_count() const
{
    return byte_count_and_short_string_flag >> StringBase::SHORT_STRING_BYTE_COUNT_SHIFT_COUNT;
}

inline ReadonlyBytes StringBase::bytes() const
{
    ASSERT(!is_invalid());
    if (is_short_string())
        return m_short_string.bytes();
    return m_data->bytes();
}

inline u32 StringBase::hash() const
{
    ASSERT(!is_invalid());
    if (is_short_string()) {
        auto bytes = this->bytes();
        return string_hash(reinterpret_cast<char const*>(bytes.data()), bytes.size());
    }
    return m_data->hash();
}

inline size_t StringBase::byte_count() const
{
    ASSERT(!is_invalid());
    if (is_short_string())
        return m_short_string.byte_count_and_short_string_flag >> StringBase::SHORT_STRING_BYTE_COUNT_SHIFT_COUNT;
    return m_data->byte_count();
}

inline void StringBase::destroy_string()
{
    if (!is_short_string())
        m_data->unref();
}

inline StringBase::StringBase(NonnullRefPtr<Detail::StringData const> data)
    : m_data(&data.leak_ref())
{
}

inline StringBase::StringBase(StringBase const& other)
    : m_data(other.m_data)
{
    if (!is_short_string())
        m_data->ref();
}

inline StringBase& StringBase::operator=(StringBase&& other)
{
    if (!is_short_string())
        m_data->unref();

    m_data = exchange(other.m_data, nullptr);
    other.m_short_string.byte_count_and_short_string_flag = SHORT_STRING_FLAG;
    return *this;
}

inline StringBase& StringBase::operator=(StringBase const& other)
{
    if (&other != this) {
        if (!is_short_string())
            m_data->unref();

        m_data = other.m_data;
        if (!is_short_string())
            m_data->ref();
    }
    return *this;
}

inline bool StringBase::operator==(StringBase const& other) const
{
    ASSERT(!is_invalid());
    if (is_short_string())
        return m_data == other.m_data;
    if (other.is_short_string())
        return false;
    if (m_data->is_fly_string() && other.m_data->is_fly_string())
        return m_data == other.m_data;
    return bytes() == other.bytes();
}

}
