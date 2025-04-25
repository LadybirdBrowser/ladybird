/*
 * Copyright (c) 2020, Itamar S. <itamar8910@gmail.com>
 * Copyright (c) 2022, the SerenityOS developers.
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/String.h>
#include <LibCrypto/BigInt/TommathForward.h>

namespace Crypto {

struct UnsignedDivisionResult;

class UnsignedBigInteger {
public:
    template<Integral T>
    UnsignedBigInteger(T value)
        : UnsignedBigInteger(static_cast<u64>(value))
    {
    }
    UnsignedBigInteger(u8 const* ptr, size_t length);

    explicit UnsignedBigInteger(Vector<u32> const& words);
    explicit UnsignedBigInteger(double value);
    explicit UnsignedBigInteger(u64 value);

    UnsignedBigInteger(UnsignedBigInteger const&);
    UnsignedBigInteger& operator=(UnsignedBigInteger const&);

    UnsignedBigInteger();
    ~UnsignedBigInteger();

    [[nodiscard]] static UnsignedBigInteger import_data(StringView data) { return import_data(reinterpret_cast<u8 const*>(data.characters_without_null_termination()), data.length()); }
    [[nodiscard]] static UnsignedBigInteger import_data(u8 const* ptr, size_t length) { return UnsignedBigInteger(ptr, length); }

    size_t export_data(Bytes) const;

    [[nodiscard]] static ErrorOr<UnsignedBigInteger> from_base(u16 N, StringView str);
    [[nodiscard]] ErrorOr<String> to_base(u16 N) const;

    [[nodiscard]] u64 to_u64() const;

    enum class RoundingMode {
        IEEERoundAndTiesToEvenMantissa,
        RoundTowardZero,
        // “the Number value for x”, https://tc39.es/ecma262/#number-value-for
        ECMAScriptNumberValueFor = IEEERoundAndTiesToEvenMantissa,
    };

    [[nodiscard]] double to_double(RoundingMode rounding_mode = RoundingMode::IEEERoundAndTiesToEvenMantissa) const;

    [[nodiscard]] Vector<u32> words() const;

    void set_to_0();
    void set_to(u64 other);
    void set_to(UnsignedBigInteger const& other);

    [[nodiscard]] bool is_zero() const;
    [[nodiscard]] bool is_odd() const;

    [[nodiscard]] size_t byte_length() const;

    size_t one_based_index_of_highest_set_bit() const;

    [[nodiscard]] UnsignedBigInteger plus(UnsignedBigInteger const& other) const;
    [[nodiscard]] ErrorOr<UnsignedBigInteger> minus(UnsignedBigInteger const& other) const;
    [[nodiscard]] UnsignedBigInteger bitwise_or(UnsignedBigInteger const& other) const;
    [[nodiscard]] UnsignedBigInteger bitwise_and(UnsignedBigInteger const& other) const;
    [[nodiscard]] UnsignedBigInteger bitwise_xor(UnsignedBigInteger const& other) const;
    [[nodiscard]] ErrorOr<UnsignedBigInteger> bitwise_not_fill_to_one_based_index(size_t) const;
    [[nodiscard]] ErrorOr<UnsignedBigInteger> shift_left(size_t num_bits) const;
    [[nodiscard]] UnsignedBigInteger shift_right(size_t num_bits) const;
    [[nodiscard]] UnsignedBigInteger multiplied_by(UnsignedBigInteger const& other) const;
    [[nodiscard]] UnsignedDivisionResult divided_by(UnsignedBigInteger const& divisor) const;
    [[nodiscard]] UnsignedBigInteger pow(u32 exponent) const;
    [[nodiscard]] UnsignedBigInteger gcd(UnsignedBigInteger const& other) const;

    [[nodiscard]] u32 hash() const;

    [[nodiscard]] bool operator==(UnsignedBigInteger const& other) const;
    [[nodiscard]] bool operator!=(UnsignedBigInteger const& other) const;
    [[nodiscard]] bool operator<(UnsignedBigInteger const& other) const;
    [[nodiscard]] bool operator<=(UnsignedBigInteger const& other) const;
    [[nodiscard]] bool operator>(UnsignedBigInteger const& other) const;
    [[nodiscard]] bool operator>=(UnsignedBigInteger const& other) const;

    enum class CompareResult {
        DoubleEqualsBigInt,
        DoubleLessThanBigInt,
        DoubleGreaterThanBigInt
    };

    [[nodiscard]] CompareResult compare_to_double(double) const;

private:
    friend class SignedBigInteger;

    mp_int m_mp;
    mutable Optional<u32> m_hash {};
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

inline Crypto::UnsignedBigInteger operator""_bigint(char const* string, size_t length)
{
    return MUST(Crypto::UnsignedBigInteger::from_base(10, { string, length }));
}

inline Crypto::UnsignedBigInteger operator""_bigint(unsigned long long value)
{
    return Crypto::UnsignedBigInteger { static_cast<u64>(value) };
}

inline Crypto::UnsignedBigInteger operator""_bigint(long double value)
{
    VERIFY(value >= 0);
    VERIFY(value < static_cast<long double>(NumericLimits<double>::max()));

    return Crypto::UnsignedBigInteger { static_cast<double>(value) };
}
