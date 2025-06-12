/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StdLibExtraDetails.h>

namespace Crypto {

class SignedBigInteger;
class UnsignedBigInteger;

template<typename T>
concept BigInteger = IsSame<T, SignedBigInteger> || IsSame<T, UnsignedBigInteger>;

}
