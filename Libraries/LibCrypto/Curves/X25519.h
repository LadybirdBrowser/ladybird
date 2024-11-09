/*
 * Copyright (c) 2022, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <LibCrypto/Curves/EllipticCurve.h>

namespace Crypto::Curves {

class X25519 : public EllipticCurve {
public:
    size_t key_size() override { return 32; }
    ErrorOr<ByteBuffer> generate_private_key() override;
    ErrorOr<ByteBuffer> generate_public_key(ReadonlyBytes a) override;
    ErrorOr<ByteBuffer> compute_coordinate(ReadonlyBytes a, ReadonlyBytes b) override;
    ErrorOr<ByteBuffer> derive_premaster_key(ReadonlyBytes shared_point) override;
};

}
