/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2020-2021, Dex♪ <dexes.ttp@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "UnsignedBigIntegerAlgorithms.h"

namespace Crypto {

void UnsignedBigIntegerAlgorithms::modular_inverse_without_allocation(
    UnsignedBigInteger const& a,
    UnsignedBigInteger const& b,
    UnsignedBigInteger& temp_1,
    UnsignedBigInteger& temp_minus,
    UnsignedBigInteger& temp_quotient,
    UnsignedBigInteger& temp_d,
    UnsignedBigInteger& temp_u,
    UnsignedBigInteger& temp_v,
    UnsignedBigInteger& temp_x,
    UnsignedBigInteger& result)
{
    UnsignedBigInteger one { 1 };

    temp_u.set_to(a);
    if (!a.is_odd()) {
        // u += b
        add_into_accumulator_without_allocation(temp_u, b);
    }

    temp_v.set_to(b);
    temp_x.set_to(0);

    // d = b - 1
    subtract_without_allocation(b, one, temp_d);

    while (!(temp_v == 1)) {
        while (temp_v < temp_u) {
            // u -= v
            subtract_without_allocation(temp_u, temp_v, temp_minus);
            temp_u.set_to(temp_minus);

            // d += x
            add_into_accumulator_without_allocation(temp_d, temp_x);

            while (!temp_u.is_odd()) {
                if (temp_d.is_odd()) {
                    // d += b
                    add_into_accumulator_without_allocation(temp_d, b);
                }

                // u /= 2
                divide_u16_without_allocation(temp_u, 2, temp_quotient, temp_1);
                temp_u.set_to(temp_quotient);

                // d /= 2
                divide_u16_without_allocation(temp_d, 2, temp_quotient, temp_1);
                temp_d.set_to(temp_quotient);
            }
        }

        // v -= u
        subtract_without_allocation(temp_v, temp_u, temp_minus);
        temp_v.set_to(temp_minus);

        // x += d
        add_into_accumulator_without_allocation(temp_x, temp_d);

        while (!temp_v.is_odd()) {
            if (temp_x.is_odd()) {
                // x += b
                add_into_accumulator_without_allocation(temp_x, b);
            }

            // v /= 2
            divide_u16_without_allocation(temp_v, 2, temp_quotient, temp_1);
            temp_v.set_to(temp_quotient);

            // x /= 2
            divide_u16_without_allocation(temp_x, 2, temp_quotient, temp_1);
            temp_x.set_to(temp_quotient);
        }
    }

    // return x % b
    divide_without_allocation(temp_x, b, temp_quotient, result);
}

}
