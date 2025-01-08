/*
 * Copyright (c) 2019-2020, Sergey Bugaev <bugaevc@serenityos.org>
 * Copyright (c) 2021, Max Wipfli <mail@maxwipfli.ch>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Debug.h>
#include <AK/Format.h>
#include <AK/Function.h>
#include <AK/StringView.h>
#include <AK/Types.h>

namespace AK {

class Utf8View;

class Utf8CodePointIterator {
    friend class Utf8View;
    friend class ByteString;

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
    ReadonlyBytes underlying_code_point_bytes() const { return { m_ptr, underlying_code_point_length_in_bytes() }; }
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

    [[nodiscard]] size_t byte_offset_of(Utf8CodePointIterator const& it) const
    {
        VERIFY(it.m_ptr >= begin_ptr());
        VERIFY(it.m_ptr <= end_ptr());

        return it.m_ptr - begin_ptr();
    }

    size_t byte_offset_of(size_t code_point_offset) const;

    Utf8View substring_view(size_t byte_offset, size_t byte_length) const { return Utf8View { m_string.substring_view(byte_offset, byte_length) }; }
    Utf8View substring_view(size_t byte_offset) const { return substring_view(byte_offset, byte_length() - byte_offset); }
    Utf8View unicode_substring_view(size_t code_point_offset, size_t code_point_length) const;
    Utf8View unicode_substring_view(size_t code_point_offset) const { return unicode_substring_view(code_point_offset, length() - code_point_offset); }

    bool is_empty() const { return m_string.is_empty(); }
    bool is_null() const { return m_string.is_null(); }
    bool starts_with(Utf8View const&) const;
    bool contains(u32) const;
    bool contains_any_of(ReadonlySpan<u32>) const;

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

    template<typename Callback>
    auto for_each_split_view(Function<bool(u32)> splitter, SplitBehavior split_behavior, Callback callback) const
    {
        bool keep_empty = has_flag(split_behavior, SplitBehavior::KeepEmpty);
        bool keep_trailing_separator = has_flag(split_behavior, SplitBehavior::KeepTrailingSeparator);

        auto start_offset = 0u;
        auto offset = 0u;

        auto run_callback = [&]() {
            auto length = offset - start_offset;

            if (length == 0 && !keep_empty)
                return;

            auto substring = unicode_substring_view(start_offset, length);

            // Reject splitter-only entries if we're not keeping empty results
            if (keep_trailing_separator && !keep_empty && length == 1 && splitter(*substring.begin()))
                return;

            callback(substring);
        };

        auto iterator = begin();
        while (iterator != end()) {
            if (splitter(*iterator)) {
                if (keep_trailing_separator)
                    ++offset;

                run_callback();

                if (!keep_trailing_separator)
                    ++offset;

                start_offset = offset;
                ++iterator;
                continue;
            }

            ++offset;
            ++iterator;
        }
        run_callback();
    }

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

template<>
struct Formatter<Utf8View> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder&, Utf8View const&);
};

inline Utf8CodePointIterator& Utf8CodePointIterator::operator++()
{
    VERIFY(m_length > 0);

    // OPTIMIZATION: Fast path for ASCII characters.
    if (*m_ptr <= 0x7F) {
        m_ptr += 1;
        m_length -= 1;
        return *this;
    }

    size_t code_point_length_in_bytes = underlying_code_point_length_in_bytes();
    if (code_point_length_in_bytes > m_length) {
        // We don't have enough data for the next code point. Skip one character and try again.
        // The rest of the code will output replacement characters as needed for any eventual extension bytes we might encounter afterwards.
        dbgln_if(UTF8_DEBUG, "Expected code point size {} is too big for the remaining length {}. Moving forward one byte.", code_point_length_in_bytes, m_length);
        m_ptr += 1;
        m_length -= 1;
        return *this;
    }

    m_ptr += code_point_length_in_bytes;
    m_length -= code_point_length_in_bytes;
    return *this;
}

inline size_t Utf8CodePointIterator::underlying_code_point_length_in_bytes() const
{
    VERIFY(m_length > 0);
    auto [code_point_length_in_bytes, value, first_byte_makes_sense] = Utf8View::decode_leading_byte(*m_ptr);

    // If any of these tests fail, we will output a replacement character for this byte and treat it as a code point of size 1.
    if (!first_byte_makes_sense)
        return 1;

    if (code_point_length_in_bytes > m_length)
        return 1;

    for (size_t offset = 1; offset < code_point_length_in_bytes; offset++) {
        if (m_ptr[offset] >> 6 != 2)
            return 1;
    }

    return code_point_length_in_bytes;
}

inline u32 Utf8CodePointIterator::operator*() const
{
    VERIFY(m_length > 0);

    // OPTIMIZATION: Fast path for ASCII characters.
    if (*m_ptr <= 0x7F)
        return *m_ptr;

    auto [code_point_length_in_bytes, code_point_value_so_far, first_byte_makes_sense] = Utf8View::decode_leading_byte(*m_ptr);

    if (!first_byte_makes_sense) {
        // The first byte of the code point doesn't make sense: output a replacement character
        dbgln_if(UTF8_DEBUG, "First byte doesn't make sense: {:#02x}.", m_ptr[0]);
        return 0xFFFD;
    }

    if (code_point_length_in_bytes > m_length) {
        // There is not enough data left for the full code point: output a replacement character
        dbgln_if(UTF8_DEBUG, "Not enough bytes (need {}, have {}), first byte is: {:#02x}.", code_point_length_in_bytes, m_length, m_ptr[0]);
        return 0xFFFD;
    }

    for (size_t offset = 1; offset < code_point_length_in_bytes; offset++) {
        if (m_ptr[offset] >> 6 != 2) {
            // One of the extension bytes of the code point doesn't make sense: output a replacement character
            dbgln_if(UTF8_DEBUG, "Extension byte {:#02x} in {} position after first byte {:#02x} doesn't make sense.", m_ptr[offset], offset, m_ptr[0]);
            return 0xFFFD;
        }

        code_point_value_so_far <<= 6;
        code_point_value_so_far |= m_ptr[offset] & 63;
    }

    if (code_point_value_so_far > 0x10FFFF) {
        dbgln_if(UTF8_DEBUG, "Multi-byte sequence is otherwise valid, but code point {:#x} is not permissible.", code_point_value_so_far);
        return 0xFFFD;
    }
    return code_point_value_so_far;
}

}

#if USING_AK_GLOBALLY
using AK::Utf8CodePointIterator;
using AK::Utf8View;
#endif
