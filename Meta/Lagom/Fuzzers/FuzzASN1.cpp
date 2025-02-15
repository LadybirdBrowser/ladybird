/*
 * Copyright (c) 2021, Brian Gianforcaro <bgianf@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/ASN1/DER.h>
#include <stddef.h>
#include <stdint.h>

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    AK::set_debug_enabled(false);

    Crypto::ASN1::Decoder decoder(ReadonlyBytes { data, size });
    while (!decoder.eof())
        (void)decoder.drop();

    return 0;
}
