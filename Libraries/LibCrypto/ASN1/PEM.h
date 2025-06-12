/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>

namespace Crypto {

enum class PEMType {
    Unknown,
    Certificate,
    PrivateKey,
    PublicKey,
    RSAPrivateKey,
    RSAPublicKey
};

struct DecodedPEM {
    PEMType type { PEMType::Unknown };
    ByteBuffer data;
};

DecodedPEM decode_pem(ReadonlyBytes);
ErrorOr<Vector<DecodedPEM>> decode_pems(ReadonlyBytes);
ErrorOr<ByteBuffer> encode_pem(ReadonlyBytes, PEMType = PEMType::Certificate);

}
