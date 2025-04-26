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

}
