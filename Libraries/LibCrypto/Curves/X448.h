/*
 * Copyright (c) 2022, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>

namespace Crypto::Curves {

class X448 {
public:
    size_t key_size() { return 56; }
    ErrorOr<ByteBuffer> generate_private_key();
    ErrorOr<ByteBuffer> generate_public_key(ReadonlyBytes a);
    ErrorOr<ByteBuffer> compute_coordinate(ReadonlyBytes a, ReadonlyBytes b);
    ErrorOr<ByteBuffer> derive_premaster_key(ReadonlyBytes shared_point);
};

}
