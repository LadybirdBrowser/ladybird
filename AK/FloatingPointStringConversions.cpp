/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BigIntBase.h>
#include <AK/CharacterTypes.h>
#include <AK/FloatingPointStringConversions.h>
#include <AK/Format.h>
#include <AK/ScopeGuard.h>
#include <AK/StringView.h>
#include <AK/UFixedBigInt.h>
#include <AK/UFixedBigIntDivision.h>

namespace AK {

// This entire algorithm is an implementation of the paper: Number Parsing at a Gigabyte per Second
// by Daniel Lemire, available at https://arxiv.org/abs/2101.11408 and an implementation
// at https://github.com/fastfloat/fast_float
// There is also a perhaps more easily understandable explanation
// at https://nigeltao.github.io/blog/2020/eisel-lemire.html

template<typename T>
concept ParseableFloatingPoint = IsFloatingPoint<T> && (sizeof(T) == sizeof(u32) || sizeof(T) == sizeof(u64));

template<ParseableFloatingPoint T>
struct FloatingPointInfo {
    static_assert(sizeof(T) == sizeof(u64) || sizeof(T) == sizeof(u32));
    using SameSizeUnsigned = Conditional<sizeof(T) == sizeof(u64), u64, u32>;

    // Implementing just this gives all the other bit sizes and mask immediately.
    static constexpr inline i32 mantissa_bits()
    {
        if constexpr (sizeof(T) == sizeof(u64))
            return 52;

        return 23;
    }

    static constexpr inline i32 exponent_bits()
    {
        return sizeof(T) * 8u - 1u - mantissa_bits();
    }

    static constexpr inline i32 exponent_bias()
    {
        return (1 << (exponent_bits() - 1)) - 1;
    }

    static constexpr inline i32 minimum_exponent()
    {
        return -exponent_bias();
    }

    static constexpr inline i32 infinity_exponent()
    {
        static_assert(exponent_bits() < 31);
        return (1 << exponent_bits()) - 1;
    }

    static constexpr inline i32 sign_bit_index()
    {
        return sizeof(T) * 8 - 1;
    }

    static constexpr inline SameSizeUnsigned sign_mask()
    {
        return SameSizeUnsigned { 1 } << sign_bit_index();
    }

    static constexpr inline SameSizeUnsigned mantissa_mask()
    {
        return (SameSizeUnsigned { 1 } << mantissa_bits()) - 1;
    }

    static constexpr inline SameSizeUnsigned exponent_mask()
    {
        return SameSizeUnsigned { infinity_exponent() } << mantissa_bits();
    }

    static constexpr inline i32 max_exponent_round_to_even()
    {
        if constexpr (sizeof(T) == sizeof(u64))
            return 23;

        return 10;
    }

    static constexpr inline i32 min_exponent_round_to_even()
    {
        if constexpr (sizeof(T) == sizeof(u64))
            return -4;

        return -17;
    }

    static constexpr inline size_t max_possible_digits_needed_for_parsing()
    {
        if constexpr (sizeof(T) == sizeof(u64))
            return 769;

        return 114;
    }

    static constexpr inline i32 max_power_of_10()
    {
        if constexpr (sizeof(T) == sizeof(u64))
            return 308;

        return 38;
    }

    static constexpr inline i32 min_power_of_10()
    {
        // Closest double value to zero is xe-324 and since we have at most 19 digits
        // we know that -324 -19 = -343 so exponent below that must be zero (for double)
        if constexpr (sizeof(T) == sizeof(u64))
            return -342;

        return -65;
    }

    static constexpr inline i32 max_exact_power_of_10()
    {
        // These are the largest power of 10 representable in T
        // So all powers of 10*i less than or equal to this should be the exact
        // values, be careful as they can be above "safe integer" limits.

        if constexpr (sizeof(T) == sizeof(u64))
            return 22;

        return 10;
    }

    static constexpr inline T power_of_ten(i32 exponent)
    {
        VERIFY(exponent <= max_exact_power_of_10());
        VERIFY(exponent >= 0);
        return m_powers_of_ten_stored[exponent];
    }

    template<u32 MaxPower>
    static constexpr inline Array<T, MaxPower + 1> compute_powers_of_ten()
    {
        // All these values are guaranteed to be exact all powers of MaxPower is the
        Array<T, MaxPower + 1> values {};

        values[0] = T(1.0);
        T ten = T(10.);

        for (u32 i = 1; i <= MaxPower; ++i)
            values[i] = values[i - 1] * ten;

        return values;
    }

