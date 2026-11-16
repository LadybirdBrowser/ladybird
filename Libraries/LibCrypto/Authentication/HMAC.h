/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <LibCrypto/Hash/Hash.h>

namespace Crypto::Authentication {

ErrorOr<Hash::Digest> hmac(Hash::HashKind, ReadonlyBytes key, ReadonlyBytes message);

}
