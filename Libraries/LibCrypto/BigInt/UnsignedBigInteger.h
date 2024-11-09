/*
 * Copyright (c) 2020, Itamar S. <itamar8910@gmail.com>
 * Copyright (c) 2022, the SerenityOS developers.
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/BigIntBase.h>
#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Concepts.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <AK/Vector.h>

namespace Crypto {

struct UnsignedDivisionResult;
constexpr size_t STARTING_WORD_SIZE = 32;

class UnsignedBigInteger {
public:
    using Word = u32;
    using StorageSpan = AK::Detail::StorageSpan<Word, false>;
    using ConstStorageSpan = AK::Detail::StorageSpan<Word const, false>;
    static constexpr size_t BITS_IN_WORD = 32;

    // This constructor accepts any unsigned with size up to Word.
    template<Integral T>
    requires(sizeof(T) <= sizeof(Word))
    UnsignedBigInteger(T value)
    {
        m_words.append(static_cast<Word>(value));
    }

    explicit UnsignedBigInteger(Vector<Word, STARTING_WORD_SIZE>&& words)
        : m_words(move(words))
    {
    }

    explicit UnsignedBigInteger(u8 const* ptr, size_t length);

    explicit UnsignedBigInteger(double value);

    explicit UnsignedBigInteger(u64 value)
    {
        static_assert(sizeof(u64) == sizeof(Word) * 2);
        m_words.resize_and_keep_capacity(2);
        m_words[0] = static_cast<Word>(value & 0xFFFFFFFF);
        m_words[1] = static_cast<Word>((value >> 32) & 0xFFFFFFFF);
    }

    UnsignedBigInteger() = default;

    [[nodiscard]] static UnsignedBigInteger create_invalid();

    [[nodiscard]] static UnsignedBigInteger import_data(StringView data) { return import_data((u8 const*)data.characters_without_null_termination(), data.length()); }
    [[nodiscard]] static UnsignedBigInteger import_data(u8 const* ptr, size_t length)
    {
        return UnsignedBigInteger(ptr, length);
    }

    size_t export_data(Bytes, bool remove_leading_zeros = false) const;

    [[nodiscard]] static ErrorOr<UnsignedBigInteger> from_base(u16 N, StringView str);
    [[nodiscard]] ErrorOr<String> to_base(u16 N) const;
    [[nodiscard]] ByteString to_base_deprecated(u16 N) const;

    [[nodiscard]] u64 to_u64() const;

    enum class RoundingMode {
        IEEERoundAndTiesToEvenMantissa,
        RoundTowardZero,
        // “the Number value for x”, https://tc39.es/ecma262/#number-value-for
        ECMAScriptNumberValueFor = IEEERoundAndTiesToEvenMantissa,
    };

    [[nodiscard]] double to_double(RoundingMode rounding_mode = RoundingMode::IEEERoundAndTiesToEvenMantissa) const;

    [[nodiscard]] Vector<Word, STARTING_WORD_SIZE> const& words() const { return m_words; }

    void set_to_0();
    void set_to(Word other);
    void set_to(UnsignedBigInteger const& other);

    void invalidate()
    {
        m_is_invalid = true;
        m_cached_trimmed_length = {};
        m_cached_hash = 0;
    }

    [[nodiscard]] bool is_zero() const;
    [[nodiscard]] bool is_odd() const { return m_words.size() && (m_words[0] & 1); }
    [[nodiscard]] bool is_invalid() const { return m_is_invalid; }

    [[nodiscard]] size_t length() const { return m_words.size(); }
    // The "trimmed length" is the number of words after trimming leading zeroed words
    [[nodiscard]] size_t trimmed_length() const;

    [[nodiscard]] size_t byte_length() const { return length() * sizeof(Word); }
    [[nodiscard]] size_t trimmed_byte_length() const { return trimmed_length() * sizeof(Word); }

    void clamp_to_trimmed_length();
    void resize_with_leading_zeros(size_t num_words);

    size_t one_based_index_of_highest_set_bit() const;

    [[nodiscard]] UnsignedBigInteger plus(UnsignedBigInteger const& other) const;
    [[nodiscard]] UnsignedBigInteger minus(UnsignedBigInteger const& other) const;
    [[nodiscard]] UnsignedBigInteger bitwise_or(UnsignedBigInteger const& other) const;
    [[nodiscard]] UnsignedBigInteger bitwise_and(UnsignedBigInteger const& other) const;
    [[nodiscard]] UnsignedBigInteger bitwise_xor(UnsignedBigInteger const& other) const;
    [[nodiscard]] UnsignedBigInteger bitwise_not_fill_to_one_based_index(size_t) const;
    [[nodiscard]] UnsignedBigInteger shift_left(size_t num_bits) const;
    [[nodiscard]] UnsignedBigInteger shift_right(size_t num_bits) const;
    [[nodiscard]] UnsignedBigInteger multiplied_by(UnsignedBigInteger const& other) const;
    [[nodiscard]] UnsignedDivisionResult divided_by(UnsignedBigInteger const& divisor) const;

    [[nodiscard]] u32 hash() const;

    void set_bit_inplace(size_t bit_index);

    [[nodiscard]] bool operator==(UnsignedBigInteger const& other) const;
    [[nodiscard]] bool operator!=(UnsignedBigInteger const& other) const;
    [[nodiscard]] bool operator<(UnsignedBigInteger const& other) const;
    [[nodiscard]] bool operator>(UnsignedBigInteger const& other) const;
    [[nodiscard]] bool operator>=(UnsignedBigInteger const& other) const;

    enum class CompareResult {
        DoubleEqualsBigInt,
        DoubleLessThanBigInt,
        DoubleGreaterThanBigInt
    };

    [[nodiscard]] CompareResult compare_to_double(double) const;

private:
    friend class UnsignedBigIntegerAlgorithms;

    // Little endian
    // m_word[0] + m_word[1] * Word::MAX + m_word[2] * Word::MAX * Word::MAX + ...
    Vector<Word, STARTING_WORD_SIZE> m_words;
    StorageSpan words_span() { return { m_words.data(), m_words.size() }; }
    ConstStorageSpan words_span() const
    {
        return { m_words.data(), m_words.size() };
    }

    mutable u32 m_cached_hash { 0 };

    // Used to indicate a negative result, or a result of an invalid operation
    bool m_is_invalid { false };

    mutable Optional<size_t> m_cached_trimmed_length;
};

struct UnsignedDivisionResult {
    Crypto::UnsignedBigInteger quotient;
    Crypto::UnsignedBigInteger remainder;
};

}

template<>
struct AK::Formatter<Crypto::UnsignedBigInteger> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder&, Crypto::UnsignedBigInteger const&);
};

inline Crypto::UnsignedBigInteger
operator""_bigint(char const* string, size_t length)
{
    return MUST(Crypto::UnsignedBigInteger::from_base(10, { string, length }));
}