    static constexpr auto m_powers_of_ten_stored = compute_powers_of_ten<max_exact_power_of_10()>();
};

template<typename T>
using BitSizedUnsignedForFloatingPoint = typename FloatingPointInfo<T>::SameSizeUnsigned;

struct BasicParseResult {
    u64 mantissa = 0;
    i64 exponent = 0;
    bool valid = false;
    bool negative = false;
    bool more_than_19_digits_with_overflow = false;
    char const* last_parsed { nullptr };
    StringView whole_part;
    StringView fractional_part;
};

static constexpr auto max_representable_power_of_ten_in_u64 = 19;
static_assert(1e19 <= static_cast<double>(NumericLimits<u64>::max()));
static_assert(1e20 >= static_cast<double>(NumericLimits<u64>::max()));

constexpr u64 read_eight_digits(char const* string)
{
    u64 val;
    __builtin_memcpy(&val, string, sizeof(val));
    return val;
}

constexpr static bool has_eight_digits(u64 value)
{
    // The ascii digits 0-9 are hex 0x30 - 0x39

    // If x is within that range then y := x + 0x46 is 0x76 to 0x7f
    //    z := x - 0x30 is 0x00 - 0x09
    //    y | z = 0x7t where t is in the range 0 - f so doing & 0x80 gives 0

    // However if a character x is below 0x30 then x - 0x30 underflows setting
    // the 0x80 bit of the next digit meaning & 0x80 will never be 0.

    // Similarly if a character x is above 0x39 then x + 0x46 gives at least
    // 0x80 thus & 0x80 will not be zero.

    return (((value + 0x4646464646464646) | (value - 0x3030303030303030)) & 0x8080808080808080) == 0;
}

constexpr static u32 eight_digits_to_value(u64 value)
{
    // THIS DOES ABSOLUTELY ASSUME has_eight_digits is true

    if constexpr (AK::HostIsLittleEndian) {
        // This trick is based on https://johnnylee-sde.github.io/Fast-numeric-string-to-int/
        // FIXME: fast_float uses a slightly different version, but that is far harder
        //        to understand and does not seem to improve performance substantially.
        //        See https://github.com/fastfloat/fast_float/pull/28

        // First convert the digits to their respectively numbers (0x30 -> 0x00 etc.)
        value -= 0x3030303030303030;

        // Because of little endian the first number will in fact be the least significant
        // bits of value i.e. "12345678" -> 0x0807060504030201
        // This means that we need to shift/multiply each digit with 8 - the byte it is in
        // So the eight need to go down, and the 01 need to be multiplied with 10000000

        // We effectively multiply by 10 and then shift those values to the right (2^8 = 256)
        // We then shift the values back down, this leads to 4 digits pairs in the 2 byte parts
        // The values between are "garbage" which we will ignore
        value = (value * (256 * 10 + 1)) >> 8;
        // So with our example this gives 0x$$4e$$38$$22$$0c, where $$ is garbage/ignored
        // In decimal this gives              78  56  34  12

        // Now we keep performing the same trick twice more
        // First * 100 and shift of 16 (2^16 = 65536) and then shift back
        value = ((value & 0x00FF00FF00FF00FF) * (65536 * 100 + 1)) >> 16;

        // Again with our example this gives 0x$$$$162e$$$$04d2
        //                                         5678    1234

        // And finally with * 10000 and shift of 32 (2^32 = 4294967296)
        value = ((value & 0x0000FFFF0000FFFF) * (4294967296 * 10000 + 1)) >> 32;

        // With the example this gives 0x$$$$$$$$00bc614e
        //                                       12345678

        // Now we just truncate to the lower part
        return u32(value);
    } else {
        value -= 0x3030303030303030;

        value = (value & 0x0fL)
            + ((value & (0x0fL << 8)) >> 8) * 10
            + ((value & (0x0fL << 16)) >> 16) * 100
            + ((value & (0x0fL << 24)) >> 24) * 1000
            + ((value & (0x0fL << 32)) >> 32) * 10000
            + ((value & (0x0fL << 40)) >> 40) * 100000
            + ((value & (0x0fL << 48)) >> 48) * 1000000
            + ((value & (0x0fL << 56)) >> 56) * 10000000;

        // Now we just truncate to the lower part
        return u32(value);
    }
}

template<typename IsDoneCallback, typename Has8CharsLeftCallback>
static BasicParseResult parse_numbers(char const* start, IsDoneCallback is_done, Has8CharsLeftCallback has_eight_chars_to_read)
{
    char const* ptr = start;
    BasicParseResult result {};

    if (start == nullptr || is_done(ptr))
        return result;

    if (*ptr == '-' || *ptr == '+') {
        result.negative = *ptr == '-';
        ++ptr;

        if (is_done(ptr) || (!is_ascii_digit(*ptr) && *ptr != '.'))
            return result;
    }

    auto const fast_parse_decimal = [&](auto& value) {
        while (has_eight_chars_to_read(ptr) && has_eight_digits(read_eight_digits(ptr))) {
            value = 100'000'000 * value + eight_digits_to_value(read_eight_digits(ptr));
            ptr += 8;
        }

        while (!is_done(ptr) && is_ascii_digit(*ptr)) {
            value = 10 * value + (*ptr - '0');
            ++ptr;
        }
    };

    u64 mantissa = 0;
    auto const* whole_part_start = ptr;
    fast_parse_decimal(mantissa);
    auto const* whole_part_end = ptr;
    auto digits_found = whole_part_end - whole_part_start;
    result.whole_part = StringView(whole_part_start, digits_found);

    i64 exponent = 0;
    auto const* start_of_fractional_part = ptr;
    if (!is_done(ptr) && *ptr == '.') {
        ++ptr;
        ++start_of_fractional_part;
        fast_parse_decimal(mantissa);

        // We parsed x digits after the dot so need to multiply with 10^-x
        exponent = -(ptr - start_of_fractional_part);
    }
    result.fractional_part = StringView(start_of_fractional_part, ptr - start_of_fractional_part);
    digits_found += -exponent;

    // If both the part
    if (digits_found == 0)
        return result;

    i64 explicit_exponent = 0;

    // We do this in a lambda to easily be able to get out of parsing the exponent
    // and resetting the final character read to before the 'e'.
    [&] {
        if (is_done(ptr))
            return;
        if (*ptr != 'e' && *ptr != 'E')
            return;

        auto* pointer_before_e = ptr;
        ArmedScopeGuard reset_ptr { [&] { ptr = pointer_before_e; } };
        ++ptr;

        if (is_done(ptr))
            return;

        bool negative_exponent = false;
        if (*ptr == '-' || *ptr == '+') {
            negative_exponent = *ptr == '-';
            ++ptr;

            if (is_done(ptr))
                return;
        }

        if (!is_ascii_digit(*ptr))
            return;

        // Now we must have an optional sign and at least one digit so we
        // will not reset
        reset_ptr.disarm();

        while (!is_done(ptr) && is_ascii_digit(*ptr)) {
            // A massive exponent is not really a problem as this would
            // require a lot of characters so we would fallback on precise
            // parsing anyway (this is already 268435456 digits or 10 megabytes of digits)
            if (explicit_exponent < 0x10'000'000)
                explicit_exponent = 10 * explicit_exponent + (*ptr - '0');

            ++ptr;
        }

        explicit_exponent = negative_exponent ? -explicit_exponent : explicit_exponent;
        exponent += explicit_exponent;
    }();

    result.valid = true;
    result.last_parsed = ptr;

    if (digits_found > max_representable_power_of_ten_in_u64) {
        // There could be overflow but because we just count the digits it could be leading zeros
        auto const* leading_digit = whole_part_start;
        while (!is_done(leading_digit) && (*leading_digit == '0' || *leading_digit == '.')) {
            if (*leading_digit == '0')
                --digits_found;

            ++leading_digit;
        }

        if (digits_found > max_representable_power_of_ten_in_u64) {
            // FIXME: We just removed leading zeros, we might be able to skip these easily again.
            // If removing the leading zeros does not help we reparse and keep just the significant digits
            result.more_than_19_digits_with_overflow = true;

            mantissa = 0;
            constexpr i64 smallest_nineteen_digit_number = { 1000000000000000000 };
            char const* reparse_ptr = whole_part_start;

            constexpr i64 smallest_eleven_digit_number = { 10000000000 };
            while (mantissa < smallest_eleven_digit_number && (whole_part_end - reparse_ptr) >= 8) {
                mantissa = 100'000'000 * mantissa + eight_digits_to_value(read_eight_digits(reparse_ptr));
                reparse_ptr += 8;
            }

            while (mantissa < smallest_nineteen_digit_number && reparse_ptr != whole_part_end) {
                mantissa = 10 * mantissa + (*reparse_ptr - '0');
                ++reparse_ptr;
            }

            if (mantissa >= smallest_nineteen_digit_number) {
                // We still needed to parse (whole_part_end - reparse_ptr) digits so scale the exponent
                exponent = explicit_exponent + (whole_part_end - reparse_ptr);
            } else {
                reparse_ptr = start_of_fractional_part;
                char const* fractional_end = result.fractional_part.characters_without_null_termination() + result.fractional_part.length();

                while (mantissa < smallest_eleven_digit_number && (fractional_end - reparse_ptr) >= 8) {
                    mantissa = 100'000'000 * mantissa + eight_digits_to_value(read_eight_digits(reparse_ptr));
                    reparse_ptr += 8;
                }

                while (mantissa < smallest_nineteen_digit_number && reparse_ptr != fractional_end) {
                    mantissa = 10 * mantissa + (*reparse_ptr - '0');
                    ++reparse_ptr;
                }

                // Again we might be truncating fractional number so scale the exponent with that
                // However here need to subtract 1 from the exponent for every fractional digit
                exponent = explicit_exponent - (reparse_ptr - start_of_fractional_part);
            }
        }
    }

    result.mantissa = mantissa;
    result.exponent = exponent;
    return result;
}

constexpr static u128 compute_power_of_five(i64 exponent)
{
    constexpr u4096 bit128 = u4096 { 1u } << 127u;
    constexpr u4096 bit129 = u4096 { 1u } << 128u;

    VERIFY(exponent <= 308);
    VERIFY(exponent >= -342);

    if (exponent >= 0) {
        u4096 base { 1u };
        for (auto i = 0u; i < exponent; ++i) {
            base *= 5u;
        }

        while (base < bit128)
            base <<= 1u;
        while (base >= bit129)
            base >>= 1u;

        return u128 { base };
    }

    exponent *= -1;
    if (exponent <= 27) {
        u4096 base { 1u };
        for (auto i = 0u; i < exponent; ++i) {
            base *= 5u;
        }

        auto z = 4096 - base.clz();

        auto b = z + 127;
        u4096 base2 { 1u };
        for (auto i = 0u; i < b; ++i) {
            base2 *= 2u;
        }

        base2 /= base;
        base2 += 1u;

        return u128 { base2 };
    }

    VERIFY(exponent <= 342);
    VERIFY(exponent >= 28);

    u4096 base { 1u };
    for (auto i = 0u; i < exponent; ++i) {
        base *= 5u;
    }

    auto z = 4096 - base.clz();

    auto b = 2 * z + 128;

    u4096 base2 { 1u };
    for (auto i = 0u; i < b; ++i) {
        base2 *= 2u;
    }

    base2 /= base;
    base2 += 1u;

    while (base2 >= bit129)
        base2 >>= 1u;

    return u128 { base2 };
}

static constexpr i64 lowest_exponent = -342;
static constexpr i64 highest_exponent = 308;

constexpr auto pre_compute_table()
{
    // Computing this entire table at compile time is slow and hits constexpr
    // limits, so we just compute a (the simplest) value to make sure the
    // function is used. This table can thus be generated with the function
    // `u128 compute_power_of_five(i64 exponent)` above.
    AK::Array<u128, highest_exponent - lowest_exponent + 1> values = {
        u128 { 0x113faa2906a13b3fULL, 0xeef453d6923bd65aULL },
        u128 { 0x4ac7ca59a424c507ULL, 0x9558b4661b6565f8ULL },
        u128 { 0x5d79bcf00d2df649ULL, 0xbaaee17fa23ebf76ULL },
        u128 { 0xf4d82c2c107973dcULL, 0xe95a99df8ace6f53ULL },
        u128 { 0x79071b9b8a4be869ULL, 0x91d8a02bb6c10594ULL },
        u128 { 0x9748e2826cdee284ULL, 0xb64ec836a47146f9ULL },
        u128 { 0xfd1b1b2308169b25ULL, 0xe3e27a444d8d98b7ULL },
        u128 { 0xfe30f0f5e50e20f7ULL, 0x8e6d8c6ab0787f72ULL },
        u128 { 0xbdbd2d335e51a935ULL, 0xb208ef855c969f4fULL },
        u128 { 0xad2c788035e61382ULL, 0xde8b2b66b3bc4723ULL },
        u128 { 0x4c3bcb5021afcc31ULL, 0x8b16fb203055ac76ULL },
        u128 { 0xdf4abe242a1bbf3dULL, 0xaddcb9e83c6b1793ULL },
        u128 { 0xd71d6dad34a2af0dULL, 0xd953e8624b85dd78ULL },
        u128 { 0x8672648c40e5ad68ULL, 0x87d4713d6f33aa6bULL },
        u128 { 0x680efdaf511f18c2ULL, 0xa9c98d8ccb009506ULL },
        u128 { 0x212bd1b2566def2ULL, 0xd43bf0effdc0ba48ULL },
        u128 { 0x14bb630f7604b57ULL, 0x84a57695fe98746dULL },
        u128 { 0x419ea3bd35385e2dULL, 0xa5ced43b7e3e9188ULL },
        u128 { 0x52064cac828675b9ULL, 0xcf42894a5dce35eaULL },
        u128 { 0x7343efebd1940993ULL, 0x818995ce7aa0e1b2ULL },
        u128 { 0x1014ebe6c5f90bf8ULL, 0xa1ebfb4219491a1fULL },
        u128 { 0xd41a26e077774ef6ULL, 0xca66fa129f9b60a6ULL },
        u128 { 0x8920b098955522b4ULL, 0xfd00b897478238d0ULL },
        u128 { 0x55b46e5f5d5535b0ULL, 0x9e20735e8cb16382ULL },
        u128 { 0xeb2189f734aa831dULL, 0xc5a890362fddbc62ULL },
        u128 { 0xa5e9ec7501d523e4ULL, 0xf712b443bbd52b7bULL },
        u128 { 0x47b233c92125366eULL, 0x9a6bb0aa55653b2dULL },
        u128 { 0x999ec0bb696e840aULL, 0xc1069cd4eabe89f8ULL },
        u128 { 0xc00670ea43ca250dULL, 0xf148440a256e2c76ULL },
        u128 { 0x380406926a5e5728ULL, 0x96cd2a865764dbcaULL },
        u128 { 0xc605083704f5ecf2ULL, 0xbc807527ed3e12bcULL },
        u128 { 0xf7864a44c633682eULL, 0xeba09271e88d976bULL },
        u128 { 0x7ab3ee6afbe0211dULL, 0x93445b8731587ea3ULL },
        u128 { 0x5960ea05bad82964ULL, 0xb8157268fdae9e4cULL },
        u128 { 0x6fb92487298e33bdULL, 0xe61acf033d1a45dfULL },
        u128 { 0xa5d3b6d479f8e056ULL, 0x8fd0c16206306babULL },
        u128 { 0x8f48a4899877186cULL, 0xb3c4f1ba87bc8696ULL },
        u128 { 0x331acdabfe94de87ULL, 0xe0b62e2929aba83cULL },
        u128 { 0x9ff0c08b7f1d0b14ULL, 0x8c71dcd9ba0b4925ULL },
        u128 { 0x7ecf0ae5ee44dd9ULL, 0xaf8e5410288e1b6fULL },
        u128 { 0xc9e82cd9f69d6150ULL, 0xdb71e91432b1a24aULL },
        u128 { 0xbe311c083a225cd2ULL, 0x892731ac9faf056eULL },
        u128 { 0x6dbd630a48aaf406ULL, 0xab70fe17c79ac6caULL },
        u128 { 0x92cbbccdad5b108ULL, 0xd64d3d9db981787dULL },
        u128 { 0x25bbf56008c58ea5ULL, 0x85f0468293f0eb4eULL },
        u128 { 0xaf2af2b80af6f24eULL, 0xa76c582338ed2621ULL },
        u128 { 0x1af5af660db4aee1ULL, 0xd1476e2c07286faaULL },
        u128 { 0x50d98d9fc890ed4dULL, 0x82cca4db847945caULL },
        u128 { 0xe50ff107bab528a0ULL, 0xa37fce126597973cULL },
        u128 { 0x1e53ed49a96272c8ULL, 0xcc5fc196fefd7d0cULL },
        u128 { 0x25e8e89c13bb0f7aULL, 0xff77b1fcbebcdc4fULL },
        u128 { 0x77b191618c54e9acULL, 0x9faacf3df73609b1ULL },
        u128 { 0xd59df5b9ef6a2417ULL, 0xc795830d75038c1dULL },
        u128 { 0x4b0573286b44ad1dULL, 0xf97ae3d0d2446f25ULL },
        u128 { 0x4ee367f9430aec32ULL, 0x9becce62836ac577ULL },
        u128 { 0x229c41f793cda73fULL, 0xc2e801fb244576d5ULL },
        u128 { 0x6b43527578c1110fULL, 0xf3a20279ed56d48aULL },
        u128 { 0x830a13896b78aaa9ULL, 0x9845418c345644d6ULL },
        u128 { 0x23cc986bc656d553ULL, 0xbe5691ef416bd60cULL },
        u128 { 0x2cbfbe86b7ec8aa8ULL, 0xedec366b11c6cb8fULL },
        u128 { 0x7bf7d71432f3d6a9ULL, 0x94b3a202eb1c3f39ULL },
        u128 { 0xdaf5ccd93fb0cc53ULL, 0xb9e08a83a5e34f07ULL },
        u128 { 0xd1b3400f8f9cff68ULL, 0xe858ad248f5c22c9ULL },
        u128 { 0x23100809b9c21fa1ULL, 0x91376c36d99995beULL },
        u128 { 0xabd40a0c2832a78aULL, 0xb58547448ffffb2dULL },
        u128 { 0x16c90c8f323f516cULL, 0xe2e69915b3fff9f9ULL },
        u128 { 0xae3da7d97f6792e3ULL, 0x8dd01fad907ffc3bULL },
        u128 { 0x99cd11cfdf41779cULL, 0xb1442798f49ffb4aULL },
        u128 { 0x40405643d711d583ULL, 0xdd95317f31c7fa1dULL },
        u128 { 0x482835ea666b2572ULL, 0x8a7d3eef7f1cfc52ULL },
        u128 { 0xda3243650005eecfULL, 0xad1c8eab5ee43b66ULL },
        u128 { 0x90bed43e40076a82ULL, 0xd863b256369d4a40ULL },
        u128 { 0x5a7744a6e804a291ULL, 0x873e4f75e2224e68ULL },
        u128 { 0x711515d0a205cb36ULL, 0xa90de3535aaae202ULL },
        u128 { 0xd5a5b44ca873e03ULL, 0xd3515c2831559a83ULL },
        u128 { 0xe858790afe9486c2ULL, 0x8412d9991ed58091ULL },
        u128 { 0x626e974dbe39a872ULL, 0xa5178fff668ae0b6ULL },
        u128 { 0xfb0a3d212dc8128fULL, 0xce5d73ff402d98e3ULL },
        u128 { 0x7ce66634bc9d0b99ULL, 0x80fa687f881c7f8eULL },
        u128 { 0x1c1fffc1ebc44e80ULL, 0xa139029f6a239f72ULL },
        u128 { 0xa327ffb266b56220ULL, 0xc987434744ac874eULL },
        u128 { 0x4bf1ff9f0062baa8ULL, 0xfbe9141915d7a922ULL },
        u128 { 0x6f773fc3603db4a9ULL, 0x9d71ac8fada6c9b5ULL },
        u128 { 0xcb550fb4384d21d3ULL, 0xc4ce17b399107c22ULL },
        u128 { 0x7e2a53a146606a48ULL, 0xf6019da07f549b2bULL },
        u128 { 0x2eda7444cbfc426dULL, 0x99c102844f94e0fbULL },
        u128 { 0xfa911155fefb5308ULL, 0xc0314325637a1939ULL },
        u128 { 0x793555ab7eba27caULL, 0xf03d93eebc589f88ULL },
        u128 { 0x4bc1558b2f3458deULL, 0x96267c7535b763b5ULL },
        u128 { 0x9eb1aaedfb016f16ULL, 0xbbb01b9283253ca2ULL },
        u128 { 0x465e15a979c1cadcULL, 0xea9c227723ee8bcbULL },
        u128 { 0xbfacd89ec191ec9ULL, 0x92a1958a7675175fULL },
        u128 { 0xcef980ec671f667bULL, 0xb749faed14125d36ULL },
        u128 { 0x82b7e12780e7401aULL, 0xe51c79a85916f484ULL },
        u128 { 0xd1b2ecb8b0908810ULL, 0x8f31cc0937ae58d2ULL },
        u128 { 0x861fa7e6dcb4aa15ULL, 0xb2fe3f0b8599ef07ULL },
        u128 { 0x67a791e093e1d49aULL, 0xdfbdcece67006ac9ULL },
        u128 { 0xe0c8bb2c5c6d24e0ULL, 0x8bd6a141006042bdULL },
        u128 { 0x58fae9f773886e18ULL, 0xaecc49914078536dULL },
        u128 { 0xaf39a475506a899eULL, 0xda7f5bf590966848ULL },
        u128 { 0x6d8406c952429603ULL, 0x888f99797a5e012dULL },
        u128 { 0xc8e5087ba6d33b83ULL, 0xaab37fd7d8f58178ULL },
        u128 { 0xfb1e4a9a90880a64ULL, 0xd5605fcdcf32e1d6ULL },
        u128 { 0x5cf2eea09a55067fULL, 0x855c3be0a17fcd26ULL },
        u128 { 0xf42faa48c0ea481eULL, 0xa6b34ad8c9dfc06fULL },
        u128 { 0xf13b94daf124da26ULL, 0xd0601d8efc57b08bULL },
        u128 { 0x76c53d08d6b70858ULL, 0x823c12795db6ce57ULL },
        u128 { 0x54768c4b0c64ca6eULL, 0xa2cb1717b52481edULL },
        u128 { 0xa9942f5dcf7dfd09ULL, 0xcb7ddcdda26da268ULL },
        u128 { 0xd3f93b35435d7c4cULL, 0xfe5d54150b090b02ULL },
        u128 { 0xc47bc5014a1a6dafULL, 0x9efa548d26e5a6e1ULL },
        u128 { 0x359ab6419ca1091bULL, 0xc6b8e9b0709f109aULL },
        u128 { 0xc30163d203c94b62ULL, 0xf867241c8cc6d4c0ULL },
        u128 { 0x79e0de63425dcf1dULL, 0x9b407691d7fc44f8ULL },
        u128 { 0x985915fc12f542e4ULL, 0xc21094364dfb5636ULL },
        u128 { 0x3e6f5b7b17b2939dULL, 0xf294b943e17a2bc4ULL },
        u128 { 0xa705992ceecf9c42ULL, 0x979cf3ca6cec5b5aULL },
        u128 { 0x50c6ff782a838353ULL, 0xbd8430bd08277231ULL },
        u128 { 0xa4f8bf5635246428ULL, 0xece53cec4a314ebdULL },
        u128 { 0x871b7795e136be99ULL, 0x940f4613ae5ed136ULL },
        u128 { 0x28e2557b59846e3fULL, 0xb913179899f68584ULL },
        u128 { 0x331aeada2fe589cfULL, 0xe757dd7ec07426e5ULL },
        u128 { 0x3ff0d2c85def7621ULL, 0x9096ea6f3848984fULL },
        u128 { 0xfed077a756b53a9ULL, 0xb4bca50b065abe63ULL },
        u128 { 0xd3e8495912c62894ULL, 0xe1ebce4dc7f16dfbULL },
        u128 { 0x64712dd7abbbd95cULL, 0x8d3360f09cf6e4bdULL },
        u128 { 0xbd8d794d96aacfb3ULL, 0xb080392cc4349decULL },
        u128 { 0xecf0d7a0fc5583a0ULL, 0xdca04777f541c567ULL },
        u128 { 0xf41686c49db57244ULL, 0x89e42caaf9491b60ULL },
        u128 { 0x311c2875c522ced5ULL, 0xac5d37d5b79b6239ULL },
        u128 { 0x7d633293366b828bULL, 0xd77485cb25823ac7ULL },
        u128 { 0xae5dff9c02033197ULL, 0x86a8d39ef77164bcULL },
        u128 { 0xd9f57f830283fdfcULL, 0xa8530886b54dbdebULL },
        u128 { 0xd072df63c324fd7bULL, 0xd267caa862a12d66ULL },
        u128 { 0x4247cb9e59f71e6dULL, 0x8380dea93da4bc60ULL },
        u128 { 0x52d9be85f074e608ULL, 0xa46116538d0deb78ULL },
        u128 { 0x67902e276c921f8bULL, 0xcd795be870516656ULL },
        u128 { 0xba1cd8a3db53b6ULL, 0x806bd9714632dff6ULL },
        u128 { 0x80e8a40eccd228a4ULL, 0xa086cfcd97bf97f3ULL },
        u128 { 0x6122cd128006b2cdULL, 0xc8a883c0fdaf7df0ULL },
        u128 { 0x796b805720085f81ULL, 0xfad2a4b13d1b5d6cULL },
        u128 { 0xcbe3303674053bb0ULL, 0x9cc3a6eec6311a63ULL },
        u128 { 0xbedbfc4411068a9cULL, 0xc3f490aa77bd60fcULL },
        u128 { 0xee92fb5515482d44ULL, 0xf4f1b4d515acb93bULL },
        u128 { 0x751bdd152d4d1c4aULL, 0x991711052d8bf3c5ULL },
        u128 { 0xd262d45a78a0635dULL, 0xbf5cd54678eef0b6ULL },
        u128 { 0x86fb897116c87c34ULL, 0xef340a98172aace4ULL },
        u128 { 0xd45d35e6ae3d4da0ULL, 0x9580869f0e7aac0eULL },
        u128 { 0x8974836059cca109ULL, 0xbae0a846d2195712ULL },
        u128 { 0x2bd1a438703fc94bULL, 0xe998d258869facd7ULL },
        u128 { 0x7b6306a34627ddcfULL, 0x91ff83775423cc06ULL },
        u128 { 0x1a3bc84c17b1d542ULL, 0xb67f6455292cbf08ULL },
        u128 { 0x20caba5f1d9e4a93ULL, 0xe41f3d6a7377eecaULL },
        u128 { 0x547eb47b7282ee9cULL, 0x8e938662882af53eULL },
        u128 { 0xe99e619a4f23aa43ULL, 0xb23867fb2a35b28dULL },
        u128 { 0x6405fa00e2ec94d4ULL, 0xdec681f9f4c31f31ULL },
        u128 { 0xde83bc408dd3dd04ULL, 0x8b3c113c38f9f37eULL },
        u128 { 0x9624ab50b148d445ULL, 0xae0b158b4738705eULL },
        u128 { 0x3badd624dd9b0957ULL, 0xd98ddaee19068c76ULL },
        u128 { 0xe54ca5d70a80e5d6ULL, 0x87f8a8d4cfa417c9ULL },
        u128 { 0x5e9fcf4ccd211f4cULL, 0xa9f6d30a038d1dbcULL },
        u128 { 0x7647c3200069671fULL, 0xd47487cc8470652bULL },
        u128 { 0x29ecd9f40041e073ULL, 0x84c8d4dfd2c63f3bULL },
        u128 { 0xf468107100525890ULL, 0xa5fb0a17c777cf09ULL },
        u128 { 0x7182148d4066eeb4ULL, 0xcf79cc9db955c2ccULL },
        u128 { 0xc6f14cd848405530ULL, 0x81ac1fe293d599bfULL },
        u128 { 0xb8ada00e5a506a7cULL, 0xa21727db38cb002fULL },
        u128 { 0xa6d90811f0e4851cULL, 0xca9cf1d206fdc03bULL },
        u128 { 0x908f4a166d1da663ULL, 0xfd442e4688bd304aULL },
        u128 { 0x9a598e4e043287feULL, 0x9e4a9cec15763e2eULL },
        u128 { 0x40eff1e1853f29fdULL, 0xc5dd44271ad3cdbaULL },
        u128 { 0xd12bee59e68ef47cULL, 0xf7549530e188c128ULL },
        u128 { 0x82bb74f8301958ceULL, 0x9a94dd3e8cf578b9ULL },
        u128 { 0xe36a52363c1faf01ULL, 0xc13a148e3032d6e7ULL },
        u128 { 0xdc44e6c3cb279ac1ULL, 0xf18899b1bc3f8ca1ULL },
        u128 { 0x29ab103a5ef8c0b9ULL, 0x96f5600f15a7b7e5ULL },
        u128 { 0x7415d448f6b6f0e7ULL, 0xbcb2b812db11a5deULL },
        u128 { 0x111b495b3464ad21ULL, 0xebdf661791d60f56ULL },
        u128 { 0xcab10dd900beec34ULL, 0x936b9fcebb25c995ULL },
        u128 { 0x3d5d514f40eea742ULL, 0xb84687c269ef3bfbULL },
        u128 { 0xcb4a5a3112a5112ULL, 0xe65829b3046b0afaULL },
        u128 { 0x47f0e785eaba72abULL, 0x8ff71a0fe2c2e6dcULL },
        u128 { 0x59ed216765690f56ULL, 0xb3f4e093db73a093ULL },
        u128 { 0x306869c13ec3532cULL, 0xe0f218b8d25088b8ULL },
        u128 { 0x1e414218c73a13fbULL, 0x8c974f7383725573ULL },
        u128 { 0xe5d1929ef90898faULL, 0xafbd2350644eeacfULL },
        u128 { 0xdf45f746b74abf39ULL, 0xdbac6c247d62a583ULL },
        u128 { 0x6b8bba8c328eb783ULL, 0x894bc396ce5da772ULL },
        u128 { 0x66ea92f3f326564ULL, 0xab9eb47c81f5114fULL },
        u128 { 0xc80a537b0efefebdULL, 0xd686619ba27255a2ULL },
        u128 { 0xbd06742ce95f5f36ULL, 0x8613fd0145877585ULL },
        u128 { 0x2c48113823b73704ULL, 0xa798fc4196e952e7ULL },
        u128 { 0xf75a15862ca504c5ULL, 0xd17f3b51fca3a7a0ULL },
        u128 { 0x9a984d73dbe722fbULL, 0x82ef85133de648c4ULL },
        u128 { 0xc13e60d0d2e0ebbaULL, 0xa3ab66580d5fdaf5ULL },
        u128 { 0x318df905079926a8ULL, 0xcc963fee10b7d1b3ULL },
        u128 { 0xfdf17746497f7052ULL, 0xffbbcfe994e5c61fULL },
        u128 { 0xfeb6ea8bedefa633ULL, 0x9fd561f1fd0f9bd3ULL },
        u128 { 0xfe64a52ee96b8fc0ULL, 0xc7caba6e7c5382c8ULL },
        u128 { 0x3dfdce7aa3c673b0ULL, 0xf9bd690a1b68637bULL },
        u128 { 0x6bea10ca65c084eULL, 0x9c1661a651213e2dULL },
        u128 { 0x486e494fcff30a62ULL, 0xc31bfa0fe5698db8ULL },
        u128 { 0x5a89dba3c3efccfaULL, 0xf3e2f893dec3f126ULL },
        u128 { 0xf89629465a75e01cULL, 0x986ddb5c6b3a76b7ULL },
        u128 { 0xf6bbb397f1135823ULL, 0xbe89523386091465ULL },
        u128 { 0x746aa07ded582e2cULL, 0xee2ba6c0678b597fULL },
        u128 { 0xa8c2a44eb4571cdcULL, 0x94db483840b717efULL },
        u128 { 0x92f34d62616ce413ULL, 0xba121a4650e4ddebULL },
        u128 { 0x77b020baf9c81d17ULL, 0xe896a0d7e51e1566ULL },
        u128 { 0xace1474dc1d122eULL, 0x915e2486ef32cd60ULL },
        u128 { 0xd819992132456baULL, 0xb5b5ada8aaff80b8ULL },
        u128 { 0x10e1fff697ed6c69ULL, 0xe3231912d5bf60e6ULL },
        u128 { 0xca8d3ffa1ef463c1ULL, 0x8df5efabc5979c8fULL },
        u128 { 0xbd308ff8a6b17cb2ULL, 0xb1736b96b6fd83b3ULL },
        u128 { 0xac7cb3f6d05ddbdeULL, 0xddd0467c64bce4a0ULL },
        u128 { 0x6bcdf07a423aa96bULL, 0x8aa22c0dbef60ee4ULL },
        u128 { 0x86c16c98d2c953c6ULL, 0xad4ab7112eb3929dULL },
        u128 { 0xe871c7bf077ba8b7ULL, 0xd89d64d57a607744ULL },
        u128 { 0x11471cd764ad4972ULL, 0x87625f056c7c4a8bULL },
        u128 { 0xd598e40d3dd89bcfULL, 0xa93af6c6c79b5d2dULL },
        u128 { 0x4aff1d108d4ec2c3ULL, 0xd389b47879823479ULL },
        u128 { 0xcedf722a585139baULL, 0x843610cb4bf160cbULL },
        u128 { 0xc2974eb4ee658828ULL, 0xa54394fe1eedb8feULL },
        u128 { 0x733d226229feea32ULL, 0xce947a3da6a9273eULL },
        u128 { 0x806357d5a3f525fULL, 0x811ccc668829b887ULL },
        u128 { 0xca07c2dcb0cf26f7ULL, 0xa163ff802a3426a8ULL },
        u128 { 0xfc89b393dd02f0b5ULL, 0xc9bcff6034c13052ULL },
        u128 { 0xbbac2078d443ace2ULL, 0xfc2c3f3841f17c67ULL },
        u128 { 0xd54b944b84aa4c0dULL, 0x9d9ba7832936edc0ULL },
        u128 { 0xa9e795e65d4df11ULL, 0xc5029163f384a931ULL },
        u128 { 0x4d4617b5ff4a16d5ULL, 0xf64335bcf065d37dULL },
        u128 { 0x504bced1bf8e4e45ULL, 0x99ea0196163fa42eULL },
        u128 { 0xe45ec2862f71e1d6ULL, 0xc06481fb9bcf8d39ULL },
        u128 { 0x5d767327bb4e5a4cULL, 0xf07da27a82c37088ULL },
        u128 { 0x3a6a07f8d510f86fULL, 0x964e858c91ba2655ULL },
        u128 { 0x890489f70a55368bULL, 0xbbe226efb628afeaULL },
        u128 { 0x2b45ac74ccea842eULL, 0xeadab0aba3b2dbe5ULL },
        u128 { 0x3b0b8bc90012929dULL, 0x92c8ae6b464fc96fULL },
        u128 { 0x9ce6ebb40173744ULL, 0xb77ada0617e3bbcbULL },
        u128 { 0xcc420a6a101d0515ULL, 0xe55990879ddcaabdULL },
        u128 { 0x9fa946824a12232dULL, 0x8f57fa54c2a9eab6ULL },
        u128 { 0x47939822dc96abf9ULL, 0xb32df8e9f3546564ULL },
        u128 { 0x59787e2b93bc56f7ULL, 0xdff9772470297ebdULL },
        u128 { 0x57eb4edb3c55b65aULL, 0x8bfbea76c619ef36ULL },
        u128 { 0xede622920b6b23f1ULL, 0xaefae51477a06b03ULL },
        u128 { 0xe95fab368e45ecedULL, 0xdab99e59958885c4ULL },
        u128 { 0x11dbcb0218ebb414ULL, 0x88b402f7fd75539bULL },
        u128 { 0xd652bdc29f26a119ULL, 0xaae103b5fcd2a881ULL },
        u128 { 0x4be76d3346f0495fULL, 0xd59944a37c0752a2ULL },
        u128 { 0x6f70a4400c562ddbULL, 0x857fcae62d8493a5ULL },
        u128 { 0xcb4ccd500f6bb952ULL, 0xa6dfbd9fb8e5b88eULL },
        u128 { 0x7e2000a41346a7a7ULL, 0xd097ad07a71f26b2ULL },
        u128 { 0x8ed400668c0c28c8ULL, 0x825ecc24c873782fULL },
        u128 { 0x728900802f0f32faULL, 0xa2f67f2dfa90563bULL },
        u128 { 0x4f2b40a03ad2ffb9ULL, 0xcbb41ef979346bcaULL },
        u128 { 0xe2f610c84987bfa8ULL, 0xfea126b7d78186bcULL },
        u128 { 0xdd9ca7d2df4d7c9ULL, 0x9f24b832e6b0f436ULL },
        u128 { 0x91503d1c79720dbbULL, 0xc6ede63fa05d3143ULL },
        u128 { 0x75a44c6397ce912aULL, 0xf8a95fcf88747d94ULL },
        u128 { 0xc986afbe3ee11abaULL, 0x9b69dbe1b548ce7cULL },
        u128 { 0xfbe85badce996168ULL, 0xc24452da229b021bULL },
        u128 { 0xfae27299423fb9c3ULL, 0xf2d56790ab41c2a2ULL },
        u128 { 0xdccd879fc967d41aULL, 0x97c560ba6b0919a5ULL },
        u128 { 0x5400e987bbc1c920ULL, 0xbdb6b8e905cb600fULL },
        u128 { 0x290123e9aab23b68ULL, 0xed246723473e3813ULL },
        u128 { 0xf9a0b6720aaf6521ULL, 0x9436c0760c86e30bULL },
        u128 { 0xf808e40e8d5b3e69ULL, 0xb94470938fa89bceULL },
        u128 { 0xb60b1d1230b20e04ULL, 0xe7958cb87392c2c2ULL },
        u128 { 0xb1c6f22b5e6f48c2ULL, 0x90bd77f3483bb9b9ULL },
        u128 { 0x1e38aeb6360b1af3ULL, 0xb4ecd5f01a4aa828ULL },
        u128 { 0x25c6da63c38de1b0ULL, 0xe2280b6c20dd5232ULL },
        u128 { 0x579c487e5a38ad0eULL, 0x8d590723948a535fULL },
        u128 { 0x2d835a9df0c6d851ULL, 0xb0af48ec79ace837ULL },
        u128 { 0xf8e431456cf88e65ULL, 0xdcdb1b2798182244ULL },
        u128 { 0x1b8e9ecb641b58ffULL, 0x8a08f0f8bf0f156bULL },
        u128 { 0xe272467e3d222f3fULL, 0xac8b2d36eed2dac5ULL },
        u128 { 0x5b0ed81dcc6abb0fULL, 0xd7adf884aa879177ULL },
        u128 { 0x98e947129fc2b4e9ULL, 0x86ccbb52ea94baeaULL },
        u128 { 0x3f2398d747b36224ULL, 0xa87fea27a539e9a5ULL },
        u128 { 0x8eec7f0d19a03aadULL, 0xd29fe4b18e88640eULL },
        u128 { 0x1953cf68300424acULL, 0x83a3eeeef9153e89ULL },
        u128 { 0x5fa8c3423c052dd7ULL, 0xa48ceaaab75a8e2bULL },
        u128 { 0x3792f412cb06794dULL, 0xcdb02555653131b6ULL },
        u128 { 0xe2bbd88bbee40bd0ULL, 0x808e17555f3ebf11ULL },
        u128 { 0x5b6aceaeae9d0ec4ULL, 0xa0b19d2ab70e6ed6ULL },
        u128 { 0xf245825a5a445275ULL, 0xc8de047564d20a8bULL },
        u128 { 0xeed6e2f0f0d56712ULL, 0xfb158592be068d2eULL },
        u128 { 0x55464dd69685606bULL, 0x9ced737bb6c4183dULL },
        u128 { 0xaa97e14c3c26b886ULL, 0xc428d05aa4751e4cULL },
        u128 { 0xd53dd99f4b3066a8ULL, 0xf53304714d9265dfULL },
        u128 { 0xe546a8038efe4029ULL, 0x993fe2c6d07b7fabULL },
        u128 { 0xde98520472bdd033ULL, 0xbf8fdb78849a5f96ULL },
        u128 { 0x963e66858f6d4440ULL, 0xef73d256a5c0f77cULL },
        u128 { 0xdde7001379a44aa8ULL, 0x95a8637627989aadULL },
        u128 { 0x5560c018580d5d52ULL, 0xbb127c53b17ec159ULL },
        u128 { 0xaab8f01e6e10b4a6ULL, 0xe9d71b689dde71afULL },
        u128 { 0xcab3961304ca70e8ULL, 0x9226712162ab070dULL },
        u128 { 0x3d607b97c5fd0d22ULL, 0xb6b00d69bb55c8d1ULL },
        u128 { 0x8cb89a7db77c506aULL, 0xe45c10c42a2b3b05ULL },
        u128 { 0x77f3608e92adb242ULL, 0x8eb98a7a9a5b04e3ULL },
        u128 { 0x55f038b237591ed3ULL, 0xb267ed1940f1c61cULL },
        u128 { 0x6b6c46dec52f6688ULL, 0xdf01e85f912e37a3ULL },
        u128 { 0x2323ac4b3b3da015ULL, 0x8b61313bbabce2c6ULL },
        u128 { 0xabec975e0a0d081aULL, 0xae397d8aa96c1b77ULL },
        u128 { 0x96e7bd358c904a21ULL, 0xd9c7dced53c72255ULL },
        u128 { 0x7e50d64177da2e54ULL, 0x881cea14545c7575ULL },
        u128 { 0xdde50bd1d5d0b9e9ULL, 0xaa242499697392d2ULL },
        u128 { 0x955e4ec64b44e864ULL, 0xd4ad2dbfc3d07787ULL },
        u128 { 0xbd5af13bef0b113eULL, 0x84ec3c97da624ab4ULL },
        u128 { 0xecb1ad8aeacdd58eULL, 0xa6274bbdd0fadd61ULL },
        u128 { 0x67de18eda5814af2ULL, 0xcfb11ead453994baULL },
        u128 { 0x80eacf948770ced7ULL, 0x81ceb32c4b43fcf4ULL },
        u128 { 0xa1258379a94d028dULL, 0xa2425ff75e14fc31ULL },
        u128 { 0x96ee45813a04330ULL, 0xcad2f7f5359a3b3eULL },
        u128 { 0x8bca9d6e188853fcULL, 0xfd87b5f28300ca0dULL },
        u128 { 0x775ea264cf55347eULL, 0x9e74d1b791e07e48ULL },
        u128 { 0x95364afe032a819eULL, 0xc612062576589ddaULL },
        u128 { 0x3a83ddbd83f52205ULL, 0xf79687aed3eec551ULL },
        u128 { 0xc4926a9672793543ULL, 0x9abe14cd44753b52ULL },
        u128 { 0x75b7053c0f178294ULL, 0xc16d9a0095928a27ULL },
        u128 { 0x5324c68b12dd6339ULL, 0xf1c90080baf72cb1ULL },
        u128 { 0xd3f6fc16ebca5e04ULL, 0x971da05074da7beeULL },
        u128 { 0x88f4bb1ca6bcf585ULL, 0xbce5086492111aeaULL },
        u128 { 0x2b31e9e3d06c32e6ULL, 0xec1e4a7db69561a5ULL },
        u128 { 0x3aff322e62439fd0ULL, 0x9392ee8e921d5d07ULL },
        u128 { 0x9befeb9fad487c3ULL, 0xb877aa3236a4b449ULL },
        u128 { 0x4c2ebe687989a9b4ULL, 0xe69594bec44de15bULL },
        u128 { 0xf9d37014bf60a11ULL, 0x901d7cf73ab0acd9ULL },
        u128 { 0x538484c19ef38c95ULL, 0xb424dc35095cd80fULL },
        u128 { 0x2865a5f206b06fbaULL, 0xe12e13424bb40e13ULL },
        u128 { 0xf93f87b7442e45d4ULL, 0x8cbccc096f5088cbULL },
        u128 { 0xf78f69a51539d749ULL, 0xafebff0bcb24aafeULL },
        u128 { 0xb573440e5a884d1cULL, 0xdbe6fecebdedd5beULL },
        u128 { 0x31680a88f8953031ULL, 0x89705f4136b4a597ULL },
        u128 { 0xfdc20d2b36ba7c3eULL, 0xabcc77118461cefcULL },
        u128 { 0x3d32907604691b4dULL, 0xd6bf94d5e57a42bcULL },
        u128 { 0xa63f9a49c2c1b110ULL, 0x8637bd05af6c69b5ULL },
        u128 { 0xfcf80dc33721d54ULL, 0xa7c5ac471b478423ULL },
        u128 { 0xd3c36113404ea4a9ULL, 0xd1b71758e219652bULL },
        u128 { 0x645a1cac083126eaULL, 0x83126e978d4fdf3bULL },
        u128 { 0x3d70a3d70a3d70a4ULL, 0xa3d70a3d70a3d70aULL },
        u128 { 0xcccccccccccccccdULL, 0xccccccccccccccccULL },
        compute_power_of_five(0),
        u128 { 0x0ULL, 0xa000000000000000ULL },
        u128 { 0x0ULL, 0xc800000000000000ULL },
        u128 { 0x0ULL, 0xfa00000000000000ULL },
        u128 { 0x0ULL, 0x9c40000000000000ULL },
        u128 { 0x0ULL, 0xc350000000000000ULL },
        u128 { 0x0ULL, 0xf424000000000000ULL },
        u128 { 0x0ULL, 0x9896800000000000ULL },
        u128 { 0x0ULL, 0xbebc200000000000ULL },
        u128 { 0x0ULL, 0xee6b280000000000ULL },
        u128 { 0x0ULL, 0x9502f90000000000ULL },
        u128 { 0x0ULL, 0xba43b74000000000ULL },
        u128 { 0x0ULL, 0xe8d4a51000000000ULL },
        u128 { 0x0ULL, 0x9184e72a00000000ULL },
        u128 { 0x0ULL, 0xb5e620f480000000ULL },
        u128 { 0x0ULL, 0xe35fa931a0000000ULL },
        u128 { 0x0ULL, 0x8e1bc9bf04000000ULL },
        u128 { 0x0ULL, 0xb1a2bc2ec5000000ULL },
        u128 { 0x0ULL, 0xde0b6b3a76400000ULL },
        u128 { 0x0ULL, 0x8ac7230489e80000ULL },
        u128 { 0x0ULL, 0xad78ebc5ac620000ULL },
        u128 { 0x0ULL, 0xd8d726b7177a8000ULL },
        u128 { 0x0ULL, 0x878678326eac9000ULL },
        u128 { 0x0ULL, 0xa968163f0a57b400ULL },
        u128 { 0x0ULL, 0xd3c21bcecceda100ULL },
        u128 { 0x0ULL, 0x84595161401484a0ULL },
        u128 { 0x0ULL, 0xa56fa5b99019a5c8ULL },
        u128 { 0x0ULL, 0xcecb8f27f4200f3aULL },
        u128 { 0x4000000000000000ULL, 0x813f3978f8940984ULL },
        u128 { 0x5000000000000000ULL, 0xa18f07d736b90be5ULL },
        u128 { 0xa400000000000000ULL, 0xc9f2c9cd04674edeULL },
        u128 { 0x4d00000000000000ULL, 0xfc6f7c4045812296ULL },
        u128 { 0xf020000000000000ULL, 0x9dc5ada82b70b59dULL },
        u128 { 0x6c28000000000000ULL, 0xc5371912364ce305ULL },
        u128 { 0xc732000000000000ULL, 0xf684df56c3e01bc6ULL },
        u128 { 0x3c7f400000000000ULL, 0x9a130b963a6c115cULL },
        u128 { 0x4b9f100000000000ULL, 0xc097ce7bc90715b3ULL },
        u128 { 0x1e86d40000000000ULL, 0xf0bdc21abb48db20ULL },
        u128 { 0x1314448000000000ULL, 0x96769950b50d88f4ULL },
        u128 { 0x17d955a000000000ULL, 0xbc143fa4e250eb31ULL },
        u128 { 0x5dcfab0800000000ULL, 0xeb194f8e1ae525fdULL },
        u128 { 0x5aa1cae500000000ULL, 0x92efd1b8d0cf37beULL },
        u128 { 0xf14a3d9e40000000ULL, 0xb7abc627050305adULL },
        u128 { 0x6d9ccd05d0000000ULL, 0xe596b7b0c643c719ULL },
        u128 { 0xe4820023a2000000ULL, 0x8f7e32ce7bea5c6fULL },
        u128 { 0xdda2802c8a800000ULL, 0xb35dbf821ae4f38bULL },
        u128 { 0xd50b2037ad200000ULL, 0xe0352f62a19e306eULL },
        u128 { 0x4526f422cc340000ULL, 0x8c213d9da502de45ULL },
        u128 { 0x9670b12b7f410000ULL, 0xaf298d050e4395d6ULL },
        u128 { 0x3c0cdd765f114000ULL, 0xdaf3f04651d47b4cULL },
        u128 { 0xa5880a69fb6ac800ULL, 0x88d8762bf324cd0fULL },
        u128 { 0x8eea0d047a457a00ULL, 0xab0e93b6efee0053ULL },
        u128 { 0x72a4904598d6d880ULL, 0xd5d238a4abe98068ULL },
        u128 { 0x47a6da2b7f864750ULL, 0x85a36366eb71f041ULL },
        u128 { 0x999090b65f67d924ULL, 0xa70c3c40a64e6c51ULL },
        u128 { 0xfff4b4e3f741cf6dULL, 0xd0cf4b50cfe20765ULL },
        u128 { 0xbff8f10e7a8921a4ULL, 0x82818f1281ed449fULL },
        u128 { 0xaff72d52192b6a0dULL, 0xa321f2d7226895c7ULL },
        u128 { 0x9bf4f8a69f764490ULL, 0xcbea6f8ceb02bb39ULL },
        u128 { 0x2f236d04753d5b4ULL, 0xfee50b7025c36a08ULL },
        u128 { 0x1d762422c946590ULL, 0x9f4f2726179a2245ULL },
        u128 { 0x424d3ad2b7b97ef5ULL, 0xc722f0ef9d80aad6ULL },
        u128 { 0xd2e0898765a7deb2ULL, 0xf8ebad2b84e0d58bULL },
        u128 { 0x63cc55f49f88eb2fULL, 0x9b934c3b330c8577ULL },
        u128 { 0x3cbf6b71c76b25fbULL, 0xc2781f49ffcfa6d5ULL },
        u128 { 0x8bef464e3945ef7aULL, 0xf316271c7fc3908aULL },
        u128 { 0x97758bf0e3cbb5acULL, 0x97edd871cfda3a56ULL },
        u128 { 0x3d52eeed1cbea317ULL, 0xbde94e8e43d0c8ecULL },
        u128 { 0x4ca7aaa863ee4bddULL, 0xed63a231d4c4fb27ULL },
        u128 { 0x8fe8caa93e74ef6aULL, 0x945e455f24fb1cf8ULL },
        u128 { 0xb3e2fd538e122b44ULL, 0xb975d6b6ee39e436ULL },
        u128 { 0x60dbbca87196b616ULL, 0xe7d34c64a9c85d44ULL },
        u128 { 0xbc8955e946fe31cdULL, 0x90e40fbeea1d3a4aULL },
        u128 { 0x6babab6398bdbe41ULL, 0xb51d13aea4a488ddULL },
        u128 { 0xc696963c7eed2dd1ULL, 0xe264589a4dcdab14ULL },
        u128 { 0xfc1e1de5cf543ca2ULL, 0x8d7eb76070a08aecULL },
        u128 { 0x3b25a55f43294bcbULL, 0xb0de65388cc8ada8ULL },
        u128 { 0x49ef0eb713f39ebeULL, 0xdd15fe86affad912ULL },
        u128 { 0x6e3569326c784337ULL, 0x8a2dbf142dfcc7abULL },
        u128 { 0x49c2c37f07965404ULL, 0xacb92ed9397bf996ULL },
        u128 { 0xdc33745ec97be906ULL, 0xd7e77a8f87daf7fbULL },
        u128 { 0x69a028bb3ded71a3ULL, 0x86f0ac99b4e8dafdULL },
        u128 { 0xc40832ea0d68ce0cULL, 0xa8acd7c0222311bcULL },
        u128 { 0xf50a3fa490c30190ULL, 0xd2d80db02aabd62bULL },
        u128 { 0x792667c6da79e0faULL, 0x83c7088e1aab65dbULL },
        u128 { 0x577001b891185938ULL, 0xa4b8cab1a1563f52ULL },
        u128 { 0xed4c0226b55e6f86ULL, 0xcde6fd5e09abcf26ULL },
        u128 { 0x544f8158315b05b4ULL, 0x80b05e5ac60b6178ULL },
        u128 { 0x696361ae3db1c721ULL, 0xa0dc75f1778e39d6ULL },
        u128 { 0x3bc3a19cd1e38e9ULL, 0xc913936dd571c84cULL },
        u128 { 0x4ab48a04065c723ULL, 0xfb5878494ace3a5fULL },
        u128 { 0x62eb0d64283f9c76ULL, 0x9d174b2dcec0e47bULL },
        u128 { 0x3ba5d0bd324f8394ULL, 0xc45d1df942711d9aULL },
        u128 { 0xca8f44ec7ee36479ULL, 0xf5746577930d6500ULL },
        u128 { 0x7e998b13cf4e1ecbULL, 0x9968bf6abbe85f20ULL },
        u128 { 0x9e3fedd8c321a67eULL, 0xbfc2ef456ae276e8ULL },
        u128 { 0xc5cfe94ef3ea101eULL, 0xefb3ab16c59b14a2ULL },
        u128 { 0xbba1f1d158724a12ULL, 0x95d04aee3b80ece5ULL },
        u128 { 0x2a8a6e45ae8edc97ULL, 0xbb445da9ca61281fULL },
        u128 { 0xf52d09d71a3293bdULL, 0xea1575143cf97226ULL },
        u128 { 0x593c2626705f9c56ULL, 0x924d692ca61be758ULL },
        u128 { 0x6f8b2fb00c77836cULL, 0xb6e0c377cfa2e12eULL },
        u128 { 0xb6dfb9c0f956447ULL, 0xe498f455c38b997aULL },
        u128 { 0x4724bd4189bd5eacULL, 0x8edf98b59a373fecULL },
        u128 { 0x58edec91ec2cb657ULL, 0xb2977ee300c50fe7ULL },
        u128 { 0x2f2967b66737e3edULL, 0xdf3d5e9bc0f653e1ULL },
        u128 { 0xbd79e0d20082ee74ULL, 0x8b865b215899f46cULL },
        u128 { 0xecd8590680a3aa11ULL, 0xae67f1e9aec07187ULL },
        u128 { 0xe80e6f4820cc9495ULL, 0xda01ee641a708de9ULL },
        u128 { 0x3109058d147fdcddULL, 0x884134fe908658b2ULL },
        u128 { 0xbd4b46f0599fd415ULL, 0xaa51823e34a7eedeULL },
        u128 { 0x6c9e18ac7007c91aULL, 0xd4e5e2cdc1d1ea96ULL },
        u128 { 0x3e2cf6bc604ddb0ULL, 0x850fadc09923329eULL },
        u128 { 0x84db8346b786151cULL, 0xa6539930bf6bff45ULL },
        u128 { 0xe612641865679a63ULL, 0xcfe87f7cef46ff16ULL },
        u128 { 0x4fcb7e8f3f60c07eULL, 0x81f14fae158c5f6eULL },
        u128 { 0xe3be5e330f38f09dULL, 0xa26da3999aef7749ULL },
        u128 { 0x5cadf5bfd3072cc5ULL, 0xcb090c8001ab551cULL },
        u128 { 0x73d9732fc7c8f7f6ULL, 0xfdcb4fa002162a63ULL },
        u128 { 0x2867e7fddcdd9afaULL, 0x9e9f11c4014dda7eULL },
        u128 { 0xb281e1fd541501b8ULL, 0xc646d63501a1511dULL },
        u128 { 0x1f225a7ca91a4226ULL, 0xf7d88bc24209a565ULL },
        u128 { 0x3375788de9b06958ULL, 0x9ae757596946075fULL },
        u128 { 0x52d6b1641c83aeULL, 0xc1a12d2fc3978937ULL },
        u128 { 0xc0678c5dbd23a49aULL, 0xf209787bb47d6b84ULL },
        u128 { 0xf840b7ba963646e0ULL, 0x9745eb4d50ce6332ULL },
        u128 { 0xb650e5a93bc3d898ULL, 0xbd176620a501fbffULL },
        u128 { 0xa3e51f138ab4cebeULL, 0xec5d3fa8ce427affULL },
        u128 { 0xc66f336c36b10137ULL, 0x93ba47c980e98cdfULL },
        u128 { 0xb80b0047445d4184ULL, 0xb8a8d9bbe123f017ULL },
        u128 { 0xa60dc059157491e5ULL, 0xe6d3102ad96cec1dULL },
        u128 { 0x87c89837ad68db2fULL, 0x9043ea1ac7e41392ULL },
        u128 { 0x29babe4598c311fbULL, 0xb454e4a179dd1877ULL },
        u128 { 0xf4296dd6fef3d67aULL, 0xe16a1dc9d8545e94ULL },
        u128 { 0x1899e4a65f58660cULL, 0x8ce2529e2734bb1dULL },
        u128 { 0x5ec05dcff72e7f8fULL, 0xb01ae745b101e9e4ULL },
        u128 { 0x76707543f4fa1f73ULL, 0xdc21a1171d42645dULL },
        u128 { 0x6a06494a791c53a8ULL, 0x899504ae72497ebaULL },
        u128 { 0x487db9d17636892ULL, 0xabfa45da0edbde69ULL },
        u128 { 0x45a9d2845d3c42b6ULL, 0xd6f8d7509292d603ULL },
        u128 { 0xb8a2392ba45a9b2ULL, 0x865b86925b9bc5c2ULL },
        u128 { 0x8e6cac7768d7141eULL, 0xa7f26836f282b732ULL },
        u128 { 0x3207d795430cd926ULL, 0xd1ef0244af2364ffULL },
        u128 { 0x7f44e6bd49e807b8ULL, 0x8335616aed761f1fULL },
        u128 { 0x5f16206c9c6209a6ULL, 0xa402b9c5a8d3a6e7ULL },
        u128 { 0x36dba887c37a8c0fULL, 0xcd036837130890a1ULL },
        u128 { 0xc2494954da2c9789ULL, 0x802221226be55a64ULL },
        u128 { 0xf2db9baa10b7bd6cULL, 0xa02aa96b06deb0fdULL },
        u128 { 0x6f92829494e5acc7ULL, 0xc83553c5c8965d3dULL },
        u128 { 0xcb772339ba1f17f9ULL, 0xfa42a8b73abbf48cULL },
        u128 { 0xff2a760414536efbULL, 0x9c69a97284b578d7ULL },
        u128 { 0xfef5138519684abaULL, 0xc38413cf25e2d70dULL },
        u128 { 0x7eb258665fc25d69ULL, 0xf46518c2ef5b8cd1ULL },
        u128 { 0xef2f773ffbd97a61ULL, 0x98bf2f79d5993802ULL },
        u128 { 0xaafb550ffacfd8faULL, 0xbeeefb584aff8603ULL },
        u128 { 0x95ba2a53f983cf38ULL, 0xeeaaba2e5dbf6784ULL },
        u128 { 0xdd945a747bf26183ULL, 0x952ab45cfa97a0b2ULL },
        u128 { 0x94f971119aeef9e4ULL, 0xba756174393d88dfULL },
        u128 { 0x7a37cd5601aab85dULL, 0xe912b9d1478ceb17ULL },
        u128 { 0xac62e055c10ab33aULL, 0x91abb422ccb812eeULL },
        u128 { 0x577b986b314d6009ULL, 0xb616a12b7fe617aaULL },
        u128 { 0xed5a7e85fda0b80bULL, 0xe39c49765fdf9d94ULL },
        u128 { 0x14588f13be847307ULL, 0x8e41ade9fbebc27dULL },
        u128 { 0x596eb2d8ae258fc8ULL, 0xb1d219647ae6b31cULL },
        u128 { 0x6fca5f8ed9aef3bbULL, 0xde469fbd99a05fe3ULL },
        u128 { 0x25de7bb9480d5854ULL, 0x8aec23d680043beeULL },
        u128 { 0xaf561aa79a10ae6aULL, 0xada72ccc20054ae9ULL },
        u128 { 0x1b2ba1518094da04ULL, 0xd910f7ff28069da4ULL },
        u128 { 0x90fb44d2f05d0842ULL, 0x87aa9aff79042286ULL },
        u128 { 0x353a1607ac744a53ULL, 0xa99541bf57452b28ULL },
        u128 { 0x42889b8997915ce8ULL, 0xd3fa922f2d1675f2ULL },
        u128 { 0x69956135febada11ULL, 0x847c9b5d7c2e09b7ULL },
        u128 { 0x43fab9837e699095ULL, 0xa59bc234db398c25ULL },
        u128 { 0x94f967e45e03f4bbULL, 0xcf02b2c21207ef2eULL },
        u128 { 0x1d1be0eebac278f5ULL, 0x8161afb94b44f57dULL },
        u128 { 0x6462d92a69731732ULL, 0xa1ba1ba79e1632dcULL },
        u128 { 0x7d7b8f7503cfdcfeULL, 0xca28a291859bbf93ULL },
        u128 { 0x5cda735244c3d43eULL, 0xfcb2cb35e702af78ULL },
        u128 { 0x3a0888136afa64a7ULL, 0x9defbf01b061adabULL },
        u128 { 0x88aaa1845b8fdd0ULL, 0xc56baec21c7a1916ULL },
        u128 { 0x8aad549e57273d45ULL, 0xf6c69a72a3989f5bULL },
        u128 { 0x36ac54e2f678864bULL, 0x9a3c2087a63f6399ULL },
        u128 { 0x84576a1bb416a7ddULL, 0xc0cb28a98fcf3c7fULL },
        u128 { 0x656d44a2a11c51d5ULL, 0xf0fdf2d3f3c30b9fULL },
        u128 { 0x9f644ae5a4b1b325ULL, 0x969eb7c47859e743ULL },
        u128 { 0x873d5d9f0dde1feeULL, 0xbc4665b596706114ULL },
        u128 { 0xa90cb506d155a7eaULL, 0xeb57ff22fc0c7959ULL },
        u128 { 0x9a7f12442d588f2ULL, 0x9316ff75dd87cbd8ULL },
        u128 { 0xc11ed6d538aeb2fULL, 0xb7dcbf5354e9beceULL },
        u128 { 0x8f1668c8a86da5faULL, 0xe5d3ef282a242e81ULL },
        u128 { 0xf96e017d694487bcULL, 0x8fa475791a569d10ULL },
        u128 { 0x37c981dcc395a9acULL, 0xb38d92d760ec4455ULL },
        u128 { 0x85bbe253f47b1417ULL, 0xe070f78d3927556aULL },
        u128 { 0x93956d7478ccec8eULL, 0x8c469ab843b89562ULL },
        u128 { 0x387ac8d1970027b2ULL, 0xaf58416654a6babbULL },
        u128 { 0x6997b05fcc0319eULL, 0xdb2e51bfe9d0696aULL },
        u128 { 0x441fece3bdf81f03ULL, 0x88fcf317f22241e2ULL },
        u128 { 0xd527e81cad7626c3ULL, 0xab3c2fddeeaad25aULL },
        u128 { 0x8a71e223d8d3b074ULL, 0xd60b3bd56a5586f1ULL },
        u128 { 0xf6872d5667844e49ULL, 0x85c7056562757456ULL },
        u128 { 0xb428f8ac016561dbULL, 0xa738c6bebb12d16cULL },
        u128 { 0xe13336d701beba52ULL, 0xd106f86e69d785c7ULL },
        u128 { 0xecc0024661173473ULL, 0x82a45b450226b39cULL },
        u128 { 0x27f002d7f95d0190ULL, 0xa34d721642b06084ULL },
        u128 { 0x31ec038df7b441f4ULL, 0xcc20ce9bd35c78a5ULL },
        u128 { 0x7e67047175a15271ULL, 0xff290242c83396ceULL },
        u128 { 0xf0062c6e984d386ULL, 0x9f79a169bd203e41ULL },
        u128 { 0x52c07b78a3e60868ULL, 0xc75809c42c684dd1ULL },
        u128 { 0xa7709a56ccdf8a82ULL, 0xf92e0c3537826145ULL },
        u128 { 0x88a66076400bb691ULL, 0x9bbcc7a142b17ccbULL },
        u128 { 0x6acff893d00ea435ULL, 0xc2abf989935ddbfeULL },
        u128 { 0x583f6b8c4124d43ULL, 0xf356f7ebf83552feULL },
        u128 { 0xc3727a337a8b704aULL, 0x98165af37b2153deULL },
        u128 { 0x744f18c0592e4c5cULL, 0xbe1bf1b059e9a8d6ULL },
        u128 { 0x1162def06f79df73ULL, 0xeda2ee1c7064130cULL },
        u128 { 0x8addcb5645ac2ba8ULL, 0x9485d4d1c63e8be7ULL },
        u128 { 0x6d953e2bd7173692ULL, 0xb9a74a0637ce2ee1ULL },
        u128 { 0xc8fa8db6ccdd0437ULL, 0xe8111c87c5c1ba99ULL },
        u128 { 0x1d9c9892400a22a2ULL, 0x910ab1d4db9914a0ULL },
        u128 { 0x2503beb6d00cab4bULL, 0xb54d5e4a127f59c8ULL },
        u128 { 0x2e44ae64840fd61dULL, 0xe2a0b5dc971f303aULL },
        u128 { 0x5ceaecfed289e5d2ULL, 0x8da471a9de737e24ULL },
        u128 { 0x7425a83e872c5f47ULL, 0xb10d8e1456105dadULL },
        u128 { 0xd12f124e28f77719ULL, 0xdd50f1996b947518ULL },
        u128 { 0x82bd6b70d99aaa6fULL, 0x8a5296ffe33cc92fULL },
        u128 { 0x636cc64d1001550bULL, 0xace73cbfdc0bfb7bULL },
        u128 { 0x3c47f7e05401aa4eULL, 0xd8210befd30efa5aULL },
        u128 { 0x65acfaec34810a71ULL, 0x8714a775e3e95c78ULL },
        u128 { 0x7f1839a741a14d0dULL, 0xa8d9d1535ce3b396ULL },
        u128 { 0x1ede48111209a050ULL, 0xd31045a8341ca07cULL },
        u128 { 0x934aed0aab460432ULL, 0x83ea2b892091e44dULL },
        u128 { 0xf81da84d5617853fULL, 0xa4e4b66b68b65d60ULL },
        u128 { 0x36251260ab9d668eULL, 0xce1de40642e3f4b9ULL },
        u128 { 0xc1d72b7c6b426019ULL, 0x80d2ae83e9ce78f3ULL },
        u128 { 0xb24cf65b8612f81fULL, 0xa1075a24e4421730ULL },
        u128 { 0xdee033f26797b627ULL, 0xc94930ae1d529cfcULL },
        u128 { 0x169840ef017da3b1ULL, 0xfb9b7cd9a4a7443cULL },
        u128 { 0x8e1f289560ee864eULL, 0x9d412e0806e88aa5ULL },
        u128 { 0xf1a6f2bab92a27e2ULL, 0xc491798a08a2ad4eULL },
        u128 { 0xae10af696774b1dbULL, 0xf5b5d7ec8acb58a2ULL },
        u128 { 0xacca6da1e0a8ef29ULL, 0x9991a6f3d6bf1765ULL },
        u128 { 0x17fd090a58d32af3ULL, 0xbff610b0cc6edd3fULL },
        u128 { 0xddfc4b4cef07f5b0ULL, 0xeff394dcff8a948eULL },
        u128 { 0x4abdaf101564f98eULL, 0x95f83d0a1fb69cd9ULL },
        u128 { 0x9d6d1ad41abe37f1ULL, 0xbb764c4ca7a4440fULL },
        u128 { 0x84c86189216dc5edULL, 0xea53df5fd18d5513ULL },
        u128 { 0x32fd3cf5b4e49bb4ULL, 0x92746b9be2f8552cULL },
        u128 { 0x3fbc8c33221dc2a1ULL, 0xb7118682dbb66a77ULL },
        u128 { 0xfabaf3feaa5334aULL, 0xe4d5e82392a40515ULL },
        u128 { 0x29cb4d87f2a7400eULL, 0x8f05b1163ba6832dULL },
        u128 { 0x743e20e9ef511012ULL, 0xb2c71d5bca9023f8ULL },
        u128 { 0x914da9246b255416ULL, 0xdf78e4b2bd342cf6ULL },
        u128 { 0x1ad089b6c2f7548eULL, 0x8bab8eefb6409c1aULL },
        u128 { 0xa184ac2473b529b1ULL, 0xae9672aba3d0c320ULL },
        u128 { 0xc9e5d72d90a2741eULL, 0xda3c0f568cc4f3e8ULL },
        u128 { 0x7e2fa67c7a658892ULL, 0x8865899617fb1871ULL },
        u128 { 0xddbb901b98feeab7ULL, 0xaa7eebfb9df9de8dULL },
        u128 { 0x552a74227f3ea565ULL, 0xd51ea6fa85785631ULL },
        u128 { 0xd53a88958f87275fULL, 0x8533285c936b35deULL },
        u128 { 0x8a892abaf368f137ULL, 0xa67ff273b8460356ULL },
        u128 { 0x2d2b7569b0432d85ULL, 0xd01fef10a657842cULL },
        u128 { 0x9c3b29620e29fc73ULL, 0x8213f56a67f6b29bULL },
        u128 { 0x8349f3ba91b47b8fULL, 0xa298f2c501f45f42ULL },
        u128 { 0x241c70a936219a73ULL, 0xcb3f2f7642717713ULL },
        u128 { 0xed238cd383aa0110ULL, 0xfe0efb53d30dd4d7ULL },
        u128 { 0xf4363804324a40aaULL, 0x9ec95d1463e8a506ULL },
        u128 { 0xb143c6053edcd0d5ULL, 0xc67bb4597ce2ce48ULL },
        u128 { 0xdd94b7868e94050aULL, 0xf81aa16fdc1b81daULL },
        u128 { 0xca7cf2b4191c8326ULL, 0x9b10a4e5e9913128ULL },
        u128 { 0xfd1c2f611f63a3f0ULL, 0xc1d4ce1f63f57d72ULL },
        u128 { 0xbc633b39673c8cecULL, 0xf24a01a73cf2dccfULL },
        u128 { 0xd5be0503e085d813ULL, 0x976e41088617ca01ULL },
        u128 { 0x4b2d8644d8a74e18ULL, 0xbd49d14aa79dbc82ULL },
        u128 { 0xddf8e7d60ed1219eULL, 0xec9c459d51852ba2ULL },
        u128 { 0xcabb90e5c942b503ULL, 0x93e1ab8252f33b45ULL },
        u128 { 0x3d6a751f3b936243ULL, 0xb8da1662e7b00a17ULL },
        u128 { 0xcc512670a783ad4ULL, 0xe7109bfba19c0c9dULL },
        u128 { 0x27fb2b80668b24c5ULL, 0x906a617d450187e2ULL },
        u128 { 0xb1f9f660802dedf6ULL, 0xb484f9dc9641e9daULL },
        u128 { 0x5e7873f8a0396973ULL, 0xe1a63853bbd26451ULL },
        u128 { 0xdb0b487b6423e1e8ULL, 0x8d07e33455637eb2ULL },
        u128 { 0x91ce1a9a3d2cda62ULL, 0xb049dc016abc5e5fULL },
        u128 { 0x7641a140cc7810fbULL, 0xdc5c5301c56b75f7ULL },
        u128 { 0xa9e904c87fcb0a9dULL, 0x89b9b3e11b6329baULL },
        u128 { 0x546345fa9fbdcd44ULL, 0xac2820d9623bf429ULL },
        u128 { 0xa97c177947ad4095ULL, 0xd732290fbacaf133ULL },
        u128 { 0x49ed8eabcccc485dULL, 0x867f59a9d4bed6c0ULL },
        u128 { 0x5c68f256bfff5a74ULL, 0xa81f301449ee8c70ULL },
        u128 { 0x73832eec6fff3111ULL, 0xd226fc195c6a2f8cULL },
        u128 { 0xc831fd53c5ff7eabULL, 0x83585d8fd9c25db7ULL },
        u128 { 0xba3e7ca8b77f5e55ULL, 0xa42e74f3d032f525ULL },
        u128 { 0x28ce1bd2e55f35ebULL, 0xcd3a1230c43fb26fULL },
        u128 { 0x7980d163cf5b81b3ULL, 0x80444b5e7aa7cf85ULL },
        u128 { 0xd7e105bcc332621fULL, 0xa0555e361951c366ULL },
        u128 { 0x8dd9472bf3fefaa7ULL, 0xc86ab5c39fa63440ULL },
        u128 { 0xb14f98f6f0feb951ULL, 0xfa856334878fc150ULL },
        u128 { 0x6ed1bf9a569f33d3ULL, 0x9c935e00d4b9d8d2ULL },
        u128 { 0xa862f80ec4700c8ULL, 0xc3b8358109e84f07ULL },
        u128 { 0xcd27bb612758c0faULL, 0xf4a642e14c6262c8ULL },
        u128 { 0x8038d51cb897789cULL, 0x98e7e9cccfbd7dbdULL },
        u128 { 0xe0470a63e6bd56c3ULL, 0xbf21e44003acdd2cULL },
        u128 { 0x1858ccfce06cac74ULL, 0xeeea5d5004981478ULL },
        u128 { 0xf37801e0c43ebc8ULL, 0x95527a5202df0ccbULL },
        u128 { 0xd30560258f54e6baULL, 0xbaa718e68396cffdULL },
        u128 { 0x47c6b82ef32a2069ULL, 0xe950df20247c83fdULL },
        u128 { 0x4cdc331d57fa5441ULL, 0x91d28b7416cdd27eULL },
        u128 { 0xe0133fe4adf8e952ULL, 0xb6472e511c81471dULL },
        u128 { 0x58180fddd97723a6ULL, 0xe3d8f9e563a198e5ULL },
        u128 { 0x570f09eaa7ea7648ULL, 0x8e679c2f5e44ff8fULL },
    };
    return values;
}

static constexpr auto pre_computed_powers_of_five = pre_compute_table();

static constexpr u128 power_of_five(i64 exponent)
{
    return pre_computed_powers_of_five[exponent - lowest_exponent];
}

struct FloatingPointBuilder {
    u64 mantissa = 0;
    // This exponent is power of 2 and with the bias already added.
    i32 exponent = 0;

