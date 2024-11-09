/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>

namespace Crypto {

struct SignedDivisionResult;

class SignedBigInteger {
public:
    template<Signed T>
    requires(sizeof(T) <= sizeof(i32))
    SignedBigInteger(T value)
        : m_sign(value < 0)
        , m_unsigned_data(abs(static_cast<i32>(value)))
    {
    }

    SignedBigInteger(UnsignedBigInteger&& unsigned_data, bool sign)
        : m_sign(sign)
        , m_unsigned_data(move(unsigned_data))
    {
        ensure_sign_is_valid();
    }

    explicit SignedBigInteger(UnsignedBigInteger unsigned_data)
        : m_sign(false)
        , m_unsigned_data(move(unsigned_data))
    {
    }

    SignedBigInteger()
        : m_sign(false)
        , m_unsigned_data()
    {
    }

    explicit SignedBigInteger(double value);

    explicit SignedBigInteger(i64 value)
        : m_sign(value < 0)
        , m_unsigned_data(value < 0 ? static_cast<u64>(-(value + 1)) + 1 : static_cast<u64>(value))
    {
    }

    [[nodiscard]] static SignedBigInteger create_invalid()
    {
        return { UnsignedBigInteger::create_invalid(), false };
    }

    [[nodiscard]] static SignedBigInteger import_data(StringView data) { return import_data((u8 const*)data.characters_without_null_termination(), data.length()); }
    [[nodiscard]] static SignedBigInteger import_data(u8 const* ptr, size_t length);

    size_t export_data(Bytes, bool remove_leading_zeros = false) const;

    [[nodiscard]] static ErrorOr<SignedBigInteger> from_base(u16 N, StringView str);
    [[nodiscard]] ErrorOr<String> to_base(u16 N) const;
    [[nodiscard]] ByteString to_base_deprecated(u16 N) const;

    [[nodiscard]] u64 to_u64() const;
    [[nodiscard]] double to_double(UnsignedBigInteger::RoundingMode rounding_mode = UnsignedBigInteger::RoundingMode::IEEERoundAndTiesToEvenMantissa) const;

    [[nodiscard]] UnsignedBigInteger const& unsigned_value() const { return m_unsigned_data; }
    [[nodiscard]] Vector<u32, STARTING_WORD_SIZE> const words() const { return m_unsigned_data.words(); }
    [[nodiscard]] bool is_positive() const { return !is_negative() && !is_zero(); }
    [[nodiscard]] bool is_negative() const { return m_sign; }
    [[nodiscard]] bool is_zero() const { return m_unsigned_data.is_zero(); }

    void negate()
    {
        if (!m_unsigned_data.is_zero())
            m_sign = !m_sign;
    }

    void set_to_0()
    {
        m_unsigned_data.set_to_0();
        m_sign = false;
    }

    void set_to(i32 other)
    {
        m_unsigned_data.set_to((u32)other);
        m_sign = other < 0;
    }
    void set_to(SignedBigInteger const& other)
    {
        m_unsigned_data.set_to(other.m_unsigned_data);
        m_sign = other.m_sign;
    }

    void invalidate()
    {
        m_unsigned_data.invalidate();
    }

    [[nodiscard]] bool is_invalid() const { return m_unsigned_data.is_invalid(); }

    // These get + 1 byte for the sign.
    [[nodiscard]] size_t length() const { return m_unsigned_data.length() + 1; }
    [[nodiscard]] size_t trimmed_length() const { return m_unsigned_data.trimmed_length() + 1; }

    [[nodiscard]] SignedBigInteger plus(SignedBigInteger const& other) const;
    [[nodiscard]] SignedBigInteger minus(SignedBigInteger const& other) const;
    [[nodiscard]] SignedBigInteger bitwise_or(SignedBigInteger const& other) const;
    [[nodiscard]] SignedBigInteger bitwise_and(SignedBigInteger const& other) const;
    [[nodiscard]] SignedBigInteger bitwise_xor(SignedBigInteger const& other) const;
    [[nodiscard]] SignedBigInteger bitwise_not() const;
    [[nodiscard]] SignedBigInteger shift_left(size_t num_bits) const;
    [[nodiscard]] SignedBigInteger shift_right(size_t num_bits) const;
    [[nodiscard]] SignedBigInteger multiplied_by(SignedBigInteger const& other) const;
    [[nodiscard]] SignedDivisionResult divided_by(SignedBigInteger const& divisor) const;

    [[nodiscard]] SignedBigInteger plus(UnsignedBigInteger const& other) const;
    [[nodiscard]] SignedBigInteger minus(UnsignedBigInteger const& other) const;
    [[nodiscard]] SignedBigInteger multiplied_by(UnsignedBigInteger const& other) const;
    [[nodiscard]] SignedDivisionResult divided_by(UnsignedBigInteger const& divisor) const;

    [[nodiscard]] SignedBigInteger negated_value() const;

    [[nodiscard]] u32 hash() const;

    void set_bit_inplace(size_t bit_index);

    [[nodiscard]] bool operator==(SignedBigInteger const& other) const;
    [[nodiscard]] bool operator!=(SignedBigInteger const& other) const;
    [[nodiscard]] bool operator<(SignedBigInteger const& other) const;
    [[nodiscard]] bool operator<=(SignedBigInteger const& other) const;
    [[nodiscard]] bool operator>(SignedBigInteger const& other) const;
    [[nodiscard]] bool operator>=(SignedBigInteger const& other) const;

    [[nodiscard]] bool operator==(UnsignedBigInteger const& other) const;
    [[nodiscard]] bool operator!=(UnsignedBigInteger const& other) const;
    [[nodiscard]] bool operator<(UnsignedBigInteger const& other) const;
    [[nodiscard]] bool operator>(UnsignedBigInteger const& other) const;

    [[nodiscard]] UnsignedBigInteger::CompareResult compare_to_double(double) const;

private:
    void ensure_sign_is_valid()
    {
        if (m_sign && m_unsigned_data.is_zero())
            m_sign = false;
    }

    bool m_sign { false };
    UnsignedBigInteger m_unsigned_data;
};

struct SignedDivisionResult {
    Crypto::SignedBigInteger quotient;
    Crypto::SignedBigInteger remainder;
};

}

template<>
struct AK::Formatter<Crypto::SignedBigInteger> : AK::Formatter<Crypto::UnsignedBigInteger> {
    ErrorOr<void> format(FormatBuilder&, Crypto::SignedBigInteger const&);
};

inline Crypto::SignedBigInteger
operator""_sbigint(char const* string, size_t length)
{
    return MUST(Crypto::SignedBigInteger::from_base(10, { string, length }));
}
