/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/Hash/Hash.h>

namespace Crypto::Hash {

ErrorOr<char const*> openssl_digest_name(HashKind kind)
{
    switch (kind) {
    case HashKind::SHA1:
        return "SHA1";
    case HashKind::SHA256:
        return "SHA256";
    case HashKind::SHA384:
        return "SHA384";
    case HashKind::SHA512:
        return "SHA512";
    case HashKind::SHA3_256:
        return "SHA3-256";
    case HashKind::SHA3_384:
        return "SHA3-384";
    case HashKind::SHA3_512:
        return "SHA3-512";
    case HashKind::Unknown:
    case HashKind::None:
        break;
    }
    return Error::from_string_literal("Unsupported hash kind");
}

}
