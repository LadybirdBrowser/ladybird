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
        : m_impl(other.m_impl)
    {
        other.m_impl = { .short_string = { .byte_count_and_short_string_flag = SHORT_STRING_FLAG } };
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
        if (is_constant_evaluated())
            return (m_impl.short_string.byte_count_and_short_string_flag & SHORT_STRING_FLAG) != 0;
        return (short_string_without_union_member_assertion().byte_count_and_short_string_flag & SHORT_STRING_FLAG) != 0;
    }

    // Returns the underlying UTF-8 encoded bytes.
    // NOTE: There is no guarantee about null-termination.
    [[nodiscard]] ReadonlyBytes bytes() const;
    [[nodiscard]] u32 hash() const;
    [[nodiscard]] size_t byte_count() const;

    [[nodiscard]] bool operator==(StringBase const&) const;

    [[nodiscard]] ALWAYS_INLINE constexpr FlatPtr raw(Badge<FlyString>) const { return bit_cast<FlatPtr>(m_impl); }
    [[nodiscard]] ALWAYS_INLINE constexpr FlatPtr raw(Badge<String>) const { return bit_cast<FlatPtr>(m_impl); }

    template<typename Func>
    ALWAYS_INLINE ErrorOr<void> replace_with_new_string(Badge<StringView>, size_t byte_count, Func&& callback)
    {
        return replace_with_new_string(byte_count, forward<Func>(callback));
    }

protected:
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
        : m_impl { .data = nullptr }
    {
    }

    explicit constexpr StringBase(ShortString short_string)
        : m_impl { .short_string = short_string }
    {
    }

    ErrorOr<Bytes> replace_with_uninitialized_buffer(size_t byte_count);

    constexpr Bytes replace_with_uninitialized_short_string(size_t byte_count)
    {
        VERIFY(is_short_string());
        VERIFY(byte_count <= MAX_SHORT_STRING_BYTE_COUNT);

        m_impl = { .short_string = {} };
        m_impl.short_string.byte_count_and_short_string_flag = (byte_count << SHORT_STRING_BYTE_COUNT_SHIFT_COUNT) | SHORT_STRING_FLAG;
        return { m_impl.short_string.storage, byte_count };
    }

    void destroy_string();

// from the union member that is not active; note that this guarantees nothing and just checks whatever state we're in - not all.
#ifdef AK_STRINGBASE_VERIFY_LAUNDER_DEBUG
    ShortString short_string_without_union_member_assertion() const
    {
        auto laundered_value = *__builtin_launder(&m_impl.short_string);
        auto bitcast_value1 = bit_cast<FlatPtr>(*__builtin_launder(&m_impl.data));
        auto bitcast_value2 = bit_cast<FlatPtr>(*__builtin_launder(&m_impl.short_string)); // one of these is the active one :P
        VERIFY(bit_cast<FlatPtr>(laundered_value) == bitcast_value1 && bit_cast<FlatPtr>(laundered_value) == bitcast_value2);
        return *__builtin_launder(&laundered_value);
    }
    StringData const* data_without_union_member_assertion() const
    {
        auto laundered_value = *__builtin_launder(&m_impl.data);
        auto bitcast_value1 = bit_cast<FlatPtr>(*__builtin_launder(&m_impl.data));
        auto bitcast_value2 = bit_cast<FlatPtr>(*__builtin_launder(&m_impl.short_string)); // one of these is the active one :P
        VERIFY(bit_cast<FlatPtr>(laundered_value) == bitcast_value1 && bit_cast<FlatPtr>(laundered_value) == bitcast_value2);
        return *__builtin_launder(&laundered_value);
    }
#else
    // This is technically **invalid**!
    // Inactive union members are not required to exist at all, and at this point there might not be any real object at this address.
    // Empirically though, they point at the same address, so we can tell the compiler to _trust me bro_, and launder the pointer despite
    // the launder itself being possibly-invalid.
    // The block above asserts that we're reading the right value (:tm:), but this here is for schpeed.
    ALWAYS_INLINE ShortString short_string_without_union_member_assertion() const { return *__builtin_launder(&m_impl.short_string); }
    ALWAYS_INLINE StringData const* data_without_union_member_assertion() const { return *__builtin_launder(&m_impl.data); }
#endif

    union {
        ShortString short_string;
        StringData const* data;
    } m_impl;
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
    if (is_short_string())
        return m_impl.short_string.bytes();
    if (!m_impl.data)
        return {};
    return data_without_union_member_assertion()->bytes();
}

inline u32 StringBase::hash() const
{
    if (is_short_string()) {
        auto bytes = this->bytes();
        return string_hash(reinterpret_cast<char const*>(bytes.data()), bytes.size());
    }
    if (!m_impl.data)
        return string_hash(nullptr, 0);
    return data_without_union_member_assertion()->hash();
}

inline size_t StringBase::byte_count() const
{
    if (is_short_string())
        return m_impl.short_string.byte_count_and_short_string_flag >> StringBase::SHORT_STRING_BYTE_COUNT_SHIFT_COUNT;

    if (!m_impl.data)
        return 0;
    return data_without_union_member_assertion()->byte_count();
}

inline void StringBase::destroy_string()
{
    if (!is_short_string() && m_impl.data)
        data_without_union_member_assertion()->unref();
}

inline StringBase::StringBase(NonnullRefPtr<Detail::StringData const> data)
    : m_impl { .data = &data.leak_ref() }
{
}

inline StringBase::StringBase(StringBase const& other)
    : m_impl(other.m_impl)
{
    if (!is_short_string() && m_impl.data)
        data_without_union_member_assertion()->ref();
}

inline StringBase& StringBase::operator=(StringBase&& other)
{
    if (!is_short_string() && m_impl.data)
        data_without_union_member_assertion()->unref();

    m_impl = exchange(other.m_impl, { .short_string = { .byte_count_and_short_string_flag = SHORT_STRING_FLAG } });
    return *this;
}

inline StringBase& StringBase::operator=(StringBase const& other)
{
    if (&other != this) {
        if (!is_short_string() && m_impl.data)
            data_without_union_member_assertion()->unref();

        m_impl = other.m_impl;
        if (!is_short_string() && m_impl.data)
            data_without_union_member_assertion()->ref();
    }
    return *this;
}

inline bool StringBase::operator==(StringBase const& other) const
{
    if (is_short_string())
        return bit_cast<FlatPtr>(m_impl) == bit_cast<FlatPtr>(other.m_impl);
    if (other.is_short_string())
        return false;
    if (m_impl.data == nullptr || other.m_impl.data == nullptr)
        return m_impl.data == other.m_impl.data;
    if (data_without_union_member_assertion()->is_fly_string() && other.data_without_union_member_assertion()->is_fly_string())
        return m_impl.data == other.m_impl.data;
    return bytes() == other.bytes();
}

}
