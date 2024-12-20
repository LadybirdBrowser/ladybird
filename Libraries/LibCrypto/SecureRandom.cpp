/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/SecureRandom.h>

#include <openssl/rand.h>

namespace Crypto {

void fill_with_secure_random(Bytes bytes)
{
    auto const size = static_cast<int>(bytes.size());

    if (RAND_bytes(bytes.data(), size) != 1)
        VERIFY_NOT_REACHED();
}

}
