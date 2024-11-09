/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Random.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>

namespace Crypto::NumberTheory {

UnsignedBigInteger Mod(UnsignedBigInteger const& a, UnsignedBigInteger const& b);
UnsignedBigInteger ModularInverse(UnsignedBigInteger const& a_, UnsignedBigInteger const& b);
UnsignedBigInteger ModularPower(UnsignedBigInteger const& b, UnsignedBigInteger const& e, UnsignedBigInteger const& m);

// Note: This function _will_ generate extremely huge numbers, and in doing so,
//       it will allocate and free a lot of memory!
//       Please use |ModularPower| if your use-case is modexp.
template<typename IntegerType>
static IntegerType Power(IntegerType const& b, IntegerType const& e)
{
    IntegerType ep { e };
    IntegerType base { b };
    IntegerType exp { 1 };

    while (!(ep < IntegerType { 1 })) {
        if (ep.words()[0] % 2 == 1)
            exp.set_to(exp.multiplied_by(base));

        // ep = ep / 2;
        ep.set_to(ep.shift_right(1));

        // base = base * base
        base.set_to(base.multiplied_by(base));
    }

    return exp;
}

UnsignedBigInteger GCD(UnsignedBigInteger const& a, UnsignedBigInteger const& b);
UnsignedBigInteger LCM(UnsignedBigInteger const& a, UnsignedBigInteger const& b);

UnsignedBigInteger random_number(UnsignedBigInteger const& min, UnsignedBigInteger const& max_excluded);
bool is_probably_prime(UnsignedBigInteger const& p);
UnsignedBigInteger random_big_prime(size_t bits);

}
