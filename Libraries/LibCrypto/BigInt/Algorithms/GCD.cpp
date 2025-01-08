/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2020-2021, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2024, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "UnsignedBigIntegerAlgorithms.h"

namespace Crypto {

void UnsignedBigIntegerAlgorithms::destructive_GCD_without_allocation(
    UnsignedBigInteger& temp_a,
    UnsignedBigInteger& temp_b,
    UnsignedBigInteger& temp_quotient,
    UnsignedBigInteger& temp_remainder,
    UnsignedBigInteger& output)
{
    for (;;) {
        if (temp_a == 0) {
            output.set_to(temp_b);
            return;
        }

        // temp_b %= temp_a
        divide_without_allocation(temp_b, temp_a, temp_quotient, temp_remainder);
        temp_b.set_to(temp_remainder);
        if (temp_b == 0) {
            output.set_to(temp_a);
            return;
        }

        // temp_a %= temp_b
        divide_without_allocation(temp_a, temp_b, temp_quotient, temp_remainder);
        temp_a.set_to(temp_remainder);
    }
}

void UnsignedBigIntegerAlgorithms::extended_GCD_without_allocation(
    UnsignedBigInteger const& a,
    UnsignedBigInteger const& b,
    UnsignedBigInteger& x,
    UnsignedBigInteger& y,
    UnsignedBigInteger& gcd,
    UnsignedBigInteger& temp_quotient,
    UnsignedBigInteger& temp_1,
    UnsignedBigInteger& temp_2,
    UnsignedBigInteger& temp_shift,
    UnsignedBigInteger& temp_r,
    UnsignedBigInteger& temp_s,
    UnsignedBigInteger& temp_t)
{
    gcd.set_to(a);
    x.set_to(1);
    y.set_to(0);

    temp_r.set_to(b);
    temp_s.set_to_0();
    temp_t.set_to(1);

    while (temp_r != 0) {
        // quotient := old_r div r
        divide_without_allocation(gcd, temp_r, temp_quotient, temp_1);

        temp_2.set_to(temp_r);
        multiply_without_allocation(temp_quotient, temp_r, temp_shift, temp_1);
        while (gcd < temp_1) {
            add_into_accumulator_without_allocation(gcd, b);
        }
        subtract_without_allocation(gcd, temp_1, temp_r);
        gcd.set_to(temp_2);

        // (old_s, s) := (s, old_s − quotient × s)
        temp_2.set_to(temp_s);
        multiply_without_allocation(temp_quotient, temp_s, temp_shift, temp_1);
        while (x < temp_1) {
            add_into_accumulator_without_allocation(x, b);
        }
        subtract_without_allocation(x, temp_1, temp_s);
        x.set_to(temp_2);

        // (old_t, t) := (t, old_t − quotient × t)
        temp_2.set_to(temp_t);
        multiply_without_allocation(temp_quotient, temp_t, temp_shift, temp_1);
        while (y < temp_1) {
            add_into_accumulator_without_allocation(y, b);
        }
        subtract_without_allocation(y, temp_1, temp_t);
        y.set_to(temp_2);
    }
}

}
