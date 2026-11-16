/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Types.h>

namespace Crypto::Hash {

// NB: libcrypto_rust passes these values across the FFI.
enum class HashKind {
    Unknown,
    None,
    SHA1,
    SHA256,
    SHA384,
    SHA512,
    SHA3_256,
    SHA3_384,
    SHA3_512
};

ErrorOr<char const*> openssl_digest_name(HashKind);

}