    static constexpr i32 invalid_exponent_offset = 32768;

    static FloatingPointBuilder zero()
    {
        return { 0, 0 };
    }

    template<typename T>
    static FloatingPointBuilder infinity()
    {
        return { 0, FloatingPointInfo<T>::infinity_exponent() };
    }

    template<typename T>
    static FloatingPointBuilder nan()
    {
        return { 1ull << (FloatingPointInfo<T>::mantissa_bits() - 1), FloatingPointInfo<T>::infinity_exponent() };
    }

    template<typename T>
    static FloatingPointBuilder from_value(T value)
    {
        using BitDetails = FloatingPointInfo<T>;
        auto bits = bit_cast<typename BitDetails::SameSizeUnsigned>(value);
        // we ignore negative

        FloatingPointBuilder result;
        i32 bias = BitDetails::mantissa_bits() + BitDetails::exponent_bias();
        if ((bits & BitDetails::exponent_mask()) == 0) {
            // 0 exponent -> denormal (or zero)
            result.exponent = 1 - bias;
            // Denormal so _DON'T_ add the implicit 1
            result.mantissa = bits & BitDetails::mantissa_mask();
        } else {
            result.exponent = (bits & BitDetails::exponent_mask()) >> BitDetails::mantissa_bits();
            result.exponent -= bias;
            result.mantissa = (bits & BitDetails::mantissa_mask()) | (1ull << BitDetails::mantissa_bits());
        }

        return result;
    }

