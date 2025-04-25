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

}
