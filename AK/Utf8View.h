/*
 * Copyright (c) 2019-2020, Sergey Bugaev <bugaevc@serenityos.org>
 * Copyright (c) 2021, Max Wipfli <mail@maxwipfli.ch>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Format.h>
#include <AK/StringView.h>
#include <AK/Types.h>

namespace AK {

class Utf8View;

class Utf8CodePointIterator {
    friend class Utf8View;

public:
    Utf8CodePointIterator() = default;
    ~Utf8CodePointIterator() = default;

    bool operator==(Utf8CodePointIterator const&) const = default;
    bool operator!=(Utf8CodePointIterator const&) const = default;
    Utf8CodePointIterator& operator++();
    u32 operator*() const;
    // NOTE: This returns {} if the peek is at or past EOF.
    Optional<u32> peek(size_t offset = 0) const;

    ssize_t operator-(Utf8CodePointIterator const& other) const
    {
        return m_ptr - other.m_ptr;
    }

    u8 const* ptr() const { return m_ptr; }

    // Note : These methods return the information about the underlying UTF-8 bytes.
    // If the UTF-8 string encoding is not valid at the iterator's position, then the underlying bytes might be different from the
    // decoded character's re-encoded bytes (which will be an `0xFFFD REPLACEMENT CHARACTER` with an UTF-8 length of three bytes).
    // If your code relies on the decoded character being equivalent to the re-encoded character, use the `UTF8View::validate()`
    // method on the view prior to using its iterator.
    size_t underlying_code_point_length_in_bytes() const;
    ReadonlyBytes underlying_code_point_bytes() const;
    bool done() const { return m_length == 0; }

private:
    Utf8CodePointIterator(u8 const* ptr, size_t length)
        : m_ptr(ptr)
        , m_length(length)
    {
    }

    u8 const* m_ptr { nullptr };
    size_t m_length { 0 };
};

class Utf8View {
public:
    using Iterator = Utf8CodePointIterator;

    Utf8View() = default;

    explicit constexpr Utf8View(StringView string)
        : m_string(string)
    {
    }

    explicit Utf8View(ByteString& string)
        : m_string(string.view())
    {
    }

    explicit Utf8View(ByteString&&) = delete;

    enum class AllowSurrogates {
        Yes,
        No,
    };

    ~Utf8View() = default;

    StringView as_string() const { return m_string; }

    Utf8CodePointIterator begin() const { return { begin_ptr(), m_string.length() }; }
    Utf8CodePointIterator end() const { return { end_ptr(), 0 }; }
    Utf8CodePointIterator iterator_at_byte_offset(size_t) const;

    Utf8CodePointIterator iterator_at_byte_offset_without_validation(size_t) const;

    unsigned char const* bytes() const { return begin_ptr(); }
    size_t byte_length() const { return m_string.length(); }
    size_t byte_offset_of(Utf8CodePointIterator const&) const;
    size_t byte_offset_of(size_t code_point_offset) const;

    Utf8View substring_view(size_t byte_offset, size_t byte_length) const { return Utf8View { m_string.substring_view(byte_offset, byte_length) }; }
    Utf8View substring_view(size_t byte_offset) const { return substring_view(byte_offset, byte_length() - byte_offset); }
    Utf8View unicode_substring_view(size_t code_point_offset, size_t code_point_length) const;
    Utf8View unicode_substring_view(size_t code_point_offset) const { return unicode_substring_view(code_point_offset, length() - code_point_offset); }

    bool is_empty() const { return m_string.is_empty(); }
    bool is_null() const { return m_string.is_null(); }
    bool starts_with(Utf8View const&) const;
    bool contains(u32) const;

    Utf8View trim(Utf8View const& characters, TrimMode mode = TrimMode::Both) const;

    size_t iterator_offset(Utf8CodePointIterator const& it) const
    {
        return byte_offset_of(it);
    }

    size_t length() const
    {
        if (!m_have_length) {
            m_length = calculate_length();
            m_have_length = true;
        }
        return m_length;
    }

    bool validate(AllowSurrogates allow_surrogates = AllowSurrogates::Yes) const
    {
        size_t valid_bytes = 0;
        return validate(valid_bytes, allow_surrogates);
    }

    bool validate(size_t& valid_bytes, AllowSurrogates allow_surrogates = AllowSurrogates::Yes) const;

private:
    friend class Utf8CodePointIterator;

    u8 const* begin_ptr() const { return reinterpret_cast<u8 const*>(m_string.characters_without_null_termination()); }
    u8 const* end_ptr() const { return begin_ptr() + m_string.length(); }
    size_t calculate_length() const;

    struct Utf8EncodedByteData {
        size_t byte_length { 0 };
        u8 encoding_bits { 0 };
        u8 encoding_mask { 0 };
        u32 first_code_point { 0 };
        u32 last_code_point { 0 };
    };

    static constexpr Array<Utf8EncodedByteData, 4> utf8_encoded_byte_data { {
        { 1, 0b0000'0000, 0b1000'0000, 0x0000, 0x007F },
        { 2, 0b1100'0000, 0b1110'0000, 0x0080, 0x07FF },
        { 3, 0b1110'0000, 0b1111'0000, 0x0800, 0xFFFF },
        { 4, 0b1111'0000, 0b1111'1000, 0x10000, 0x10FFFF },
    } };

    struct LeadingByte {
        size_t byte_length { 0 };
        u32 code_point_bits { 0 };
        bool is_valid { false };
    };

    static constexpr LeadingByte decode_leading_byte(u8 byte)
    {
        for (auto const& data : utf8_encoded_byte_data) {
            if ((byte & data.encoding_mask) != data.encoding_bits)
                continue;

            byte &= ~data.encoding_mask;
            return { data.byte_length, byte, true };
        }

        return { .is_valid = false };
    }

    StringView m_string;
    mutable size_t m_length { 0 };
    mutable bool m_have_length { false };
};

class DeprecatedStringCodePointIterator {
public:
    Optional<u32> next()
    {
        if (m_it.done())
            return {};
        auto value = *m_it;
        ++m_it;
        return value;
    }

    [[nodiscard]] Optional<u32> peek() const
    {
        if (m_it.done())
            return {};
        return *m_it;
    }

    [[nodiscard]] size_t byte_offset() const
    {
        return Utf8View(m_string).byte_offset_of(m_it);
    }

    DeprecatedStringCodePointIterator(ByteString string)
        : m_string(move(string))
        , m_it(Utf8View(m_string).begin())
    {
    }

private:
    ByteString m_string;
    Utf8CodePointIterator m_it;
};

template<>
struct Formatter<Utf8View> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder&, Utf8View const&);
};

}

#if USING_AK_GLOBALLY
using AK::DeprecatedStringCodePointIterator;
using AK::Utf8CodePointIterator;
using AK::Utf8View;
#endif
