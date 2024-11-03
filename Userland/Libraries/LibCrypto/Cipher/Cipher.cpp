/*
 * Copyright (c) 2024, Ben Wiederhake
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/Cipher/Cipher.h>

namespace Crypto::Cipher {

bool padding_always_needs_extra_block(PaddingMode mode)
{
    switch (mode) {
    case PaddingMode::CMS:
        return true;
    case PaddingMode::RFC5246:
    case PaddingMode::Null:
    case PaddingMode::Bit:
    case PaddingMode::Random:
    case PaddingMode::Space:
    case PaddingMode::ZeroLength:
        return false;
    }
    VERIFY_NOT_REACHED();
}

}