    template<typename T>
    T to_value(bool is_negative) const
    {
        if constexpr (IsSame<double, T>) {
            VERIFY((mantissa & 0xffe0'0000'0000'0000) == 0);
            VERIFY((mantissa & 0xfff0'0000'0000'0000) == 0 || exponent == 1);
            VERIFY((exponent & ~(0x7ff)) == 0);
        } else {
            static_assert(IsSame<float, T>);
            VERIFY((mantissa & 0xff00'0000) == 0);
            VERIFY((mantissa & 0xff80'0000) == 0 || exponent == 1);
            VERIFY((exponent & ~(0xff)) == 0);
        }

        using BitSizedUnsigened = BitSizedUnsignedForFloatingPoint<T>;

        BitSizedUnsigened raw_bits = mantissa;
        raw_bits |= BitSizedUnsigened(exponent) << FloatingPointInfo<T>::mantissa_bits();
        raw_bits |= BitSizedUnsigened(is_negative) << FloatingPointInfo<T>::sign_bit_index();
        return bit_cast<T>(raw_bits);
    }
};

template<typename T>
static FloatingPointBuilder parse_arbitrarily_long_floating_point(BasicParseResult& result, FloatingPointBuilder initial);

static i32 decimal_exponent_to_binary_exponent(i32 exponent)
{
    return ((((152170 + 65536) * exponent) >> 16) + 63);
}

static u128 multiply(u64 a, u64 b)
{
    return UFixedBigInt<64>(a).wide_multiply(b);
}

template<unsigned Precision>
u128 multiplication_approximation(u64 value, i32 exponent)
{
    auto z = power_of_five(exponent);

    static_assert(Precision < 64);
    constexpr u64 mask = NumericLimits<u64>::max() >> Precision;

    auto lower_result = multiply(z.high(), value);

    if ((lower_result.high() & mask) == mask) {
        auto upper_result = multiply(z.low(), value);
        lower_result += upper_result.high();
    }

    return lower_result;
}

template<typename T>
static FloatingPointBuilder not_enough_precision_binary_to_decimal(i64 exponent, u64 mantissa, int leading_zeros)
{
    using FloatingPointRepr = FloatingPointInfo<T>;
    i32 did_not_have_upper_bit = static_cast<i32>(mantissa >> 63) ^ 1;
    FloatingPointBuilder answer;
    answer.mantissa = mantissa << did_not_have_upper_bit;

    i32 bias = FloatingPointRepr::mantissa_bits() + FloatingPointRepr::exponent_bias();
    answer.exponent = decimal_exponent_to_binary_exponent(static_cast<i32>(exponent)) - leading_zeros - did_not_have_upper_bit - 62 + bias;
    // Make it negative to show we need more precision.
    answer.exponent -= FloatingPointBuilder::invalid_exponent_offset;
    VERIFY(answer.exponent < 0);
    return answer;
}

template<typename T>
static FloatingPointBuilder fallback_binary_to_decimal(u64 mantissa, i64 exponent)
{
    // We should have caught huge exponents already
    VERIFY(exponent >= -400 && exponent <= 400);

    // Perform the initial steps of binary_to_decimal.
    auto w = mantissa;
    auto leading_zeros = count_leading_zeroes(mantissa);
    w <<= leading_zeros;

    auto product = multiplication_approximation<FloatingPointInfo<T>::mantissa_bits() + 3>(w, exponent);

    return not_enough_precision_binary_to_decimal<T>(exponent, product.high(), leading_zeros);
}

template<typename T>
static FloatingPointBuilder binary_to_decimal(u64 mantissa, i64 exponent)
{
    using FloatingPointRepr = FloatingPointInfo<T>;

    if (mantissa == 0 || exponent < FloatingPointRepr::min_power_of_10())
        return FloatingPointBuilder::zero();

    // Max double value which isn't negative is xe308
    if (exponent > FloatingPointRepr::max_power_of_10())
        return FloatingPointBuilder::infinity<T>();

    auto w = mantissa;
    // Normalize the decimal significand w by shifting it so that w ∈ [2^63, 2^64)
    auto leading_zeros = count_leading_zeroes(mantissa);
    w <<= leading_zeros;

    // We need at least mantissa bits + 1 for the implicit bit + 1 for the implicit 0 top bit and one extra for rounding
    u128 approximation_of_product_with_power_of_five = multiplication_approximation<FloatingPointRepr::mantissa_bits() + 3>(w, exponent);

    // The paper (and code of fastfloat/fast_float as of writing) mention that the low part
    // of approximation_of_product_with_power_of_five can be 2^64 - 1 here in which case we need more
    // precision if the exponent lies outside of [-27, 55]. However the authors of the paper have
    // shown that this case cannot actually occur. See https://github.com/fastfloat/fast_float/issues/146#issuecomment-1262527329

    u8 upperbit = approximation_of_product_with_power_of_five.high() >> 63;
    auto real_mantissa = approximation_of_product_with_power_of_five.high() >> (upperbit + 64 - FloatingPointRepr::mantissa_bits() - 3);

    // We immediately normalize the exponent to 0 - max else we have to add the bias in most following calculations
    i32 power_of_two_with_bias = decimal_exponent_to_binary_exponent(exponent) - leading_zeros + upperbit + FloatingPointRepr::exponent_bias();

    if (power_of_two_with_bias <= 0) {
        // If the exponent is less than the bias we might have a denormal on our hands
        // A denormal is a float with exponent zero, which means it doesn't have the implicit
        // 1 at the top of the mantissa.

        // If the top bit would be below the bottom of the mantissa we have to round to zero
        if (power_of_two_with_bias <= -63)
            return FloatingPointBuilder::zero();

        // Otherwise, we have to shift the mantissa to be a denormal
        auto s = -power_of_two_with_bias + 1;
        real_mantissa = real_mantissa >> s;

        // And then round ties to even
        real_mantissa += real_mantissa & 1;
        real_mantissa >>= 1;

        // Check for subnormal by checking if the 53th bit of the mantissa it set in which case exponent is 1 not 0
        // It is only a real subnormal if the top bit isn't set
        power_of_two_with_bias = real_mantissa < (1ull << FloatingPointRepr::mantissa_bits()) ? 0 : 1;

        return { real_mantissa, power_of_two_with_bias };
    }

    if (approximation_of_product_with_power_of_five.low() <= 1 && (real_mantissa & 0b11) == 0b01
        && exponent >= FloatingPointRepr::min_exponent_round_to_even()
        && exponent <= FloatingPointRepr::max_exponent_round_to_even()) {
        // If the lowest bit is set but the one above it isn't this is a value exactly halfway
        // between two floating points
        // if (z ÷ 264 )/m is a power of two then m ← m − 1

        // effectively all discard bits from z.high are 0
        if (approximation_of_product_with_power_of_five.high() == (real_mantissa << (upperbit + 64 - FloatingPointRepr::mantissa_bits() - 3))) {
            real_mantissa &= ~u64(1);
        }
    }

    real_mantissa += real_mantissa & 1;
    real_mantissa >>= 1;

    // If we overflowed the mantissa round up the exponent
    if (real_mantissa >= (2ull << FloatingPointRepr::mantissa_bits())) {
        real_mantissa = 1ull << FloatingPointRepr::mantissa_bits();
        ++power_of_two_with_bias;
    }

    real_mantissa &= ~(1ull << FloatingPointRepr::mantissa_bits());

    // We might have rounded exponent up to infinity
    if (power_of_two_with_bias >= FloatingPointRepr::infinity_exponent())
        return FloatingPointBuilder::infinity<T>();

    return {
        real_mantissa, power_of_two_with_bias
    };
}

class MinimalBigInt {
    using Ops = StorageOperations<>;

public:
    MinimalBigInt() = default;
    MinimalBigInt(u64 value)
    {
        Ops::copy(Detail::get_storage_of(value), get_storage(words_in_u64));
    }

    static MinimalBigInt from_decimal_floating_point(BasicParseResult const& parse_result, size_t& digits_parsed, size_t max_total_digits)
    {
        size_t current_word_counter = 0;
        // 10**19 is the biggest power of ten which fits in 64 bit
        constexpr size_t max_word_counter = max_representable_power_of_ten_in_u64;

        u64 current_word = 0;

        enum AddDigitResult {
            DidNotHitMaxDigits,
            HitMaxDigits,
        };

        auto does_truncate_non_zero = [](char const* parse_head, char const* parse_end) {
            while (parse_end - parse_head >= 8) {
                static_assert('0' == 0x30);

                if (read_eight_digits(parse_head) != 0x3030303030303030)
                    return true;

                parse_head += 8;
            }

            while (parse_head != parse_end) {
                if (*parse_head != '0')
                    return true;

                ++parse_head;
            }

            return false;
        };

        MinimalBigInt value;
        auto add_digits = [&](StringView digits, bool check_fraction_for_truncation = false) {
            char const* parse_head = digits.characters_without_null_termination();
            char const* parse_end = digits.characters_without_null_termination() + digits.length();

            if (digits_parsed == 0) {
                // Skip all leading zeros as long as we haven't hit a non zero
                while (parse_head != parse_end && *parse_head == '0')
                    ++parse_head;
            }

            while (parse_head != parse_end) {
                while (max_word_counter - current_word_counter >= 8
                    && parse_end - parse_head >= 8
                    && max_total_digits - digits_parsed >= 8) {

                    current_word = current_word * 100'000'000 + eight_digits_to_value(read_eight_digits(parse_head));

                    digits_parsed += 8;
                    current_word_counter += 8;
                    parse_head += 8;
                }

                while (current_word_counter < max_word_counter
                    && parse_head != parse_end
                    && digits_parsed < max_total_digits) {

                    current_word = current_word * 10 + (*parse_head - '0');

                    ++digits_parsed;
                    ++current_word_counter;
                    ++parse_head;
                }

                if (digits_parsed == max_total_digits) {
                    // Check if we are leaving behind any non zero
                    bool truncated = does_truncate_non_zero(parse_head, parse_end);
                    if (auto fraction = parse_result.fractional_part; check_fraction_for_truncation && !fraction.is_empty())
                        truncated = truncated || does_truncate_non_zero(fraction.characters_without_null_termination(), fraction.characters_without_null_termination() + fraction.length());

                    // If we truncated we just pretend there is another 1 after the already parsed digits

                    if (truncated && current_word_counter != max_word_counter) {
                        // If it still fits in the current add it there, this saves a wide multiply
                        current_word = current_word * 10 + 1;
                        ++current_word_counter;
                        truncated = false;
                    }
                    value.add_digits(current_word, current_word_counter);

                    // If it didn't fit just do * 10 + 1
                    if (truncated)
                        value.add_digits(1, 1);

                    return HitMaxDigits;
                } else {
                    value.add_digits(current_word, current_word_counter);
                    current_word = 0;
                    current_word_counter = 0;
                }
            }

            return DidNotHitMaxDigits;
        };

        if (add_digits(parse_result.whole_part, true) == HitMaxDigits)
            return value;

        add_digits(parse_result.fractional_part);

        return value;
    }

    u64 top_64_bits(bool& has_truncated_bits) const
    {
        if (m_used_length == 0)
            return 0;

        // Top word should be non-zero
        VERIFY(m_words[m_used_length - 1] != 0);

        auto top_u64_start = static_cast<size_t>(size_in_bits()) - 64;
        u64 top_u64 = 0;

        for (size_t i = 0; i < m_used_length; ++i) {
            size_t word_start = i * native_word_size;
            size_t word_end = word_start + native_word_size;

            if (top_u64_start < word_end) {
                if (top_u64_start >= word_start) {
                    auto shift = top_u64_start - word_start;
                    top_u64 = m_words[i] >> shift;
                    has_truncated_bits |= m_words[i] ^ (top_u64 << shift);
                } else {
                    top_u64 |= static_cast<u64>(m_words[i]) << (word_start - top_u64_start);
                }
            } else {
                has_truncated_bits |= m_words[i];
            }
        }

        return top_u64;
    }

    i32 size_in_bits() const
    {
        if (m_used_length == 0)
            return 0;
        // This is guaranteed to be at most max_words_needed * word_size so not above i32 max
        return static_cast<i32>(native_word_size * m_used_length) - count_leading_zeroes(m_words[m_used_length - 1]);
    }

    void multiply_with_power_of_10(u32 exponent)
    {
        multiply_with_power_of_5(exponent);
        multiply_with_power_of_2(exponent);
    }

    void multiply_with_power_of_5(u32 exponent)
    {
        // FIXME: Using UFixedBigInt here does not feel right. We need to nicely (without
        //        reinterpret_cast and ifs) convert u64 to NativeWord (if NativeWord == u32)
        //        and one of UFixedBigInt constructors happens to do this exact job.
        //
        // To calculate this lookup table, we compute 5 ** (2 ** i) for all i \in [0; 10], split
        // numbers into 64-bit words and concatenate the results, writing corresponding sizes to
        // `sizes` array in `power_of_5` lambda.
        static constexpr UFixedBigInt<82 * 64> power_of_5_coefficients = { {
            5ull,
            25ull,
            625ull,
            390625ull,
            152587890625ull,
            3273344365508751233ull,
            1262ull,
            7942358959831785217ull,
            16807427164405733357ull,
            1593091ull,
            279109966635548161ull,
            2554917779393558781ull,
            14124656261812188652ull,
            11976055582626787546ull,
            2537941837315ull,
            13750482914757213185ull,
            1302999927698857842ull,
            14936872543252795590ull,
            2788415840139466767ull,
            2095640732773017264ull,
            7205570348933370714ull,
            7348167152523113408ull,
            9285516396840364274ull,
            6907659600622710236ull,
            349175ull,
            8643096425819600897ull,
            6743743997439985372ull,
            14059704609098336919ull,
            10729359125898331411ull,
            4933048501514368705ull,
            12258131603170554683ull,
            2172371001088594721ull,
            13569903330219142946ull,
            13809142207969578845ull,
            16716360519037769646ull,
            9631256923806107285ull,
            12866941232305103710ull,
            1397931361048440292ull,
            7619627737732970332ull,
            12725409486282665900ull,
            11703051443360963910ull,
            9947078370803086083ull,
            13966287901448440471ull,
            121923442132ull,
            17679772531488845825ull,
            2216509366347768155ull,
            1568689219195129479ull,
            5511594616325588277ull,
            1067709417009240089ull,
            9070650952098657518ull,
            11515285870634858015ull,
            2539561553659505564ull,
            17604889300961091799ull,
            14511540856854204724ull,
            12099083339557485471ull,
            7115240299237943815ull,
            313979240050606788ull,
            10004784664717172195ull,
            15570268847930131473ull,
            10359715202835930803ull,
            17685054012115162812ull,
            13183273382855797757ull,
            7743260039872919062ull,
            9284593436392572926ull,
            11105921222066415013ull,
            18198799323400703846ull,
            16314988383739458320ull,
            4387527177871570570ull,
            8476708682254672590ull,
            4925096874831034057ull,
            14075687868072027455ull,
            112866656203221926ull,
            9852830467773230418ull,
            25755239915196746ull,
            2201493076310172510ull,
            8342165458688466438ull,
            13954006576066379050ull,
            15193819059903295636ull,
            12565616718911389531ull,
            3815854855847885129ull,
            15696762163583540628ull,
            805ull,
        } };

        // power_of_5[i] = 5 ** (2 ** i)
        static constexpr auto power_of_5 = [&] {
            constexpr size_t powers_count = 11;

            Array<UnsignedStorageReadonlySpan, powers_count> result;
            Array<size_t, powers_count> sizes = { 1, 1, 1, 1, 1, 2, 3, 5, 10, 19, 38 };

            size_t offset = 0;
            for (size_t i = 0; i < powers_count; ++i) {
                if constexpr (SameAs<NativeWord, u32>)
                    sizes[i] *= 2;
                result[i] = UnsignedStorageReadonlySpan(power_of_5_coefficients.span().slice(offset, sizes[i]));
                offset += sizes[i];
            }

            return result;
        }();

        VERIFY(exponent < (1 << power_of_5.size()));

        // Binary exponentiation
        for (size_t i = 0; i < power_of_5.size(); ++i) {
            if (exponent >> i & 1) {
                if (power_of_5[i].size() == 1) {
                    multiply_with_small(power_of_5[i][0]);
                } else {
                    auto copy = *this;
                    Ops::baseline_mul(copy.get_storage(), power_of_5[i],
                        get_storage(m_used_length + power_of_5[i].size()), g_null_allocator);
                    trim_last_word_if_zero();
                }
            }
        }
    }

    void multiply_with_power_of_2(u32 exponent)
    {
        if (exponent) {
            size_t max_new_length = m_used_length + (exponent + native_word_size - 1) / native_word_size;
            if (m_used_length != max_words_needed)
                m_words[m_used_length] = 0;
            auto storage = get_storage(max_new_length);

            Ops::shift_left(storage, exponent, storage);
            trim_last_word_if_zero();
        }
    }

    enum class CompareResult {
        Equal = 0,
        GreaterThan = 1,
        LessThan = -1
    };

    CompareResult compare_to(MinimalBigInt const& other) const
    {
        return static_cast<CompareResult>(Ops::compare(get_storage(), other.get_storage(), false));
    }

private:
    UnsignedStorageSpan get_storage(size_t new_length = 0)
    {
        if (new_length > m_used_length)
            m_used_length = min(max_words_needed, new_length);
        return { m_words.data(), m_used_length };
    }

    UnsignedStorageReadonlySpan get_storage() const
    {
        return { m_words.data(), m_used_length };
    }

    void trim_last_word_if_zero()
    {
        if (m_used_length > 0 && !m_words[m_used_length - 1])
            --m_used_length;
    }

    static constexpr Array<u64, 20> powers_of_ten_uint64 = {
        1UL, 10UL, 100UL, 1000UL, 10000UL, 100000UL, 1000000UL, 10000000UL, 100000000UL,
        1000000000UL, 10000000000UL, 100000000000UL, 1000000000000UL, 10000000000000UL,
        100000000000000UL, 1000000000000000UL, 10000000000000000UL, 100000000000000000UL,
        1000000000000000000UL, 10000000000000000000UL
    };

    void multiply_with_small(u64 value)
    {
        if (value <= max_native_word) {
            auto native_value = static_cast<NativeWord>(value);
            NativeWord carry = 0;
            for (size_t i = 0; i < m_used_length; ++i) {
                auto result = UFixedBigInt<native_word_size>(m_words[i]).wide_multiply(native_value) + carry;
                carry = result.high();
                m_words[i] = result.low();
            }
            if (carry != 0)
                m_words[m_used_length++] = carry;
        } else {
            // word_size == 32 && value > NumericLimits<u32>::max()
            auto copy = *this;
            Ops::baseline_mul(copy.get_storage(), Detail::get_storage_of(value), get_storage(m_used_length + 2), g_null_allocator);
            trim_last_word_if_zero();
        }
    }

    void add_small(u64 value)
    {
        if (m_used_length == 0 && value <= max_native_word) {
            m_words[m_used_length++] = static_cast<NativeWord>(value);
            return;
        }

        auto initial_storage = get_storage();
        auto expanded_storage = get_storage(max(m_used_length, words_in_u64));
        if (Ops::add<false>(initial_storage, Detail::get_storage_of(value), expanded_storage))
            m_words[m_used_length++] = 1;
    }

    void add_digits(u64 value, size_t digits_for_value)
    {
        VERIFY(digits_for_value < powers_of_ten_uint64.size());

        multiply_with_small(powers_of_ten_uint64[digits_for_value]);
        add_small(value);
    }

    // The max valid words we might need are log2(10^(769 + 342)), max digits + max exponent
    static constexpr size_t words_in_u64 = native_word_size == 64 ? 1 : 2;
    static constexpr size_t max_words_needed = 58 * words_in_u64;

    size_t m_used_length = 0;

    // FIXME: This is an array just to avoid allocations, but the max size is only needed for
    //        massive amount of digits, so a smaller vector would work for most cases.
    Array<NativeWord, max_words_needed> m_words {};
};

static bool round_nearest_tie_even(FloatingPointBuilder& value, bool did_truncate_bits, i32 shift)
{
    VERIFY(shift == 11 || shift == 40);
    u64 mask = (1ull << shift) - 1;
    u64 halfway = 1ull << (shift - 1);

    u64 truncated_bits = value.mantissa & mask;
    bool is_halfway = truncated_bits == halfway;
    bool is_above = truncated_bits > halfway;

    value.mantissa >>= shift;
    value.exponent += shift;

    bool is_odd = (value.mantissa & 1) == 1;
    return is_above || (is_halfway && did_truncate_bits) || (is_halfway && is_odd);
}

template<typename T, typename Callback>
static void round(FloatingPointBuilder& value, Callback&& should_round_up)
{
    using FloatingRepr = FloatingPointInfo<T>;

    i32 mantissa_shift = 64 - FloatingRepr::mantissa_bits() - 1;
    if (-value.exponent >= mantissa_shift) {
        // This is a denormal so we have to shift????
        mantissa_shift = min(-value.exponent + 1, 64);
        if (should_round_up(value, mantissa_shift))
            ++value.mantissa;

        value.exponent = (value.mantissa < (1ull << FloatingRepr::mantissa_bits())) ? 0 : 1;
        return;
    }

    if (should_round_up(value, mantissa_shift))
        ++value.mantissa;

    // Mantissa might have been rounded so if it overflowed increase the exponent
    if (value.mantissa >= (2ull << FloatingRepr::mantissa_bits())) {
        value.mantissa = 0;
        ++value.exponent;
    } else {
        // Clear the implicit top bit
        value.mantissa &= ~(1ull << FloatingRepr::mantissa_bits());
    }

    // If we also overflowed the exponent make it infinity!
    if (value.exponent >= FloatingRepr::infinity_exponent()) {
        value.exponent = FloatingRepr::infinity_exponent();
        value.mantissa = 0;
    }
}

template<typename T>
static FloatingPointBuilder build_positive_double(MinimalBigInt& mantissa, i32 exponent)
{
    mantissa.multiply_with_power_of_10(exponent);

    FloatingPointBuilder result {};
    bool should_round_up_ties = false;
    // First we get the 64 most significant bits WARNING not masked to real mantissa yet
    result.mantissa = mantissa.top_64_bits(should_round_up_ties);

    i32 bias = FloatingPointInfo<T>::mantissa_bits() + FloatingPointInfo<T>::exponent_bias();
    result.exponent = mantissa.size_in_bits() - 64 + bias;

    round<T>(result, [should_round_up_ties](FloatingPointBuilder& value, i32 shift) {
        return round_nearest_tie_even(value, should_round_up_ties, shift);
    });
    return result;
}

template<ParseableFloatingPoint T>
static FloatingPointBuilder build_negative_exponent_double(MinimalBigInt& mantissa, i32 exponent, FloatingPointBuilder initial)
{
    VERIFY(exponent < 0);

    // Building a fraction from a big integer is harder to understand
    // But fundamentely we have mantissa * 10^-e and so divide by 5^f

    auto parts_copy = initial;
    round<T>(parts_copy, [](FloatingPointBuilder& value, i32 shift) {
        if (shift == 64)
            value.mantissa = 0;
        else
            value.mantissa >>= shift;

        value.exponent += shift;

        return false;
    });

    T rounded_down_double_value = parts_copy.template to_value<T>(false);
    auto exact_halfway_builder = FloatingPointBuilder::from_value(rounded_down_double_value);
    // halfway is exactly just the next bit 1 (rest implicit zeros)
    exact_halfway_builder.mantissa <<= 1;
    exact_halfway_builder.mantissa += 1;
    --exact_halfway_builder.exponent;

    MinimalBigInt rounded_down_full_mantissa { exact_halfway_builder.mantissa };

    // Scale halfway up with 5**(-e)
    if (u32 power_of_5 = -exponent; power_of_5 != 0)
        rounded_down_full_mantissa.multiply_with_power_of_5(power_of_5);

    i32 power_of_2 = exact_halfway_builder.exponent - exponent;
    if (power_of_2 > 0) {
        // halfway has lower exponent scale up to real exponent
        rounded_down_full_mantissa.multiply_with_power_of_2(power_of_2);
    } else if (power_of_2 < 0) {
        // halfway has higher exponent scale original mantissa up to real halfway
        mantissa.multiply_with_power_of_2(-power_of_2);
    }

    auto compared_to_halfway = mantissa.compare_to(rounded_down_full_mantissa);

    round<T>(initial, [compared_to_halfway](FloatingPointBuilder& value, i32 shift) {
        if (shift == 64) {
            value.mantissa = 0;
        } else {
            value.mantissa >>= shift;
        }
        value.exponent += shift;

        if (compared_to_halfway == MinimalBigInt::CompareResult::GreaterThan)
            return true;
        if (compared_to_halfway == MinimalBigInt::CompareResult::LessThan)
            return false;

        return (value.mantissa & 1) == 1;
    });

    return initial;
}

template<typename T>
static FloatingPointBuilder parse_arbitrarily_long_floating_point(BasicParseResult& result, FloatingPointBuilder initial)
{
    VERIFY(initial.exponent < 0);
    initial.exponent += FloatingPointBuilder::invalid_exponent_offset;

    VERIFY(result.exponent >= NumericLimits<i32>::min() && result.exponent <= NumericLimits<i32>::max());
    i32 exponent = static_cast<i32>(result.exponent);
    {
        u64 mantissa_copy = result.mantissa;

        while (mantissa_copy >= 10000) {
            mantissa_copy /= 10000;
            exponent += 4;
        }

        while (mantissa_copy >= 10) {
            mantissa_copy /= 10;
            ++exponent;
        }
    }

    size_t digits = 0;

    constexpr auto max_digits_to_parse = FloatingPointInfo<T>::max_possible_digits_needed_for_parsing();

    // Reparse mantissa into big int
    auto mantissa = MinimalBigInt::from_decimal_floating_point(result, digits, max_digits_to_parse);

    VERIFY(digits <= 1024);

    exponent += 1 - static_cast<i32>(digits);

    if (exponent >= 0)
        return build_positive_double<T>(mantissa, exponent);

    return build_negative_exponent_double<T>(mantissa, exponent, initial);
}

template<FloatingPoint T>
T parse_result_to_value(BasicParseResult& parse_result)
{
    using FloatingPointRepr = FloatingPointInfo<T>;

    if (parse_result.mantissa <= u64(2) << FloatingPointRepr::mantissa_bits()
        && parse_result.exponent >= -FloatingPointRepr::max_exact_power_of_10() && parse_result.exponent <= FloatingPointRepr::max_exact_power_of_10()
        && !parse_result.more_than_19_digits_with_overflow) {

        T value = parse_result.mantissa;
        VERIFY(u64(value) == parse_result.mantissa);

        if (parse_result.exponent < 0)
            value = value / FloatingPointRepr::power_of_ten(-parse_result.exponent);
        else
            value = value * FloatingPointRepr::power_of_ten(parse_result.exponent);

        if (parse_result.negative)
            value = -value;

        return value;
    }

    auto floating_point_parts = binary_to_decimal<T>(parse_result.mantissa, parse_result.exponent);
    if (parse_result.more_than_19_digits_with_overflow && floating_point_parts.exponent >= 0) {
        auto rounded_up_double_build = binary_to_decimal<T>(parse_result.mantissa + 1, parse_result.exponent);
        if (floating_point_parts.mantissa != rounded_up_double_build.mantissa || floating_point_parts.exponent != rounded_up_double_build.exponent) {
            floating_point_parts = fallback_binary_to_decimal<T>(parse_result.mantissa, parse_result.exponent);
            VERIFY(floating_point_parts.exponent < 0);
        }
    }

    if (floating_point_parts.exponent < 0) {
        // Invalid have to parse perfectly
        floating_point_parts = parse_arbitrarily_long_floating_point<T>(parse_result, floating_point_parts);
    }

    return floating_point_parts.template to_value<T>(parse_result.negative);
}

template<FloatingPoint T>
constexpr FloatingPointParseResults<T> parse_result_to_full_result(BasicParseResult parse_result)
{
    if (!parse_result.valid)
        return { nullptr, FloatingPointError::NoOrInvalidInput, __builtin_nan("") };

    FloatingPointParseResults<T> full_result {};
    full_result.end_ptr = parse_result.last_parsed;

    // We special case this to be able to differentiate between 0 and values rounded down to 0
    if (parse_result.mantissa == 0) {
        full_result.value = parse_result.negative ? -0. : 0.;
        return full_result;
    }

    full_result.value = parse_result_to_value<T>(parse_result);

    // The only way we can get infinity is from rounding up/down to it.
    if (__builtin_isinf(full_result.value))
        full_result.error = FloatingPointError::OutOfRange;
    else if (full_result.value == T(0.))
        full_result.error = FloatingPointError::RoundedDownToZero;

    return full_result;
}

template<FloatingPoint T>
FloatingPointParseResults<T> parse_first_floating_point(char const* start, char const* end)
{
    auto parse_result = parse_numbers(
        start,
        [end](char const* head) { return head == end; },
        [end](char const* head) { return head - end >= 8; });

    return parse_result_to_full_result<T>(parse_result);
}

template FloatingPointParseResults<double> parse_first_floating_point(char const* start, char const* end);

template FloatingPointParseResults<float> parse_first_floating_point(char const* start, char const* end);

template<FloatingPoint T>
FloatingPointParseResults<T> parse_first_floating_point_until_zero_character(char const* start)
{
    auto parse_result = parse_numbers(
        start,
        [](char const* head) { return *head == '\0'; },
        [](char const*) { return false; });

    return parse_result_to_full_result<T>(parse_result);
}

template FloatingPointParseResults<double> parse_first_floating_point_until_zero_character(char const* start);

template FloatingPointParseResults<float> parse_first_floating_point_until_zero_character(char const* start);

template<FloatingPoint T>
Optional<T> parse_floating_point_completely(char const* start, char const* end)
{
    auto parse_result = parse_numbers(
        start,
        [end](char const* head) { return head == end; },
        [end](char const* head) { return head - end >= 8; });

    if (!parse_result.valid || parse_result.last_parsed != end)
        return {};

    return parse_result_to_value<T>(parse_result);
}

template Optional<double> parse_floating_point_completely(char const* start, char const* end);

template Optional<float> parse_floating_point_completely(char const* start, char const* end);

struct HexFloatParseResult {
    bool is_negative = false;
    bool valid = false;
    char const* last_parsed = nullptr;
    u64 mantissa = 0;
    i64 exponent = 0;
};

static HexFloatParseResult parse_hexfloat(char const* start)
{
    HexFloatParseResult result {};
    if (start == nullptr || *start == '\0')
        return result;

    char const* parse_head = start;
    bool any_digits = false;
    bool truncated_non_zero = false;

    if (*parse_head == '-') {
        result.is_negative = true;
        ++parse_head;

        if (*parse_head == '\0' || (!is_ascii_hex_digit(*parse_head) && *parse_head != floating_point_decimal_separator))
            return result;
    } else if (*parse_head == '+') {
        ++parse_head;

        if (*parse_head == '\0' || (!is_ascii_hex_digit(*parse_head) && *parse_head != floating_point_decimal_separator))
            return result;
    }
    if (*parse_head == '0' && (*(parse_head + 1) != '\0') && (*(parse_head + 1) == 'x' || *(parse_head + 1) == 'X')) {
        // Skip potential 0[xX], we have to do this here since the sign comes at the front
        parse_head += 2;
    }

    auto add_mantissa_digit = [&] {
        any_digits = true;

        // We assume you already checked this is actually a digit
        auto digit = parse_ascii_hex_digit(*parse_head);

        // Because the power of sixteen is just scaling of power of two we don't
        // need to keep all the remaining digits beyond the first 52 bits, just because
        // it's easy we store the first 16 digits. However for rounding we do need to parse
        // all the digits and keep track if we see any non zero one.
        if (result.mantissa < (1ull << 60)) {
            result.mantissa = (result.mantissa * 16) + digit;
            return true;
        }

        if (digit != 0)
            truncated_non_zero = true;

        return false;
    };

    while (*parse_head != '\0' && is_ascii_hex_digit(*parse_head)) {
        add_mantissa_digit();

        ++parse_head;
    }

    if (*parse_head != '\0' && *parse_head == floating_point_decimal_separator) {
        ++parse_head;
        i64 digits_after_separator = 0;
        while (*parse_head != '\0' && is_ascii_hex_digit(*parse_head)) {
            // Track how many characters we actually read into the mantissa
            digits_after_separator += add_mantissa_digit() ? 1 : 0;

            ++parse_head;
        }

        // We parsed x digits after the dot so need to multiply with 2^(-x * 4)
        // Since every digit is 4 bits
        result.exponent = -digits_after_separator * 4;
    }

    if (!any_digits)
        return result;

    if (*parse_head != '\0' && (*parse_head == 'p' || *parse_head == 'P')) {
        [&] {
            auto const* head_before_p = parse_head;
            ArmedScopeGuard reset_ptr { [&] { parse_head = head_before_p; } };
            ++parse_head;

            if (*parse_head == '\0')
                return;

            bool exponent_is_negative = false;
            i64 explicit_exponent = 0;

            if (*parse_head == '-' || *parse_head == '+') {
                exponent_is_negative = *parse_head == '-';
                ++parse_head;
                if (*parse_head == '\0')
                    return;
            }

            if (!is_ascii_digit(*parse_head))
                return;

            // We have at least one digit (with optional preceding sign) so we will not reset
            reset_ptr.disarm();

            while (*parse_head != '\0' && is_ascii_digit(*parse_head)) {
                // If we hit exponent overflow the number is so huge we are in trouble anyway, see
                // a comment in parse_numbers.
                if (explicit_exponent < 0x10000000)
                    explicit_exponent = 10 * explicit_exponent + (*parse_head - '0');
                ++parse_head;
            }

            if (exponent_is_negative)
                explicit_exponent = -explicit_exponent;

            result.exponent += explicit_exponent;
        }();
    }

    result.valid = true;

    // Round up exactly halfway with truncated non zeros, but don't if it would cascade up
    if (truncated_non_zero && (result.mantissa & 0xF) != 0xF) {
        VERIFY(result.mantissa >= 0x1000'0000'0000'0000);
        result.mantissa |= 1;
    }

    result.last_parsed = parse_head;

    return result;
}

template<FloatingPoint T>
static FloatingPointBuilder build_hex_float(HexFloatParseResult& parse_result)
{
    using FloatingPointRepr = FloatingPointInfo<T>;
    VERIFY(parse_result.mantissa != 0);

    if (parse_result.exponent >= FloatingPointRepr::infinity_exponent())
        return FloatingPointBuilder::infinity<T>();

    auto leading_zeros = count_leading_zeroes(parse_result.mantissa);
    u64 normalized_mantissa = parse_result.mantissa << leading_zeros;

    // No need to multiply with some power of 5 here the exponent is already a power of 2.

    u8 upperbit = normalized_mantissa >> 63;
    FloatingPointBuilder parts;
    parts.mantissa = normalized_mantissa >> (upperbit + 64 - FloatingPointRepr::mantissa_bits() - 3);

    parts.exponent = parse_result.exponent + upperbit - leading_zeros + FloatingPointRepr::exponent_bias() + 62;

    if (parts.exponent <= 0) {
        // subnormal
        if (-parts.exponent + 1 >= 64) {
            parts.mantissa = 0;
            parts.exponent = 0;
            return parts;
        }

        parts.mantissa >>= -parts.exponent + 1;
        parts.mantissa += parts.mantissa & 1;
        parts.mantissa >>= 1;

        if (parts.mantissa < (1ull << FloatingPointRepr::mantissa_bits())) {
            parts.exponent = 0;
        } else {
            parts.exponent = 1;
        }

        return parts;
    }

    // Here we don't have to only do this halfway check for some exponents
    if ((parts.mantissa & 0b11) == 0b01) {
        // effectively all discard bits from z.high are 0
        if (normalized_mantissa == (parts.mantissa << (upperbit + 64 - FloatingPointRepr::mantissa_bits() - 3)))
            parts.mantissa &= ~u64(1);
    }

    parts.mantissa += parts.mantissa & 1;
    parts.mantissa >>= 1;

    if (parts.mantissa >= (2ull << FloatingPointRepr::mantissa_bits())) {
        parts.mantissa = 1ull << FloatingPointRepr::mantissa_bits();
        ++parts.exponent;
    }

    parts.mantissa &= ~(1ull << FloatingPointRepr::mantissa_bits());

    if (parts.exponent >= FloatingPointRepr::infinity_exponent()) {
        parts.mantissa = 0;
        parts.exponent = FloatingPointRepr::infinity_exponent();
    }

    return parts;
}

template<FloatingPoint T>
FloatingPointParseResults<T> parse_first_hexfloat_until_zero_character(char const* start)
{
    using FloatingPointRepr = FloatingPointInfo<T>;
    auto parse_result = parse_hexfloat(start);

    if (!parse_result.valid)
        return { nullptr, FloatingPointError::NoOrInvalidInput, __builtin_nan("") };

    FloatingPointParseResults<T> full_result {};
    full_result.end_ptr = parse_result.last_parsed;

    // We special case this to be able to differentiate between 0 and values rounded down to 0

    if (parse_result.mantissa == 0) {
        full_result.value = 0.;
        return full_result;
    }

    auto result = build_hex_float<T>(parse_result);
    full_result.value = result.template to_value<T>(parse_result.is_negative);

    if (result.exponent == FloatingPointRepr::infinity_exponent()) {
        VERIFY(result.mantissa == 0);
        full_result.error = FloatingPointError::OutOfRange;
    } else if (result.mantissa == 0 && result.exponent == 0) {
        full_result.error = FloatingPointError::RoundedDownToZero;
    }

    return full_result;
}

template FloatingPointParseResults<double> parse_first_hexfloat_until_zero_character(char const* start);

template FloatingPointParseResults<float> parse_first_hexfloat_until_zero_character(char const* start);

}
