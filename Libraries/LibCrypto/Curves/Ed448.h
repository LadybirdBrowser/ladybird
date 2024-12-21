/*
 * Copyright (c) 2024, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>

namespace Crypto::Curves {

class Ed448 {
public:
    constexpr size_t key_size() const { return 57; }
    constexpr size_t signature_size() const { return 114; }
    ErrorOr<ByteBuffer> generate_private_key();
    ErrorOr<ByteBuffer> generate_public_key(ReadonlyBytes private_key);

    ErrorOr<ByteBuffer> sign(ReadonlyBytes private_key, ReadonlyBytes message, ReadonlyBytes context = {});
    ErrorOr<bool> verify(ReadonlyBytes public_key, ReadonlyBytes signature, ReadonlyBytes message, ReadonlyBytes context = {});
};

}
