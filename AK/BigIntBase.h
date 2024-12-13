/*
 * Copyright (c) 2023, Dan Klishch <danilklishch@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/BuiltinWrappers.h>
#include <AK/Span.h>
#include <AK/StdLibExtras.h>
#include <AK/Types.h>

namespace AK {

namespace Detail {

template<typename T>
struct DoubleWordHelper;

template<>
struct DoubleWordHelper<u32> {
    using Type = u64;
    using SignedType = i64;
};
template<typename T>
using DoubleWord = typename DoubleWordHelper<T>::Type;
template<typename T>
using SignedDoubleWord = typename DoubleWordHelper<T>::SignedType;

// Ideally, we want to store data in the native processor's words. However, for some algorithms,
// particularly multiplication, we require double of the amount of the native word size.
#if defined(__SIZEOF_INT128__) && defined(AK_ARCH_64_BIT)
template<>
struct DoubleWordHelper<u64> {
    using Type = unsigned __int128;
    using SignedType = __int128;
};
using NativeWord = u64;
#else
using NativeWord = u32;
#endif

using NativeDoubleWord = DoubleWord<NativeWord>;
using SignedNativeDoubleWord = SignedDoubleWord<NativeWord>;

template<typename WordType, bool sign>
using ConditionallySignedDoubleWord = Conditional<sign, SignedDoubleWord<WordType>, DoubleWord<WordType>>;

template<typename T>
concept BuiltInUFixedInt = OneOf<T, bool, u8, u16, u32, u64, unsigned long, unsigned long long, NativeDoubleWord>;

template<typename T>
constexpr inline size_t bit_width = sizeof(T) * 8;

constexpr size_t native_word_size = bit_width<NativeWord>;
constexpr NativeWord max_native_word = NumericLimits<NativeWord>::max();
static_assert(native_word_size == 32 || native_word_size == 64);

// Max big integer length is 256 MiB (2.1e9 bits) for 32-bit, 4 GiB (3.4e10 bits) for 64-bit.
constexpr size_t max_big_int_length = 1 << (native_word_size == 32 ? 26 : 29);

// ===== Static storage for big integers =====
template<typename T, typename WordType = NativeWord>
concept IntegerStorage = requires(T storage, size_t index) {
    {
        storage.is_negative()
    } -> SameAs<bool>;
    {
        storage.size()
    } -> SameAs<size_t>;
    {
        storage[index]
    } -> ConvertibleTo<WordType&>;
    {
        storage.data()
    } -> ConvertibleTo<WordType*>;
};

template<typename T, typename WordType = NativeWord>
concept IntegerReadonlyStorage = IntegerStorage<T, WordType const>;

struct NullAllocator {
    NativeWord* allocate(size_t) { VERIFY_NOT_REACHED(); }
};

template<typename Word, bool is_signed_>
struct StorageSpan : AK::Span<Word> {
    using AK::Span<Word>::Span;

    constexpr static bool is_signed = is_signed_;

    explicit constexpr StorageSpan(AK::Span<Word> span)
        : AK::Span<Word>(span)
    {
    }

    constexpr bool is_negative() const
    {
        return is_signed && this->last() >> (bit_width<Word> - 1);
    }
};

using UnsignedStorageSpan = StorageSpan<NativeWord, false>;
using UnsignedStorageReadonlySpan = StorageSpan<NativeWord const, false>;

// Sometimes we want to know the exact maximum amount of the bits required to represent the number.
// However, the bit size only acts as a hint for wide multiply operations. For all other purposes,
// `bit_size`-sized and `ceil(bit_size / word_size) * word_size`-sized `StaticStorage`s will act the
// same.
template<bool is_signed_, size_t bit_size>
requires(bit_size <= max_big_int_length * native_word_size) struct StaticStorage {
    constexpr static size_t static_size = (bit_size + native_word_size - 1) / native_word_size;
    constexpr static bool is_signed = is_signed_;

    // We store integers in little-endian regardless of the host endianness. We use two's complement
    // representation of negative numbers and do not bother at all if `bit_size % word_size != 0`
    // (i. e. do not properly handle overflows).
    NativeWord m_data[static_size];

    constexpr bool is_negative() const
    {
        return is_signed_ && m_data[static_size - 1] >> (native_word_size - 1);
    }

    constexpr static size_t size()
    {
        return static_size;
    }

    constexpr NativeWord operator[](size_t i) const
    {
        return m_data[i];
    }

    constexpr NativeWord& operator[](size_t i)
    {
        return m_data[i];
    }

    constexpr NativeWord const* data() const
    {
        return m_data;
    }

    constexpr NativeWord* data()
    {
        return m_data;
    }

    constexpr operator StorageSpan<NativeWord, is_signed>() { return { m_data, static_size }; }
};

struct IntegerWrapper {
    StaticStorage<false, bit_width<int>> m_data;

    // There is no reason to ban u128{0} + 1 (although the second argument type is signed, the value
    // is known at the compile time to be non-negative). In order to do so, we provide overloads in
    // UFixedBigInt which take IntegerWrapper as an argument.
    consteval IntegerWrapper(int value)
    {
        if (value < 0)
            compiletime_fail("Requested implicit conversion of an integer to the unsigned one will underflow.");
        m_data[0] = static_cast<NativeWord>(value);
    }
};

constexpr inline auto get_storage_of(IntegerWrapper value) { return value.m_data; }

template<BuiltInUFixedInt T>
constexpr StaticStorage<false, bit_width<T>> get_storage_of(T value)
{
    if constexpr (sizeof(T) > sizeof(NativeWord)) {
        static_assert(sizeof(T) == 2 * sizeof(NativeWord));
        return { static_cast<NativeWord>(value), static_cast<NativeWord>(value >> native_word_size) };
    }
    return { static_cast<NativeWord>(value) };
}

// ===== Utilities =====
template<typename Word>
ALWAYS_INLINE constexpr Word extend_sign(bool sign)
{
    return sign ? NumericLimits<Word>::max() : 0;
}

// FIXME: If available, we might try to use AVX2 and AVX512.
template<typename WordType>
ALWAYS_INLINE constexpr WordType add_words(WordType word1, WordType word2, bool& carry)
{
    if (!is_constant_evaluated()) {
#if __has_builtin(__builtin_addc)
        WordType ncarry, output;
        if constexpr (SameAs<WordType, unsigned int>)
            output = __builtin_addc(word1, word2, carry, reinterpret_cast<unsigned int*>(&ncarry));
        else if constexpr (SameAs<WordType, unsigned long>)
            output = __builtin_addcl(word1, word2, carry, reinterpret_cast<unsigned long*>(&ncarry));
        else if constexpr (SameAs<WordType, unsigned long long>)
            output = __builtin_addcll(word1, word2, carry, reinterpret_cast<unsigned long long*>(&ncarry));
        else
            VERIFY_NOT_REACHED();
        carry = ncarry;
        return output;
#elif ARCH(X86_64)
        if constexpr (SameAs<WordType, unsigned int>) {
            unsigned int output;
            carry = __builtin_ia32_addcarryx_u32(carry, word1, word2, &output);
            return output;
        } else if constexpr (OneOf<WordType, unsigned long, unsigned long long>) {
            unsigned long long output;
            carry = __builtin_ia32_addcarryx_u64(carry, word1, word2, &output);
            return output;
        } else {
            VERIFY_NOT_REACHED();
        }
#endif
    }
    // Note: This is usually too confusing for both GCC and Clang.
    WordType output;
    bool ncarry = __builtin_add_overflow(word1, word2, &output);
    if (carry) {
        ++output;
        if (output == 0)
            ncarry = true;
    }
    carry = ncarry;
    return output;
}

template<typename WordType>
ALWAYS_INLINE constexpr WordType sub_words(WordType word1, WordType word2, bool& carry)
{
    if (!is_constant_evaluated()) {
#if __has_builtin(__builtin_subc)
        WordType ncarry, output;
        if constexpr (SameAs<WordType, unsigned int>)
            output = __builtin_subc(word1, word2, carry, reinterpret_cast<unsigned int*>(&ncarry));
        else if constexpr (SameAs<WordType, unsigned long>)
            output = __builtin_subcl(word1, word2, carry, reinterpret_cast<unsigned long*>(&ncarry));
        else if constexpr (SameAs<WordType, unsigned long long>)
            output = __builtin_subcll(word1, word2, carry, reinterpret_cast<unsigned long long*>(&ncarry));
        else
            VERIFY_NOT_REACHED();
        carry = ncarry;
        return output;
#elif ARCH(X86_64) && defined(AK_COMPILER_GCC)
        if constexpr (SameAs<WordType, unsigned int>) {
            unsigned int output;
            carry = __builtin_ia32_sbb_u32(carry, word1, word2, &output);
            return output;
        } else if constexpr (OneOf<WordType, unsigned long, unsigned long long>) {
            unsigned long long output;
            carry = __builtin_ia32_sbb_u64(carry, word1, word2, &output);
            return output;
        } else {
            VERIFY_NOT_REACHED();
        }
#endif
    }
    // Note: This is usually too confusing for both GCC and Clang.
    WordType output;
    bool ncarry = __builtin_sub_overflow(word1, word2, &output);
    if (carry) {
        if (output == 0)
            ncarry = true;
        --output;
    }
    carry = ncarry;
    return output;
}

template<typename WordType>
ALWAYS_INLINE constexpr DoubleWord<WordType> wide_multiply(WordType word1, WordType word2)
{
    return static_cast<DoubleWord<WordType>>(word1) * word2;
}

template<typename WordType>
constexpr DoubleWord<WordType> dword(WordType low, WordType high)
{
    return (static_cast<DoubleWord<WordType>>(high) << bit_width<WordType>) | low;
}

// Calculate ((dividend_high << word_size) + dividend_low) / divisor. Quotient should be guaranteed to fit
// into WordType.
template<typename WordType>
ALWAYS_INLINE constexpr WordType div_mod_words(WordType dividend_low, WordType dividend_high, WordType divisor, WordType& remainder)
{
    auto dividend = dword(dividend_low, dividend_high);
    remainder = static_cast<WordType>(dividend % divisor);
    return static_cast<WordType>(dividend / divisor);
}

// ===== Operations on integer storages =====
// Naming scheme for variables belonging to one of the operands or the result is as follows:
// trailing digit in a name is 1 if a variable belongs to `operand1` (or the only `operand`), 2 --
// for `operand2` and no trailing digit -- for `result`.
template<typename WordType = NativeWord>
struct StorageOperations {
    static constexpr size_t word_size = bit_width<WordType>;
    using DoubleWordType = DoubleWord<WordType>;

    static constexpr void copy(IntegerReadonlyStorage<WordType> auto const& operand, IntegerStorage<WordType> auto&& result, size_t offset = 0)
    {
        auto fill = extend_sign<WordType>(operand.is_negative());
        size_t size1 = operand.size(), size = result.size();

        for (size_t i = 0; i < size; ++i)
            result[i] = i + offset < size1 ? operand[i + offset] : fill;
    }

    static constexpr void set(WordType value, auto&& result)
    {
        result[0] = value;
        for (size_t i = 1; i < result.size(); ++i)
            result[i] = 0;
    }

    // `is_for_inequality' is a hint to compiler that we do not need to differentiate between < and >.
    static constexpr int compare(IntegerReadonlyStorage<WordType> auto const& operand1, IntegerReadonlyStorage<WordType> auto const& operand2, bool is_for_inequality)
    {
        bool sign1 = operand1.is_negative(), sign2 = operand2.is_negative();
        size_t size1 = operand1.size(), size2 = operand2.size();

        if (sign1 != sign2) {
            if (sign1)
                return -1;
            return 1;
        }

        WordType compare_value = extend_sign<WordType>(sign1);
        bool differ_in_high_bits = false;

        if (size1 > size2) {
            for (size_t i = size1; i-- > size2;)
                if (operand1[i] != compare_value)
                    differ_in_high_bits = true;
        } else if (size1 < size2) {
            for (size_t i = size2; i-- > size1;)
                if (operand2[i] != compare_value)
                    differ_in_high_bits = true;
        }

        if (differ_in_high_bits)
            return (size1 > size2) ^ sign1 ? 1 : -1;

        // FIXME: Using min(size1, size2) in the next line triggers -Warray-bounds on GCC with -O2 and
        //        -fsanitize=address. I have not reported this.
        //        Reduced testcase: https://godbolt.org/z/TE3MbfhnE
        for (size_t i = (size1 > size2 ? size2 : size1); i--;) {
            auto word1 = operand1[i], word2 = operand2[i];

            if (is_for_inequality) {
                if (word1 != word2)
                    return 1;
            } else {
                if (word1 > word2)
                    return 1;
                if (word1 < word2)
                    return -1;
            }
        }
        return 0;
    }

    enum class Bitwise {
        AND,
        OR,
        XOR,
        INVERT,
    };

    // Requirements:
    //  - !operand1.is_signed && !operand2.is_signed && !result.is_signed (the function will also work
    //    for signed storages but will extend them with zeroes regardless of the actual sign).
    template<Bitwise operation>
    static constexpr void compute_bitwise(IntegerReadonlyStorage<WordType> auto const& operand1, IntegerReadonlyStorage<WordType> auto const& operand2, IntegerStorage<WordType> auto&& result)
    {
        size_t size1 = operand1.size(), size2 = operand2.size(), size = result.size();

        for (size_t i = 0; i < size; ++i) {
            auto word1 = i < size1 ? operand1[i] : 0;
            auto word2 = i < size2 ? operand2[i] : 0;

            if constexpr (operation == Bitwise::AND)
                result[i] = word1 & word2;
            else if constexpr (operation == Bitwise::OR)
                result[i] = word1 | word2;
            else if constexpr (operation == Bitwise::XOR)
                result[i] = word1 ^ word2;
            else if constexpr (operation == Bitwise::INVERT)
                result[i] = ~word1;
            else
                static_assert(((void)operation, false));
        }
    }

    // See `storage_compute_bitwise` for the signedness requirements.
    //
    // NOTE: We want to be able to call all of the storage_* functions like
    //       `storage_*(operand1, operand2, result)`, even if some of the operands are unused (in order
    //       to then easily generate most of the operators via defines). That is why we have unused
    //       first operand here.
    template<Bitwise operation>
    static constexpr void compute_inplace_bitwise(IntegerReadonlyStorage<WordType> auto const&, IntegerReadonlyStorage<WordType> auto const& operand2, IntegerStorage<WordType> auto&& result)
    {
        size_t min_size = min(result.size(), operand2.size());

        for (size_t i = 0; i < min_size; ++i) {
            if constexpr (operation == Bitwise::AND)
                result[i] &= operand2[i];
            else if constexpr (operation == Bitwise::OR)
                result[i] |= operand2[i];
            else if constexpr (operation == Bitwise::XOR)
                result[i] ^= operand2[i];
            else
                static_assert(((void)operation, false));
        }
    }

    // Requirements for the next two functions:
    //  - shift < result.size() * word_size;
    //  - result.size() == operand.size().
    static constexpr void shift_left(IntegerReadonlyStorage<WordType> auto const& operand, size_t shift, IntegerStorage<WordType> auto&& result)
    {
        size_t size = operand.size();
        size_t offset = shift / word_size, remainder = shift % word_size;

        if (shift % word_size == 0) {
            for (size_t i = size; i-- > offset;)
                result[i] = operand[i - offset];
            for (size_t i = 0; i < offset; ++i)
                result[i] = 0;
        } else {
            for (size_t i = size; --i > offset;)
                result[i] = (operand[i - offset] << remainder) | (operand[i - offset - 1] >> (word_size - remainder));
            result[offset] = operand[0] << remainder;
            for (size_t i = 0; i < offset; ++i)
                result[i] = 0;
        }
    }

    static constexpr void shift_right(IntegerReadonlyStorage<WordType> auto const& operand, size_t shift, IntegerStorage<WordType> auto&& result)
    {
        size_t size = operand.size();
        size_t offset = shift / word_size, remainder = shift % word_size;

        if (shift % word_size == 0) {
            for (size_t i = 0; i < size - offset; ++i)
                result[i] = operand[i + offset];
            for (size_t i = size - offset; i < size; ++i)
                result[i] = 0;
        } else {
            for (size_t i = 0; i < size - offset - 1; ++i)
                result[i] = (operand[i + offset] >> remainder) | (operand[i + offset + 1] << (word_size - remainder));
            result[size - offset - 1] = operand[size - 1] >> remainder;
            for (size_t i = size - offset; i < size; ++i)
                result[i] = 0;
        }
    }

    // Requirements:
    //  - result.size() >= max(operand1.size(), operand2.size()) (not a real constraint but overflow
    //    detection will not work otherwise).
    //
    // Return value:
    // Let r be the return value of the function and a, b, c -- the integer values stored in `operand1`,
    // `operand2` and `result`, respectively. Then,
    //     a + b * (-1) ** subtract = c + r * 2 ** (result.size() * word_size).
    // In particular, r equals 0 iff no overflow has happened.
    template<bool subtract>
    static constexpr int add(IntegerReadonlyStorage<WordType> auto const& operand1, IntegerReadonlyStorage<WordType> auto const& operand2, IntegerStorage<WordType> auto&& result, bool carry = false)
    {
        bool sign1 = operand1.is_negative(), sign2 = operand2.is_negative();
        auto fill1 = extend_sign<WordType>(sign1), fill2 = extend_sign<WordType>(sign2);
        size_t size1 = operand1.size(), size2 = operand2.size(), size = result.size();

        for (size_t i = 0; i < size; ++i) {
            auto word1 = i < size1 ? operand1[i] : fill1;
            auto word2 = i < size2 ? operand2[i] : fill2;

            if constexpr (!subtract)
                result[i] = add_words(word1, word2, carry);
            else
                result[i] = sub_words(word1, word2, carry);
        }

        if constexpr (!subtract)
            return -sign1 - sign2 + carry + result.is_negative();
        else
            return -sign1 + sign2 - carry + result.is_negative();
    }

    // See `storage_add` for the meaning of the return value.
    template<bool subtract>
    static constexpr int increment(IntegerStorage<WordType> auto&& operand)
    {
        bool carry = true;
        bool sign = operand.is_negative();
        size_t size = operand.size();

        for (size_t i = 0; i < size; ++i) {
            if constexpr (!subtract)
                operand[i] = add_words<WordType>(operand[i], 0, carry);
            else
                operand[i] = sub_words<WordType>(operand[i], 0, carry);
        }

        if constexpr (!subtract)
            return -sign + carry + operand.is_negative();
        else
            return -sign - carry + operand.is_negative();
    }

    // Requirements:
    //  - result.size() == operand.size().
    //
    // Return value: operand != 0.
    static constexpr bool negate(IntegerReadonlyStorage<WordType> auto const& operand, IntegerStorage<WordType> auto&& result)
    {
        bool carry = false;
        size_t size = operand.size();
        for (size_t i = 0; i < size; ++i)
            result[i] = sub_words<WordType>(0, operand[i], carry);
        return carry;
    }

    // No allocations will occur if both operands are unsigned.
    template<IntegerReadonlyStorage<WordType> Operand1, IntegerReadonlyStorage<WordType> Operand2>
    static constexpr void baseline_mul(Operand1 const& operand1, Operand2 const& operand2, IntegerStorage<WordType> auto&& __restrict__ result, auto&& buffer)
    {
        bool sign1 = operand1.is_negative(), sign2 = operand2.is_negative();
        size_t size1 = operand1.size(), size2 = operand2.size(), size = result.size();

        if (size1 == 1 && size2 == 1) {
            // We do not want to compete with the cleverness of the compiler of multiplying NativeWords.
            ConditionallySignedDoubleWord<WordType, Operand1::is_signed> word1 = operand1[0];
            ConditionallySignedDoubleWord<WordType, Operand2::is_signed> word2 = operand2[0];
            auto value = static_cast<DoubleWordType>(word1 * word2);

            result[0] = value;
            if (size > 1) {
                result[1] = value >> word_size;

                auto fill = extend_sign<WordType>(sign1 ^ sign2);
                for (size_t i = 2; i < result.size(); ++i)
                    result[i] = fill;
            }
            return;
        }

        if (size1 < size2) {
            baseline_mul(operand2, operand1, result, buffer);
            return;
        }
        // Now size1 >= size2

        // Normalize signs
        auto data1 = operand1.data(), data2 = operand2.data();
        if (size2 < size) {
            if (sign1) {
                auto inverted = buffer.allocate(size1);
                negate(operand1, StorageSpan<WordType, false> { inverted, size1 });
                data1 = inverted;
            }
            if (sign2) {
                auto inverted = buffer.allocate(size2);
                negate(operand2, StorageSpan<WordType, false> { inverted, size2 });
                data2 = inverted;
            }
        }
        size1 = min(size1, size), size2 = min(size2, size);

        // Do schoolbook O(size1 * size2).
        DoubleWordType carry = 0;
        for (size_t i = 0; i < size; ++i) {
            result[i] = static_cast<WordType>(carry);
            carry >>= word_size;

            size_t start_index = i >= size2 ? i - size2 + 1 : 0;
            size_t end_index = min(i + 1, size1);

            for (size_t j = start_index; j < end_index; ++j) {
                auto x = static_cast<DoubleWordType>(data1[j]) * data2[i - j];

                bool ncarry = false;
                result[i] = add_words(result[i], static_cast<WordType>(x), ncarry);
                carry += (x >> word_size) + ncarry;
            }
        }

        if (size2 < size && (sign1 ^ sign2))
            negate(result, result);
    }

    template<bool restore_remainder = false>
    static constexpr void div_mod_internal(
        StorageSpan<WordType, false> dividend, StorageSpan<WordType, false> divisor,
        StorageSpan<WordType, false> quotient, StorageSpan<WordType, false> remainder,
        size_t dividend_len, size_t divisor_len)
    {
        // Knuth's algorithm D
        // D1. Normalize
        // FIXME: Investigate GCC producing bogus -Warray-bounds when dividing u128 by u32. This code
        //        should not be reachable at all in this case because fast paths above cover all cases
        //        when `operand2.size() == 1`.
        AK_IGNORE_DIAGNOSTIC("-Warray-bounds", size_t shift = count_leading_zeroes(divisor[divisor_len - 1]);)
        shift_left(dividend, shift, dividend);
        shift_left(divisor, shift, divisor);

        auto divisor_approx = divisor[divisor_len - 1];

        for (size_t i = dividend_len + 1; i-- > divisor_len;) {
            // D3. Calculate qhat
            WordType qhat;
            VERIFY(dividend[i] <= divisor_approx);
            if (dividend[i] == divisor_approx) {
                qhat = NumericLimits<WordType>::max();
            } else {
                WordType rhat;
                qhat = div_mod_words(dividend[i - 1], dividend[i], divisor_approx, rhat);

                auto is_qhat_too_large = [&] {
                    return wide_multiply(qhat, divisor[divisor_len - 2]) > dword(dividend[i - 2], rhat);
                };
                if (is_qhat_too_large()) {
                    --qhat;
                    bool carry = false;
                    rhat = add_words(rhat, divisor_approx, carry);
                    if (!carry && is_qhat_too_large())
                        --qhat;
                }
            }

            // D4. Multiply & subtract
            WordType mul_carry = 0;
            bool sub_carry = false;
            for (size_t j = 0; j < divisor_len; ++j) {
                auto mul_result = wide_multiply(qhat, divisor[j]) + mul_carry;
                auto& output = dividend[i + j - divisor_len];
                output = sub_words(output, static_cast<WordType>(mul_result), sub_carry);
                mul_carry = mul_result >> word_size;
            }
            dividend[i] = sub_words(dividend[i], mul_carry, sub_carry);

            if (sub_carry) {
                // D6. Add back
                auto dividend_part = StorageSpan<WordType, false> { dividend.slice(i - divisor_len, divisor_len + 1) };
                auto overflow = add<false>(dividend_part, divisor, dividend_part);
                VERIFY(overflow == 1);
            }

            quotient[i - divisor_len] = qhat - sub_carry;
        }

        for (size_t i = dividend_len - divisor_len + 1; i < quotient.size(); ++i)
            quotient[i] = 0;

        // D8. Unnormalize
        if constexpr (restore_remainder)
            shift_right(StorageSpan<WordType, false> { dividend.trim(remainder.size()) }, shift, remainder);
    }
};

}

using Detail::StorageOperations, Detail::NativeWord, Detail::native_word_size, Detail::max_native_word,
    Detail::UnsignedStorageSpan, Detail::UnsignedStorageReadonlySpan;

inline Detail::NullAllocator g_null_allocator;

}
