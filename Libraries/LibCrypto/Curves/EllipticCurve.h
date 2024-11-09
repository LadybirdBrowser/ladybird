/*
 * Copyright (c) 2022, Michiel Visser <opensource@webmichiel.nl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>

namespace Crypto::Curves {

class EllipticCurve {
public:
    virtual size_t key_size() = 0;
    virtual ErrorOr<ByteBuffer> generate_private_key() = 0;
    virtual ErrorOr<ByteBuffer> generate_public_key(ReadonlyBytes a) = 0;
    virtual ErrorOr<ByteBuffer> compute_coordinate(ReadonlyBytes scalar_bytes, ReadonlyBytes point_bytes) = 0;
    virtual ErrorOr<ByteBuffer> derive_premaster_key(ReadonlyBytes shared_point) = 0;

    virtual ~EllipticCurve() = default;
};

}
