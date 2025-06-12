/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/NumericLimits.h>
#include <AK/RefCounted.h>
#include <AK/Span.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <AK/Utf16View.h>

namespace AK::Detail {

class Utf16StringData final : public RefCounted<Utf16StringData> {
public:
    enum class StorageType : u8 {
        ASCII,
        UTF16,
    };

    enum class AllowASCIIStorage : u8 {
        No,
        Yes,
    };

    static NonnullRefPtr<Utf16StringData> from_utf8(StringView, AllowASCIIStorage);
    static NonnullRefPtr<Utf16StringData> from_utf16(Utf16View const&);
    static NonnullRefPtr<Utf16StringData> from_utf32(Utf32View const&);

    ~Utf16StringData() = default;

    void operator delete(void* ptr)
    {
        free(ptr);
    }

    [[nodiscard]] ALWAYS_INLINE bool operator==(Utf16StringData const& other) const
    {
        return utf16_view() == other.utf16_view();
    }

    [[nodiscard]] ALWAYS_INLINE bool operator==(Utf16View const& other) const
    {
        return utf16_view() == other;
    }

    [[nodiscard]] ALWAYS_INLINE bool operator==(StringView const& other) const
    {
        if (has_ascii_storage())
            return ascii_view() == other;
        return utf16_view() == Utf16View { other.characters_without_null_termination(), other.length() };
    }

    [[nodiscard]] ALWAYS_INLINE bool has_ascii_storage() const { return m_length_in_code_units >> Detail::UTF16_FLAG == 0; }
    [[nodiscard]] ALWAYS_INLINE bool has_utf16_storage() const { return m_length_in_code_units >> Detail::UTF16_FLAG != 0; }

    ALWAYS_INLINE u32 hash() const
    {
        if (!m_has_hash)
            m_hash = calculate_hash();
        return m_hash;
    }

    [[nodiscard]] ALWAYS_INLINE size_t length_in_code_units() const { return m_length_in_code_units & ~(1uz << Detail::UTF16_FLAG); }
    [[nodiscard]] ALWAYS_INLINE size_t length_in_code_points() const
    {
        if (has_ascii_storage())
            return length_in_code_units();
        if (m_length_in_code_points == NumericLimits<size_t>::max())
            m_length_in_code_points = calculate_code_point_length();
        return m_length_in_code_points;
    }

    [[nodiscard]] ALWAYS_INLINE StringView ascii_view() const
    {
        ASSERT(has_ascii_storage());
        return { m_ascii_data, length_in_code_units() };
    }

    [[nodiscard]] ALWAYS_INLINE Utf16View utf16_view() const
    {
        if (has_ascii_storage())
            return { m_ascii_data, length_in_code_units() };

        Utf16View view { m_utf16_data, length_in_code_units() };
        view.m_length_in_code_points = m_length_in_code_points;

        return view;
    }

private:
    ALWAYS_INLINE Utf16StringData(StorageType storage_type, size_t code_unit_length)
        : m_length_in_code_units(code_unit_length)
    {
        if (storage_type == StorageType::UTF16)
            m_length_in_code_units |= 1uz << Detail::UTF16_FLAG;
    }

    static NonnullRefPtr<Utf16StringData> create_uninitialized(StorageType storage_type, size_t code_unit_length);

    template<typename ViewType>
    static NonnullRefPtr<Utf16StringData> create_from_code_point_iterable(ViewType const&);

    [[nodiscard]] size_t calculate_code_point_length() const;

    [[nodiscard]] ALWAYS_INLINE u32 calculate_hash() const
    {
        if (has_ascii_storage())
            return ascii_view().hash();
        return utf16_view().hash();
    }

    // We store whether this string has ASCII or UTF-16 storage by setting the most significant bit of m_length_in_code_units
    // to 1 for UTF-16 storage. This shrinks the size of most UTF-16 string related classes, at the cost of not being
    // allowed to create a string larger than 2**63 - 1.
    size_t m_length_in_code_units { 0 };
    mutable size_t m_length_in_code_points { NumericLimits<size_t>::max() };

    mutable u32 m_hash { 0 };
    mutable bool m_has_hash { false };

    union {
        char m_ascii_data[0];
        char16_t m_utf16_data[0];
    };
};

}
