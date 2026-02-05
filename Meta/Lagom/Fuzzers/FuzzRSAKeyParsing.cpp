/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/PK/RSA.h>
#include <stddef.h>
#include <stdint.h>

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    AK::set_debug_enabled(false);
    (void)Crypto::PK::RSA::parse_rsa_key({ data, size }, true, {});
    (void)Crypto::PK::RSA::parse_rsa_key({ data, size }, false, {});
    return 0;
}
