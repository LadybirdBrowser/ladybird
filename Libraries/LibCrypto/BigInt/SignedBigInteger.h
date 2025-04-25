/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCrypto/BigInt/UnsignedBigInteger.h>

namespace Crypto {

struct SignedDivisionResult;

class SignedBigInteger {
public:
    template<Signed T>
    SignedBigInteger(T value)
        : SignedBigInteger(static_cast<i64>(value))
    {
    }
    SignedBigInteger(UnsignedBigInteger&& unsigned_data, bool sign);
    SignedBigInteger(u8 const* ptr, size_t length);

    explicit SignedBigInteger(UnsignedBigInteger const& unsigned_data);
    explicit SignedBigInteger(double value);
    explicit SignedBigInteger(i64 value);

    SignedBigInteger(SignedBigInteger const&);
    SignedBigInteger& operator=(SignedBigInteger const&);

    SignedBigInteger();
    ~SignedBigInteger();

    [[nodiscard]] static SignedBigInteger import_data(StringView data) { return import_data(reinterpret_cast<u8 const*>(data.characters_without_null_termination()), data.length()); }
    [[nodiscard]] static SignedBigInteger import_data(u8 const* ptr, size_t length) { return SignedBigInteger(ptr, length); }

    size_t export_data(Bytes) const;

    [[nodiscard]] static ErrorOr<SignedBigInteger> from_base(u16 N, StringView str);
    [[nodiscard]] ErrorOr<String> to_base(u16 N) const;

    [[nodiscard]] u64 to_u64() const;
    [[nodiscard]] double to_double(UnsignedBigInteger::RoundingMode rounding_mode = UnsignedBigInteger::RoundingMode::IEEERoundAndTiesToEvenMantissa) const;

    [[nodiscard]] UnsignedBigInteger unsigned_value() const;
    [[nodiscard]] bool is_positive() const { return !is_negative() && !is_zero(); }
    [[nodiscard]] bool is_negative() const;
    [[nodiscard]] bool is_zero() const;

    void negate();
    void set_to_0();
    void set_to(i64 other);
    void set_to(SignedBigInteger const& other);

    [[nodiscard]] size_t byte_length() const;

    [[nodiscard]] SignedBigInteger plus(SignedBigInteger const& other) const;
    [[nodiscard]] SignedBigInteger minus(SignedBigInteger const& other) const;
    [[nodiscard]] SignedBigInteger bitwise_or(SignedBigInteger const& other) const;
    [[nodiscard]] SignedBigInteger bitwise_and(SignedBigInteger const& other) const;
    [[nodiscard]] SignedBigInteger bitwise_xor(SignedBigInteger const& other) const;
    [[nodiscard]] SignedBigInteger bitwise_not() const;
    [[nodiscard]] ErrorOr<SignedBigInteger> shift_left(size_t num_bits) const;
    [[nodiscard]] SignedBigInteger shift_right(size_t num_bits) const;
    [[nodiscard]] SignedBigInteger multiplied_by(SignedBigInteger const& other) const;
    [[nodiscard]] SignedDivisionResult divided_by(SignedBigInteger const& divisor) const;
    [[nodiscard]] SignedBigInteger pow(u32 exponent) const;
    [[nodiscard]] ErrorOr<SignedBigInteger> mod_power_of_two(size_t power_of_two) const;

    [[nodiscard]] SignedBigInteger plus(UnsignedBigInteger const& other) const;
    [[nodiscard]] SignedBigInteger minus(UnsignedBigInteger const& other) const;
    [[nodiscard]] SignedBigInteger multiplied_by(UnsignedBigInteger const& other) const;
    [[nodiscard]] SignedDivisionResult divided_by(UnsignedBigInteger const& divisor) const;

    [[nodiscard]] SignedBigInteger negated_value() const;

    [[nodiscard]] u32 hash() const;

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
    mp_int m_mp {};
    mutable Optional<u32> m_hash {};
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
