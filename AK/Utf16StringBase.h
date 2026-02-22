/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/CharacterTypes.h>
#include <AK/NonnullRefPtr.h>
#include <AK/StringBase.h>
#include <AK/StringView.h>
#include <AK/Utf16StringData.h>
#include <AK/Utf16View.h>

namespace AK::Detail {

class Utf16StringBase {
public:
    constexpr Utf16StringBase()
        : Utf16StringBase(ShortString::create_empty())
    {
    }

    explicit constexpr Utf16StringBase(ShortString short_string)
        : m_value { .short_ascii_string = short_string }
    {
    }

    ALWAYS_INLINE explicit Utf16StringBase(NonnullRefPtr<Utf16StringData const> value)
        : m_value { .data = &value.leak_ref() }
    {
    }

    ALWAYS_INLINE Utf16StringBase(Utf16StringBase const& other)
        : m_value(other.m_value)
    {
        if (has_long_storage()) {
            if (auto const* data = data_without_union_member_assertion())
                data->ref();
        }
    }

    constexpr Utf16StringBase(Utf16StringBase&& other)
        : m_value(other.m_value)
    {
        other.m_value = { .short_ascii_string = ShortString::create_empty() };
    }

    constexpr ~Utf16StringBase()
    {
        if !consteval {
            destroy_string();
        }
    }

    ALWAYS_INLINE operator Utf16View() const& LIFETIME_BOUND { return utf16_view(); }
    explicit operator Utf16View() const&& = delete;

    [[nodiscard]] ALWAYS_INLINE String to_utf8(AllowLonelySurrogates allow_lonely_surrogates = AllowLonelySurrogates::Yes) const
    {
        return MUST(utf16_view().to_utf8(allow_lonely_surrogates));
    }

    [[nodiscard]] ALWAYS_INLINE String to_utf8_but_should_be_ported_to_utf16(AllowLonelySurrogates allow_lonely_surrogates = AllowLonelySurrogates::Yes) const
    {
        return to_utf8(allow_lonely_surrogates);
    }

    [[nodiscard]] ALWAYS_INLINE ByteString to_byte_string(AllowLonelySurrogates allow_lonely_surrogates = AllowLonelySurrogates::Yes) const
    {
        return MUST(utf16_view().to_byte_string(allow_lonely_surrogates));
    }

    [[nodiscard]] ALWAYS_INLINE StringView ascii_view() const& LIFETIME_BOUND
    {
        if (has_short_ascii_storage())
            return short_ascii_string_without_union_member_assertion().bytes();

        if (auto const* data = data_without_union_member_assertion())
            return data->ascii_view();
        return {};
    }

    [[nodiscard]] ALWAYS_INLINE Utf16View utf16_view() const& LIFETIME_BOUND
    {
        if (has_short_ascii_storage())
            return Utf16View { ascii_view().characters_without_null_termination(), length_in_code_units() };

        if (auto const* data = data_without_union_member_assertion())
            return data->utf16_view();
        return {};
    }

    StringView ascii_view() const&& = delete;
    Utf16View utf16_view() const&& = delete;

    template<Arithmetic T>
    ALWAYS_INLINE Optional<T> to_number(TrimWhitespace trim_whitespace = TrimWhitespace::Yes, int base = 10) const
    {
        return utf16_view().to_number<T>(trim_whitespace, base);
    }

    ALWAYS_INLINE Utf16StringBase& operator=(Utf16StringBase const& other)
    {
        if (&other != this) {
            if (has_long_storage()) {
                if (auto const* data = data_without_union_member_assertion())
                    data->unref();
            }

            m_value = other.m_value;

            if (has_long_storage()) {
                if (auto const* data = data_without_union_member_assertion())
                    data->ref();
            }
        }

        return *this;
    }

    ALWAYS_INLINE Utf16StringBase& operator=(Utf16StringBase&& other)
    {
        if (has_long_storage()) {
            if (auto const* data = data_without_union_member_assertion())
                data->unref();
        }

        m_value = exchange(other.m_value, { .short_ascii_string = ShortString::create_empty() });
        return *this;
    }

    [[nodiscard]] ALWAYS_INLINE bool operator==(Utf16StringBase const& other) const
    {
        if (has_short_ascii_storage() && other.has_short_ascii_storage())
            return bit_cast<FlatPtr>(m_value) == bit_cast<FlatPtr>(other.m_value);

        if (has_long_storage() && other.has_long_storage()) {
            auto const* this_data = data_without_union_member_assertion();
            auto const* other_data = other.data_without_union_member_assertion();

            if (!this_data || !other_data)
                return this_data == other_data;

            return *this_data == *other_data;
        }

        return utf16_view() == other.utf16_view();
    }

    [[nodiscard]] ALWAYS_INLINE bool operator==(Utf16View const& other) const { return utf16_view() == other; }
    [[nodiscard]] ALWAYS_INLINE bool operator==(StringView other) const { return utf16_view() == other; }

