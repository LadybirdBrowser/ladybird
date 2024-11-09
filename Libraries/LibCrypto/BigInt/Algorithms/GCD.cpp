/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2020-2021, Dex♪ <dexes.ttp@gmail.com>
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

}
