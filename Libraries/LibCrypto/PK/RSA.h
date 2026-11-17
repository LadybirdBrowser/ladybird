/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2022, the SerenityOS developers.
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Span.h>
#include <LibCrypto/Hash/Hash.h>

namespace Crypto::PK {

ErrorOr<bool> rsa_pkcs1_verify(ReadonlyBytes modulus, ReadonlyBytes public_exponent, Hash::HashKind, ReadonlyBytes message, ReadonlyBytes signature);

}