    [[nodiscard]] ALWAYS_INLINE int operator<=>(Utf16StringBase const& other) const { return utf16_view().operator<=>(other.utf16_view()); }
    [[nodiscard]] ALWAYS_INLINE int operator<=>(Utf16View const& other) const { return utf16_view().operator<=>(other); }

    [[nodiscard]] ALWAYS_INLINE bool equals_ignoring_ascii_case(Utf16View const& other) const { return utf16_view().equals_ignoring_ascii_case(other); }
    [[nodiscard]] ALWAYS_INLINE bool equals_ignoring_ascii_case(Utf16StringBase const& other) const { return utf16_view().equals_ignoring_ascii_case(other.utf16_view()); }

    template<typename... Ts>
    [[nodiscard]] ALWAYS_INLINE bool is_one_of(Ts&&... strings) const
    {
        return (this->operator==(forward<Ts>(strings)) || ...);
    }

    template<typename... Ts>
    [[nodiscard]] ALWAYS_INLINE bool is_one_of_ignoring_ascii_case(Ts&&... strings) const
    {
        return (this->equals_ignoring_ascii_case(forward<Ts>(strings)) || ...);
    }

    [[nodiscard]] ALWAYS_INLINE u32 hash() const
    {
        if (has_short_ascii_storage())
            return StringView { short_ascii_string_without_union_member_assertion().bytes() }.hash();

        if (auto const* data = data_without_union_member_assertion())
            return data->hash();
        return string_hash<char16_t>(nullptr, 0);
    }

    [[nodiscard]] ALWAYS_INLINE bool is_empty() const { return length_in_code_units() == 0uz; }
    [[nodiscard]] ALWAYS_INLINE bool is_ascii() const { return utf16_view().is_ascii(); }
    [[nodiscard]] ALWAYS_INLINE bool is_ascii_whitespace() const { return utf16_view().is_ascii_whitespace(); }

    [[nodiscard]] ALWAYS_INLINE size_t length_in_code_units() const
    {
        if (has_short_ascii_storage())
            return short_ascii_string_without_union_member_assertion().byte_count();

        if (auto const* data = data_without_union_member_assertion())
            return data->length_in_code_units();
        return 0;
    }

    [[nodiscard]] ALWAYS_INLINE size_t length_in_code_points() const
    {
        if (has_short_ascii_storage())
            return short_ascii_string_without_union_member_assertion().byte_count();

        if (auto const* data = data_without_union_member_assertion())
            return data->length_in_code_points();
        return 0;
    }

    [[nodiscard]] ALWAYS_INLINE char16_t code_unit_at(size_t code_unit_offset) const { return utf16_view().code_unit_at(code_unit_offset); }
    [[nodiscard]] ALWAYS_INLINE u32 code_point_at(size_t code_unit_offset) const { return utf16_view().code_point_at(code_unit_offset); }

    [[nodiscard]] ALWAYS_INLINE size_t code_unit_offset_of(size_t code_point_offset) const
    {
        if (has_ascii_storage())
            return code_point_offset;
        return utf16_view().code_unit_offset_of(code_point_offset);
    }

    [[nodiscard]] ALWAYS_INLINE size_t code_point_offset_of(size_t code_unit_offset) const
    {
        if (has_ascii_storage())
            return code_unit_offset;
        return utf16_view().code_point_offset_of(code_unit_offset);
    }

    [[nodiscard]] ALWAYS_INLINE Utf16CodePointIterator begin() const { return utf16_view().begin(); }
    [[nodiscard]] ALWAYS_INLINE Utf16CodePointIterator end() const { return utf16_view().end(); }

    [[nodiscard]] ALWAYS_INLINE Utf16View substring_view(size_t code_unit_offset, size_t code_unit_length) const LIFETIME_BOUND
    {
        return utf16_view().substring_view(code_unit_offset, code_unit_length);
    }

    [[nodiscard]] ALWAYS_INLINE Utf16View substring_view(size_t code_unit_offset) const LIFETIME_BOUND
    {
        return utf16_view().substring_view(code_unit_offset);
    }

    ALWAYS_INLINE Optional<size_t> find_code_unit_offset(char16_t needle, size_t start_offset = 0) const
    {
        return utf16_view().find_code_unit_offset(needle, start_offset);
    }

    ALWAYS_INLINE Optional<size_t> find_code_unit_offset(Utf16View const& needle, size_t start_offset = 0) const
    {
        return utf16_view().find_code_unit_offset(needle, start_offset);
    }

    ALWAYS_INLINE Optional<size_t> find_code_unit_offset_ignoring_case(Utf16View const& needle, size_t start_offset = 0) const
    {
        return utf16_view().find_code_unit_offset_ignoring_case(needle, start_offset);
    }

