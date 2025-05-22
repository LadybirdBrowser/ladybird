/*
 * Copyright (c) 2021, Leon Albrecht <leon2002.la@gmail.com>
 * Copyright (c) 2023, Dan Klishch <danilklishch@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/BigIntBase.h>
#include <AK/BuiltinWrappers.h>
#include <AK/Checked.h>
#include <AK/Concepts.h>
#include <AK/Endian.h>
#include <AK/Format.h>
#include <AK/NumericLimits.h>
#include <AK/StdLibExtraDetails.h>
#include <AK/StdLibExtras.h>
#include <AK/StringBuilder.h>

namespace AK {

namespace Detail {

// As noted near the declaration of StaticStorage, bit_size is more like a hint for a storage size.
// The effective bit size is `sizeof(StaticStorage<...>) * 8`. It is a programmer's responsibility
// to ensure that the hinted bit_size is always greater than the actual integer size.
// That said, do not use unaligned (bit_size % 64 != 0) `UFixedBigInt`s if you do not know what you
// are doing.
template<size_t bit_size, typename Storage = StaticStorage<false, bit_size>>
class UFixedBigInt;

// ===== Concepts =====
template<typename T>
constexpr inline size_t assumed_bit_size = 0;
template<>
constexpr inline size_t assumed_bit_size<IntegerWrapper> = bit_width<int>;
template<size_t bit_size>
constexpr inline size_t assumed_bit_size<UFixedBigInt<bit_size>> = bit_size;
template<BuiltInUFixedInt T>
constexpr inline size_t assumed_bit_size<T> = bit_width<T>;

template<typename T>
concept ConvertibleToUFixedInt = (assumed_bit_size<T> != 0);

template<typename T>
concept UFixedInt = (ConvertibleToUFixedInt<T> && !IsSame<T, IntegerWrapper>);

template<typename T>
concept NotBuiltInUFixedInt = (UFixedInt<T> && !BuiltInUFixedInt<T>);

// ===== UFixedBigInt itself =====
template<size_t bit_size>
constexpr auto& get_storage_of(UFixedBigInt<bit_size>& value) { return value.m_data; }

template<size_t bit_size>
constexpr auto& get_storage_of(UFixedBigInt<bit_size> const& value) { return value.m_data; }

template<typename Operand1, typename Operand2, typename Result>
constexpr void mul_internal(Operand1 const& operand1, Operand2 const& operand2, Result& result)
{
    StorageOperations<>::baseline_mul(operand1, operand2, result, g_null_allocator);
}

template<size_t dividend_size, size_t divisor_size, bool restore_remainder>
constexpr void div_mod_internal( // Include AK/UFixedBigIntDivision.h to use UFixedBigInt division
    StaticStorage<false, dividend_size> const& dividend,
    StaticStorage<false, divisor_size> const& divisor,
    StaticStorage<false, dividend_size>& quotient,
    StaticStorage<false, divisor_size>& remainder);

template<size_t bit_size, typename Storage>
class UFixedBigInt {
    constexpr static size_t static_size = Storage::static_size;
    constexpr static size_t part_size = static_size / 2;
    using UFixedBigIntPart = Conditional<part_size * native_word_size <= 64, u64, UFixedBigInt<part_size * native_word_size>>;
    using Ops = StorageOperations<>;

public:
    constexpr UFixedBigInt() = default;

    explicit constexpr UFixedBigInt(IntegerWrapper value) { Ops::copy(value.m_data, m_data); }
    consteval UFixedBigInt(int value)
    {
        Ops::copy(IntegerWrapper(value).m_data, m_data);
    }

    template<UFixedInt T>
    requires(sizeof(T) > sizeof(Storage)) explicit constexpr UFixedBigInt(T const& value)
    {
        Ops::copy(get_storage_of(value), m_data);
    }

    template<UFixedInt T>
    requires(sizeof(T) <= sizeof(Storage)) constexpr UFixedBigInt(T const& value)
    {
        Ops::copy(get_storage_of(value), m_data);
    }

    constexpr UFixedBigInt(UFixedBigIntPart const& low, UFixedBigIntPart const& high)
    requires(static_size % 2 == 0)
    {
        decltype(auto) low_storage = get_storage_of(low);
        decltype(auto) high_storage = get_storage_of(high);
        for (size_t i = 0; i < part_size; ++i)
            m_data[i] = low_storage[i];
        for (size_t i = 0; i < part_size; ++i)
            m_data[i + part_size] = high_storage[i];
    }

    template<UFixedInt T, size_t n>
    requires((assumed_bit_size<T> * n) <= bit_size) constexpr UFixedBigInt(T const (&value)[n])
    {
        size_t offset = 0;

        for (size_t i = 0; i < n; ++i) {
            if (offset % native_word_size == 0) {
                // Aligned initialization (i. e. u256 from two u128)
                decltype(auto) storage = get_storage_of(value[i]);
                for (size_t i = 0; i < storage.size(); ++i)
                    m_data[i + offset / native_word_size] = storage[i];
            } else if (offset % native_word_size == 32 && IsSame<T, u32>) {
                // u32 vector initialization on 64-bit platforms
                m_data[offset / native_word_size] |= static_cast<NativeDoubleWord>(value[i]) << 32;
            } else {
                VERIFY_NOT_REACHED();
            }
            offset += assumed_bit_size<T>;
        }

        for (size_t i = (offset + native_word_size - 1) / native_word_size; i < m_data.size(); ++i)
            m_data[i] = 0;
    }

    // Casts & parts extraction
    template<NotBuiltInUFixedInt T>
    constexpr explicit operator T() const
    {
        T result;
        Ops::copy(m_data, result.m_data);
        return result;
    }

    template<BuiltInUFixedInt T>
    requires(sizeof(T) <= sizeof(NativeWord)) constexpr explicit operator T() const
    {
        return m_data[0];
    }

    template<BuiltInUFixedInt T>
    requires(sizeof(T) == sizeof(NativeDoubleWord)) constexpr explicit operator T() const
    {
        return (static_cast<NativeDoubleWord>(m_data[1]) << native_word_size) + m_data[0];
    }

    constexpr UFixedBigIntPart low() const
    requires(static_size % 2 == 0)
    {
        if constexpr (part_size == 1) {
            return m_data[0];
        } else if constexpr (IsSame<UFixedBigIntPart, NativeDoubleWord>) {
            return m_data[0] + (static_cast<NativeDoubleWord>(m_data[1]) << native_word_size);
        } else {
            UFixedBigInt<part_size * native_word_size> result;
            Ops::copy(m_data, result.m_data);
            return result;
        }
    }

    constexpr UFixedBigIntPart high() const
    requires(static_size % 2 == 0)
    {
        if constexpr (part_size == 1) {
            return m_data[part_size];
        } else if constexpr (IsSame<UFixedBigIntPart, NativeDoubleWord>) {
            return m_data[part_size] + (static_cast<NativeDoubleWord>(m_data[part_size + 1]) << native_word_size);
        } else {
            UFixedBigInt<part_size * native_word_size> result;
            Ops::copy(m_data, result.m_data, part_size);
            return result;
        }
    }

    Bytes bytes()
    {
        return Bytes(reinterpret_cast<u8*>(this), sizeof(Storage));
    }

    ReadonlyBytes bytes() const
    {
        return ReadonlyBytes(reinterpret_cast<u8 const*>(this), sizeof(Storage));
    }

    constexpr UnsignedStorageSpan span()
    {
        return { m_data.data(), static_size };
    }

    constexpr UnsignedStorageReadonlySpan span() const
    {
        return { m_data.data(), static_size };
    }

    // Binary utils
    constexpr size_t popcnt() const
    {
        size_t result = 0;
        for (size_t i = 0; i < m_data.size(); ++i)
            result += popcount(m_data[i]);
        return result;
    }

    constexpr size_t ctz() const
    {
        size_t result = 0;
        for (size_t i = 0; i < m_data.size(); ++i) {
            if (m_data[i]) {
                result += count_trailing_zeroes(m_data[i]);
                break;
            } else {
                result += native_word_size;
            }
        }
        return result;
    }

    constexpr size_t clz() const
    {
        size_t result = 0;
        for (size_t i = m_data.size(); i--;) {
            if (m_data[i]) {
                result += count_leading_zeroes(m_data[i]);
                break;
            } else {
                result += native_word_size;
            }
        }
        return result + bit_size - native_word_size * static_size;
    }

    // Comparisons
    constexpr bool operator!() const
    {
        bool result = true;
        for (size_t i = 0; i < m_data.size(); ++i)
            result &= !m_data[i];
        return result;
    }

    constexpr explicit operator bool() const
    {
        bool result = false;
        for (size_t i = 0; i < m_data.size(); ++i)
            result |= m_data[i];
        return result;
    }

    constexpr bool operator==(UFixedInt auto const& other) const
    {
        return Ops::compare(m_data, get_storage_of(other), true) == 0;
    }

    constexpr bool operator==(IntegerWrapper other) const
    {
        return Ops::compare(m_data, get_storage_of(other), true) == 0;
    }

    constexpr int operator<=>(UFixedInt auto const& other) const
    {
        return Ops::compare(m_data, get_storage_of(other), false);
    }

    constexpr int operator<=>(IntegerWrapper other) const
    {
        return Ops::compare(m_data, get_storage_of(other), false);
    }

#define DEFINE_STANDARD_BINARY_OPERATOR(op, function)                        \
    constexpr auto operator op(UFixedInt auto const& other) const            \
    {                                                                        \
        auto func = [](auto&& a, auto&& b, auto&& c) { function(a, b, c); }; \
        return do_standard_binary_operation(other, func);                    \
    }                                                                        \
                                                                             \
    constexpr auto operator op(IntegerWrapper other) const                   \
    {                                                                        \
        auto func = [](auto&& a, auto&& b, auto&& c) { function(a, b, c); }; \
        return do_standard_binary_operation(other, func);                    \
    }

#define DEFINE_STANDARD_COMPOUND_ASSIGNMENT(op, function)                    \
    constexpr auto& operator op(UFixedInt auto const& other)                 \
    {                                                                        \
        auto func = [](auto&& a, auto&& b, auto&& c) { function(a, b, c); }; \
        do_standard_compound_assignment(other, func);                        \
        return *this;                                                        \
    }                                                                        \
                                                                             \
    constexpr auto& operator op(IntegerWrapper other)                        \
    {                                                                        \
        auto func = [](auto&& a, auto&& b, auto&& c) { function(a, b, c); }; \
        do_standard_compound_assignment(other, func);                        \
        return *this;                                                        \
    }

    // Binary operators
    DEFINE_STANDARD_BINARY_OPERATOR(^, Ops::compute_bitwise<Ops::Bitwise::XOR>)
    DEFINE_STANDARD_BINARY_OPERATOR(&, Ops::compute_bitwise<Ops::Bitwise::AND>)
    DEFINE_STANDARD_BINARY_OPERATOR(|, Ops::compute_bitwise<Ops::Bitwise::OR>)
    DEFINE_STANDARD_COMPOUND_ASSIGNMENT(^=, Ops::compute_inplace_bitwise<Ops::Bitwise::XOR>)
    DEFINE_STANDARD_COMPOUND_ASSIGNMENT(&=, Ops::compute_inplace_bitwise<Ops::Bitwise::AND>)
    DEFINE_STANDARD_COMPOUND_ASSIGNMENT(|=, Ops::compute_inplace_bitwise<Ops::Bitwise::OR>)

    constexpr auto operator~() const
    {
        UFixedBigInt<bit_size> result;
        Ops::compute_bitwise<Ops::Bitwise::INVERT>(m_data, m_data, result.m_data);
        return result;
    }

    constexpr auto operator<<(size_t shift) const
    {
        UFixedBigInt<bit_size> result;
        Ops::shift_left(m_data, shift, result.m_data);
        return result;
    }

    constexpr auto& operator<<=(size_t shift)
    {
        Ops::shift_left(m_data, shift, m_data);
        return *this;
    }

    constexpr auto operator>>(size_t shift) const
    {
        UFixedBigInt<bit_size> result;
        Ops::shift_right(m_data, shift, result.m_data);
        return result;
    }

    constexpr auto& operator>>=(size_t shift)
    {
        Ops::shift_right(m_data, shift, m_data);
        return *this;
    }

    // Arithmetic
    template<UFixedInt T>
    constexpr auto addc(T const& other, bool& carry) const
    {
        UFixedBigInt<max(bit_size, assumed_bit_size<T>)> result;
        carry = Ops::add<false>(m_data, get_storage_of(other), result.m_data, carry);
        return result;
    }

    template<UFixedInt T>
    constexpr auto subc(T const& other, bool& borrow) const
    {
        UFixedBigInt<max(bit_size, assumed_bit_size<T>)> result;
        borrow = Ops::add<true>(m_data, get_storage_of(other), result.m_data, borrow);
        return result;
    }

    DEFINE_STANDARD_BINARY_OPERATOR(+, Ops::add<false>)
    DEFINE_STANDARD_BINARY_OPERATOR(-, Ops::add<true>)
    DEFINE_STANDARD_COMPOUND_ASSIGNMENT(+=, Ops::add<false>)
    DEFINE_STANDARD_COMPOUND_ASSIGNMENT(-=, Ops::add<true>)

    constexpr auto& operator++()
    {
        Ops::increment<false>(m_data);
        return *this;
    }

    constexpr auto& operator--()
    {
        Ops::increment<true>(m_data);
        return *this;
    }

    constexpr auto operator++(int)
    {
        UFixedBigInt<bit_size> result = *this;
        Ops::increment<false>(m_data);
        return result;
    }

    constexpr auto operator--(int)
    {
        UFixedBigInt<bit_size> result = *this;
        Ops::increment<true>(m_data);
        return result;
    }

    DEFINE_STANDARD_BINARY_OPERATOR(*, mul_internal)

    constexpr auto& operator*=(UFixedInt auto const& other) { return *this = *this * other; }
    constexpr auto& operator*=(IntegerWrapper const& other) { return *this = *this * other; }

    template<UFixedInt T>
    constexpr auto wide_multiply(T const& other) const
    {
        UFixedBigInt<bit_size + assumed_bit_size<T>> result;
        mul_internal(m_data, get_storage_of(other), result.m_data);
        return result;
    }

    template<NotBuiltInUFixedInt T>
    constexpr UFixedBigInt<bit_size> div_mod(T const& divisor, T& remainder) const
    {
        UFixedBigInt<bit_size> quotient;
        UFixedBigInt<assumed_bit_size<T>> resulting_remainder;
        div_mod_internal<bit_size, assumed_bit_size<T>, true>(m_data, get_storage_of(divisor), get_storage_of(quotient), get_storage_of(resulting_remainder));
        remainder = resulting_remainder;
        return quotient;
    }

    template<UFixedInt T>
    constexpr auto operator/(T const& other) const
    {
        UFixedBigInt<bit_size> quotient;
        StaticStorage<false, assumed_bit_size<T>> remainder; // unused
        div_mod_internal<bit_size, assumed_bit_size<T>, false>(m_data, get_storage_of(other), get_storage_of(quotient), remainder);
        return quotient;
    }

    template<UFixedInt T>
    constexpr auto operator%(T const& other) const
    {
        StaticStorage<false, bit_size> quotient; // unused
        UFixedBigInt<assumed_bit_size<T>> remainder;
        div_mod_internal<bit_size, assumed_bit_size<T>, true>(m_data, get_storage_of(other), quotient, get_storage_of(remainder));
        return remainder;
    }

    constexpr auto operator/(IntegerWrapper const& other) const { return *this / static_cast<UFixedBigInt<32>>(other); }
    constexpr auto operator%(IntegerWrapper const& other) const { return *this % static_cast<UFixedBigInt<32>>(other); }

    template<UFixedInt T>
    constexpr auto& operator/=(T const& other) { return *this = *this / other; }
    constexpr auto& operator/=(IntegerWrapper const& other) { return *this = *this / other; }

    template<Unsigned U>
    constexpr auto& operator%=(U const& other) { return *this = *this % other; }
    constexpr auto& operator%=(IntegerWrapper const& other) { return *this = *this % other; }

    // Note: If there ever be need for non side-channel proof sqrt/pow/pow_mod of UFixedBigInt, you
    //       can restore them from Git history.

#undef DEFINE_STANDARD_BINARY_OPERATOR
#undef DEFINE_STANDARD_COMPOUND_ASSIGNMENT

    // These functions are intended to be used in LibCrypto for equality checks without branching.
    constexpr bool is_zero_constant_time() const
    {
        NativeWord fold = 0;
        for (size_t i = 0; i < m_data.size(); ++i)
            taint_for_optimizer(fold |= m_data[i]);
        return !fold;
    }

    constexpr bool is_equal_to_constant_time(UFixedBigInt<bit_size> other) const
    {
        NativeWord fold = 0;
        for (size_t i = 0; i < m_data.size(); ++i)
            taint_for_optimizer(fold |= m_data[i] ^ other.m_data[i]);
        return !fold;
    }

private:
    template<ConvertibleToUFixedInt T, typename Function>
    constexpr auto do_standard_binary_operation(T const& other, Function function) const
    {
        UFixedBigInt<max(bit_size, assumed_bit_size<T>)> result;
        function(m_data, get_storage_of(other), result.m_data);
        return result;
    }

    template<ConvertibleToUFixedInt T, typename Function>
    constexpr void do_standard_compound_assignment(T const& other, Function function)
    {
        static_assert(bit_size >= assumed_bit_size<T>, "Requested operation requires integer size to be expanded.");
        function(m_data, get_storage_of(other), m_data);
    }

    template<size_t other_bit_size, typename OtherStorage>
    friend class UFixedBigInt;

    friend constexpr auto& get_storage_of<bit_size>(UFixedBigInt<bit_size>&);
    friend constexpr auto& get_storage_of<bit_size>(UFixedBigInt<bit_size> const&);

    Storage m_data;
};

// FIXME: There is a bug in LLVM (https://github.com/llvm/llvm-project/issues/59783) which doesn't
//        allow to use the following comparisons.
bool operator==(BuiltInUFixedInt auto const& a, NotBuiltInUFixedInt auto const& b) { return b.operator==(a); }
int operator<=>(BuiltInUFixedInt auto const& a, NotBuiltInUFixedInt auto const& b) { return -b.operator<=>(a); }
bool operator==(IntegerWrapper const& a, NotBuiltInUFixedInt auto const& b) { return b.operator==(a); }
int operator<=>(IntegerWrapper const& a, NotBuiltInUFixedInt auto const& b) { return -b.operator<=>(a); }

}

using Detail::UFixedBigInt;

template<size_t bit_size>
constexpr inline bool IsUnsigned<UFixedBigInt<bit_size>> = true;
template<size_t bit_size>
constexpr inline bool IsSigned<UFixedBigInt<bit_size>> = false;

template<size_t bit_size>
struct NumericLimits<UFixedBigInt<bit_size>> {
    using T = UFixedBigInt<bit_size>;

    static constexpr T min() { return T {}; }
    static constexpr T max() { return --T {}; }
    static constexpr bool is_signed() { return false; }
};

template<size_t N>
class LittleEndian<UFixedBigInt<N>> {
    template<size_t M>
    constexpr static auto byte_swap_if_not_little_endian(UFixedBigInt<M> value)
    {
        if constexpr (HostIsLittleEndian) {
            return value;
        } else {
            auto words = value.span();
            auto front_it = words.begin();
            auto ending_half_words = words.slice(ceil_div(words.size(), static_cast<size_t>(2)));
            for (size_t i = 0; i < ending_half_words.size(); ++i, ++front_it)
                *front_it = convert_between_host_and_little_endian(exchange(ending_half_words[ending_half_words.size() - i - 1], convert_between_host_and_little_endian(*front_it)));
            if (words.size() % 2)
                words[words.size() / 2] = convert_between_host_and_little_endian(*front_it);
            return value;
        }
    }

public:
    constexpr LittleEndian() = default;

    constexpr LittleEndian(UFixedBigInt<N> value)
        : m_value(byte_swap_if_not_little_endian(value))
    {
    }

    constexpr operator UFixedBigInt<N>() const { return byte_swap_if_not_little_endian(m_value); }

private:
    UFixedBigInt<N> m_value { 0 };
};

template<size_t N>
class BigEndian<UFixedBigInt<N>> {
    template<size_t M>
    constexpr static auto byte_swap_if_not_big_endian(UFixedBigInt<M> value)
    {
        if constexpr (!HostIsLittleEndian) {
            return value;
        } else {
            auto words = value.span();
            auto front_it = words.begin();
            auto ending_half_words = words.slice(ceil_div(words.size(), static_cast<size_t>(2)));
            for (size_t i = 0; i < ending_half_words.size(); ++i, ++front_it)
                *front_it = convert_between_host_and_big_endian(exchange(ending_half_words[ending_half_words.size() - i - 1], convert_between_host_and_big_endian(*front_it)));
            if (words.size() % 2)
                words[words.size() / 2] = convert_between_host_and_big_endian(*front_it);
            return value;
        }
    }

public:
    constexpr BigEndian() = default;

    constexpr BigEndian(UFixedBigInt<N> value)
        : m_value(byte_swap_if_not_big_endian(value))
    {
    }

    constexpr operator UFixedBigInt<N>() const { return byte_swap_if_not_big_endian(m_value); }

private:
    UFixedBigInt<N> m_value { 0 };
};

template<size_t M>
struct Traits<UFixedBigInt<M>> : public DefaultTraits<UFixedBigInt<M>> {
    static constexpr bool is_trivially_serializable() { return true; }
    static constexpr bool is_trivial() { return true; }
};

// ===== Formatting =====
// FIXME: This does not work for size != 2 ** x
template<Detail::NotBuiltInUFixedInt T>
struct Formatter<T> : StandardFormatter {
    Formatter() = default;
    explicit Formatter(StandardFormatter formatter)
        : StandardFormatter(formatter)
    {
    }

    ErrorOr<void> format(FormatBuilder& builder, T const& value)
    {
        using U = decltype(value.low());

        if (m_precision.has_value())
            VERIFY_NOT_REACHED();

        if (m_mode == Mode::Pointer) {
            // these are way to big for a pointer
            VERIFY_NOT_REACHED();
        }
        if (m_mode == Mode::Default)
            m_mode = Mode::Hexadecimal;

        if (!value.high()) {
            Formatter<U> formatter { *this };
            return formatter.format(builder, value.low());
        }

        u8 base = 0;
        if (m_mode == Mode::Binary) {
            base = 2;
        } else if (m_mode == Mode::BinaryUppercase) {
            base = 2;
        } else if (m_mode == Mode::Octal) {
            TODO();
        } else if (m_mode == Mode::Decimal) {
            TODO();
        } else if (m_mode == Mode::Hexadecimal) {
            base = 16;
        } else if (m_mode == Mode::HexadecimalUppercase) {
            base = 16;
        } else {
            VERIFY_NOT_REACHED();
        }
        ssize_t width = m_width.value_or(0);
        ssize_t lower_length = ceil_div(Detail::assumed_bit_size<U>, (ssize_t)base);
        Formatter<U> formatter { *this };
        formatter.m_width = max(width - lower_length, (ssize_t)0);
        TRY(formatter.format(builder, value.high()));
        TRY(builder.put_literal("'"sv));
        formatter.m_zero_pad = true;
        formatter.m_alternative_form = false;
        formatter.m_width = lower_length;
        TRY(formatter.format(builder, value.low()));
        return {};
    }
};

}

// these sizes should suffice for most usecases
using u128 = AK::UFixedBigInt<128>;
using u256 = AK::UFixedBigInt<256>;
using u384 = AK::UFixedBigInt<384>;
using u512 = AK::UFixedBigInt<512>;
using u768 = AK::UFixedBigInt<768>;
using u1024 = AK::UFixedBigInt<1024>;
using u1536 = AK::UFixedBigInt<1536>;
using u2048 = AK::UFixedBigInt<2048>;
using u4096 = AK::UFixedBigInt<4096>;
