/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2020-2021, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2024, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "UnsignedBigIntegerAlgorithms.h"

namespace Crypto {

void UnsignedBigIntegerAlgorithms::modular_inverse_without_allocation(
    UnsignedBigInteger const& a,
    UnsignedBigInteger const& b,
    UnsignedBigInteger& result,
    UnsignedBigInteger& temp_y,
    UnsignedBigInteger& temp_gcd,
    UnsignedBigInteger& temp_quotient,
    UnsignedBigInteger& temp_1,
    UnsignedBigInteger& temp_2,
    UnsignedBigInteger& temp_shift,
    UnsignedBigInteger& temp_r,
    UnsignedBigInteger& temp_s,
    UnsignedBigInteger& temp_t)
{
    extended_GCD_without_allocation(a, b, result, temp_y, temp_gcd, temp_quotient, temp_1, temp_2, temp_shift, temp_r, temp_s, temp_t);

    divide_without_allocation(result, b, temp_quotient, temp_1);
    add_into_accumulator_without_allocation(temp_1, b);
    divide_without_allocation(temp_1, b, temp_quotient, result);
}

}