    [[nodiscard]] ALWAYS_INLINE bool contains(char16_t needle) const { return find_code_unit_offset(needle).has_value(); }
    [[nodiscard]] ALWAYS_INLINE bool contains(Utf16View const& needle) const { return find_code_unit_offset(needle).has_value(); }
    [[nodiscard]] ALWAYS_INLINE bool contains_any_of(ReadonlySpan<u32> needles) const { return utf16_view().contains_any_of(needles); }

    [[nodiscard]] ALWAYS_INLINE size_t count(Utf16View const& needle) const { return utf16_view().count(needle); }

    [[nodiscard]] ALWAYS_INLINE bool starts_with(char16_t needle) const { return utf16_view().starts_with(needle); }
    [[nodiscard]] ALWAYS_INLINE bool starts_with(Utf16View const& needle) const { return utf16_view().starts_with(needle); }

    [[nodiscard]] ALWAYS_INLINE bool ends_with(char16_t needle) const { return utf16_view().ends_with(needle); }
    [[nodiscard]] ALWAYS_INLINE bool ends_with(Utf16View const& needle) const { return utf16_view().ends_with(needle); }

    [[nodiscard]] ALWAYS_INLINE Vector<Utf16View> split_view(char16_t needle, SplitBehavior split_behavior) const { return utf16_view().split_view(needle, split_behavior); }
    [[nodiscard]] ALWAYS_INLINE Vector<Utf16View> split_view(Utf16View const& needle, SplitBehavior split_behavior) const { return utf16_view().split_view(needle, split_behavior); }

    template<typename Callback>
    ALWAYS_INLINE void for_each_split_view(char16_t separator, SplitBehavior split_behavior, Callback&& callback) const
    {
        utf16_view().for_each_split_view(separator, split_behavior, forward<Callback>(callback));
    }

    template<typename Callback>
    ALWAYS_INLINE void for_each_split_view(Utf16View const& separator, SplitBehavior split_behavior, Callback&& callback) const
    {
        utf16_view().for_each_split_view(separator, split_behavior, forward<Callback>(callback));
    }

    // This is primarily interesting to unit tests.
    [[nodiscard]] constexpr bool has_short_ascii_storage() const
    {
        if consteval {
            return (m_value.short_ascii_string.byte_count_and_short_string_flag & StringBase::SHORT_STRING_FLAG) != 0;
        } else {
            return (short_ascii_string_without_union_member_assertion().byte_count_and_short_string_flag & StringBase::SHORT_STRING_FLAG) != 0;
        }
    }

    // This is primarily interesting to unit tests.
    [[nodiscard]] ALWAYS_INLINE bool has_long_ascii_storage() const
    {
        if (has_short_ascii_storage())
            return false;

        if (auto const* data = data_without_union_member_assertion())
            return data->has_ascii_storage();
        return false;
    }

    // This is primarily interesting to unit tests.
    [[nodiscard]] ALWAYS_INLINE bool has_ascii_storage() const
    {
        return has_short_ascii_storage() || has_long_ascii_storage();
    }

    // This is primarily interesting to unit tests.
    [[nodiscard]] ALWAYS_INLINE bool has_long_utf16_storage() const
    {
        if (has_short_ascii_storage())
            return false;

        if (auto const* data = data_without_union_member_assertion())
            return data->has_utf16_storage();
        return false;
    }

    // This is primarily interesting to unit tests.
    [[nodiscard]] ALWAYS_INLINE bool has_long_storage() const
    {
        return !has_short_ascii_storage();
    }

    [[nodiscard]] ALWAYS_INLINE Utf16StringData const* data(Badge<Utf16FlyString>) const
    {
        VERIFY(has_long_storage());
        return data_without_union_member_assertion();
    }

    ALWAYS_INLINE void set_data(Badge<Utf16FlyString>, Utf16StringData const* data)
    {
        auto const** this_data = __builtin_launder(&m_value.data);
        (*this_data) = data;
        (*this_data)->ref();
    }

    template<OneOf<Utf16String, Utf16FlyString> T>
    constexpr Utf16StringBase(Badge<T>, nullptr_t)
        : m_value { .data = nullptr }
    {
    }

    [[nodiscard]] constexpr FlatPtr raw(Badge<Utf16FlyString>) const { return raw(); }

protected:
    [[nodiscard]] constexpr FlatPtr raw() const { return bit_cast<FlatPtr>(m_value); }

    ALWAYS_INLINE void destroy_string() const
    {
        if (has_long_storage()) {
            if (auto const* data = data_without_union_member_assertion())
                data->unref();
        }
    }

    // This is technically **invalid**! See StringBase for details.
    ALWAYS_INLINE ShortString const& short_ascii_string_without_union_member_assertion() const { return *__builtin_launder(&m_value.short_ascii_string); }
    ALWAYS_INLINE Utf16StringData const* data_without_union_member_assertion() const { return *__builtin_launder(&m_value.data); }

    union {
        ShortString short_ascii_string;
        Utf16StringData const* data;
    } m_value;
};

}
