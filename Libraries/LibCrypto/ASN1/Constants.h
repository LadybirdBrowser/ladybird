/*
 * Copyright (c) 2020-2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCrypto/ASN1/DER.h>

namespace Crypto::ASN1 {

constexpr static Array<u32, 7>
    rsa_encryption_oid { 1, 2, 840, 113549, 1, 1, 1 },
    secp256r1_oid { 1, 2, 840, 10045, 3, 1, 7 };

constexpr static Array<u32, 6>
    ec_public_key_encryption_oid { 1, 2, 840, 10045, 2, 1 };

constexpr static Array<u32, 5>
    secp384r1_oid { 1, 3, 132, 0, 34 },
    secp521r1_oid { 1, 3, 132, 0, 35 };

constexpr static Array<u32, 4>
    x25519_oid { 1, 3, 101, 110 },
    x448_oid { 1, 3, 101, 111 },
    ed25519_oid { 1, 3, 101, 112 },
    ed448_oid { 1, 3, 101, 113 };

constexpr static Array<u32, 9>
    ml_dsa_44_oid { 2, 16, 840, 1, 101, 3, 4, 3, 17 },
    ml_dsa_65_oid { 2, 16, 840, 1, 101, 3, 4, 3, 18 },
    ml_dsa_87_oid { 2, 16, 840, 1, 101, 3, 4, 3, 19 };

constexpr static Array<u32, 9>
    ml_kem_512_oid { 2, 16, 840, 1, 101, 3, 4, 4, 1 },
    ml_kem_768_oid { 2, 16, 840, 1, 101, 3, 4, 4, 2 },
    ml_kem_1024_oid { 2, 16, 840, 1, 101, 3, 4, 4, 3 };

}
