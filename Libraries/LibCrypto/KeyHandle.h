/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <AK/kmalloc.h>
#include <LibCrypto/PK/EC.h>
#include <LibCrypto/PK/MLDSA.h>
#include <LibCrypto/PK/MLKEM.h>
#include <LibCrypto/PK/RSA.h>

namespace Crypto {

// The raw key octets of an Ed25519, Ed448, X25519, or X448 ("OKP") key.
struct OKPPublicKey {
    ByteBuffer bytes;
};

struct OKPPrivateKey {
    ByteBuffer bytes;
};

struct KeyHandle final : Variant<ByteBuffer, PK::RSAPublicKey, PK::RSAPrivateKey, PK::ECPublicKey, PK::ECPrivateKey, PK::MLDSAPublicKey, PK::MLDSAPrivateKey, PK::MLKEMPublicKey, PK::MLKEMPrivateKey, OKPPublicKey, OKPPrivateKey> {
    AK_ALLOC_WITH_KMALLOC;

    using Variant::Variant;
};

// These tags identify the key components in stored CryptoKey records.
enum class HandleTag : u8 {
    ByteBuffer = 0,
    RSAPublicKey = 1,
    RSAPrivateKey = 2,
    ECPublicKey = 3,
    ECPrivateKey = 4,
    MLDSAPublicKey = 5,
    MLDSAPrivateKey = 6,
    MLKEMPublicKey = 7,
    MLKEMPrivateKey = 8,
    OKPPublicKey = 9,
    OKPPrivateKey = 10,
};

}
