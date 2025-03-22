/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Random.h>
#include <LibCrypto/BigInt/Algorithms/UnsignedBigIntegerAlgorithms.h>
#include <LibCrypto/NumberTheory/ModularFunctions.h>
#include <LibCrypto/SecureRandom.h>

namespace Crypto::NumberTheory {

UnsignedBigInteger Mod(UnsignedBigInteger const& a, UnsignedBigInteger const& b)
{
    UnsignedBigInteger result;
    result.set_to(a);
    result.set_to(result.divided_by(b).remainder);
    return result;
}

UnsignedBigInteger ModularInverse(UnsignedBigInteger const& a, UnsignedBigInteger const& b)
{
    if (b == 1)
        return { 1 };

    UnsignedBigInteger result;
    UnsignedBigInteger temp_y;
    UnsignedBigInteger temp_gcd;
    UnsignedBigInteger temp_quotient;
    UnsignedBigInteger temp_1;
    UnsignedBigInteger temp_2;
    UnsignedBigInteger temp_shift;
    UnsignedBigInteger temp_r;
    UnsignedBigInteger temp_s;
    UnsignedBigInteger temp_t;

    UnsignedBigIntegerAlgorithms::modular_inverse_without_allocation(a, b, result, temp_y, temp_gcd, temp_quotient, temp_1, temp_2, temp_shift, temp_r, temp_s, temp_t);

    return result;
}

UnsignedBigInteger ModularPower(UnsignedBigInteger const& b, UnsignedBigInteger const& e, UnsignedBigInteger const& m)
{
    if (m == 1)
        return 0;

    if (m.is_odd()) {
        UnsignedBigInteger temp_z0 { 0 };
        UnsignedBigInteger temp_rr { 0 };
        UnsignedBigInteger temp_one { 0 };
        UnsignedBigInteger temp_z { 0 };
        UnsignedBigInteger temp_zz { 0 };
        UnsignedBigInteger temp_x { 0 };
        UnsignedBigInteger temp_extra { 0 };

        UnsignedBigInteger result;
        UnsignedBigIntegerAlgorithms::montgomery_modular_power_with_minimal_allocations(b, e, m, temp_z0, temp_rr, temp_one, temp_z, temp_zz, temp_x, temp_extra, result);
        return result;
    }

    UnsignedBigInteger ep { e };
    UnsignedBigInteger base { b };

    UnsignedBigInteger result;
    UnsignedBigInteger temp_1;
    UnsignedBigInteger temp_multiply;
    UnsignedBigInteger temp_quotient;
    UnsignedBigInteger temp_remainder;

    UnsignedBigIntegerAlgorithms::destructive_modular_power_without_allocation(ep, base, m, temp_1, temp_multiply, temp_quotient, temp_remainder, result);

    return result;
}

UnsignedBigInteger GCD(UnsignedBigInteger const& a, UnsignedBigInteger const& b)
{
    UnsignedBigInteger temp_a { a };
    UnsignedBigInteger temp_b { b };
    UnsignedBigInteger temp_quotient;
    UnsignedBigInteger temp_remainder;
    UnsignedBigInteger output;

    UnsignedBigIntegerAlgorithms::destructive_GCD_without_allocation(temp_a, temp_b, temp_quotient, temp_remainder, output);

    return output;
}

UnsignedBigInteger LCM(UnsignedBigInteger const& a, UnsignedBigInteger const& b)
{
    UnsignedBigInteger temp_a { a };
    UnsignedBigInteger temp_b { b };
    UnsignedBigInteger temp_1;
    UnsignedBigInteger temp_2;
    UnsignedBigInteger temp_3;
    UnsignedBigInteger temp_quotient;
    UnsignedBigInteger temp_remainder;
    UnsignedBigInteger gcd_output;
    UnsignedBigInteger output { 0 };

    UnsignedBigIntegerAlgorithms::destructive_GCD_without_allocation(temp_a, temp_b, temp_quotient, temp_remainder, gcd_output);
    if (gcd_output == 0) {
        dbgln_if(NT_DEBUG, "GCD is zero");
        return output;
    }

    // output = (a / gcd_output) * b
    UnsignedBigIntegerAlgorithms::divide_without_allocation(a, gcd_output, temp_quotient, temp_remainder);
    UnsignedBigIntegerAlgorithms::multiply_without_allocation(temp_quotient, b, temp_1, output);

    dbgln_if(NT_DEBUG, "quot: {} rem: {} out: {}", temp_quotient, temp_remainder, output);

    return output;
}

static bool MR_primality_test(UnsignedBigInteger n, Vector<UnsignedBigInteger, 256> const& tests)
{
    // Written using Wikipedia:
    // https://en.wikipedia.org/wiki/Miller%E2%80%93Rabin_primality_test#Miller%E2%80%93Rabin_test
    VERIFY(!(n < 4));
    auto predecessor = n.minus({ 1 });
    auto d = predecessor;
    size_t r = 0;

    {
        auto div_result = d.divided_by(2);
        while (div_result.remainder == 0) {
            d = div_result.quotient;
            div_result = d.divided_by(2);
            ++r;
        }
    }
    if (r == 0) {
        // n - 1 is odd, so n was even. But there is only one even prime:
        return n == 2;
    }

    for (auto& a : tests) {
        // Technically: VERIFY(2 <= a && a <= n - 2)
        VERIFY(a < n);
        auto x = ModularPower(a, d, n);
        if (x == 1 || x == predecessor)
            continue;
        bool skip_this_witness = false;
        // r − 1 iterations.
        for (size_t i = 0; i < r - 1; ++i) {
            x = ModularPower(x, 2, n);
            if (x == predecessor) {
                skip_this_witness = true;
                break;
            }
        }
        if (skip_this_witness)
            continue;
        return false; // "composite"
    }

    return true; // "probably prime"
}

UnsignedBigInteger random_number(UnsignedBigInteger const& min, UnsignedBigInteger const& max_excluded)
{
    VERIFY(min < max_excluded);
    auto range = max_excluded.minus(min);
    UnsignedBigInteger base;
    auto size = range.trimmed_length() * sizeof(u32) + 2;
    // "+2" is intentional (see below).
    auto buffer = ByteBuffer::create_uninitialized(size).release_value_but_fixme_should_propagate_errors(); // FIXME: Handle possible OOM situation.
    auto* buf = buffer.data();

    fill_with_secure_random(buffer);
    UnsignedBigInteger random { buf, size };
    // At this point, `random` is a large number, in the range [0, 256^size).
    // To get down to the actual range, we could just compute random % range.
    // This introduces "modulo bias". However, since we added 2 to `size`,
    // we know that the generated range is at least 65536 times as large as the
    // required range! This means that the modulo bias is only 0.0015%, if all
    // inputs are chosen adversarially. Let's hope this is good enough.
    auto divmod = random.divided_by(range);
    // The proper way to fix this is to restart if `divmod.quotient` is maximal.
    return divmod.remainder.plus(min);
}

bool is_probably_prime(UnsignedBigInteger const& p)
{
    // Is it a small number?
    if (p < 49) {
        u32 p_value = p.words()[0];
        // Is it a very small prime?
        if (p_value == 2 || p_value == 3 || p_value == 5 || p_value == 7)
            return true;
        // Is it the multiple of a very small prime?
        if (p_value % 2 == 0 || p_value % 3 == 0 || p_value % 5 == 0 || p_value % 7 == 0)
            return false;
        // Then it must be a prime, but not a very small prime, like 37.
        return true;
    }

    Vector<UnsignedBigInteger, 256> tests;
    // Make some good initial guesses that are guaranteed to find all primes < 2^64.
    tests.append(UnsignedBigInteger(2));
    tests.append(UnsignedBigInteger(3));
    tests.append(UnsignedBigInteger(5));
    tests.append(UnsignedBigInteger(7));
    tests.append(UnsignedBigInteger(11));
    tests.append(UnsignedBigInteger(13));
    UnsignedBigInteger seventeen { 17 };
    for (size_t i = tests.size(); i < 256; ++i) {
        tests.append(random_number(seventeen, p.minus(2)));
    }
    // Miller-Rabin's "error" is 8^-k. In adversarial cases, it's 4^-k.
    // With 200 random numbers, this would mean an error of about 2^-400.
    // So we don't need to worry too much about the quality of the random numbers.

    return MR_primality_test(p, tests);
}

UnsignedBigInteger random_big_prime(size_t bits)
{
    VERIFY(bits >= 33);
    UnsignedBigInteger min = "6074001000"_bigint.shift_left(bits - 33);
    UnsignedBigInteger max = UnsignedBigInteger { 1 }.shift_left(bits).minus(1);
    for (;;) {
        auto p = random_number(min, max);
        if ((p.words()[0] & 1) == 0) {
            // An even number is definitely not a large prime.
            continue;
        }
        if (is_probably_prime(p))
            return p;
    }
}

}
