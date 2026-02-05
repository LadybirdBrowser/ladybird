/*
 * Copyright (c) 2025, mikiubo <michele.uboldi@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/Noncopyable.h>
#include <LibCrypto/OpenSSLForward.h>

namespace Crypto::Hash {

enum class SHAKEKind {
    CSHAKE128,
    CSHAKE256
};

class SHAKE {
    AK_MAKE_NONCOPYABLE(SHAKE);

public:
    explicit SHAKE(SHAKEKind);

    ~SHAKE() = default;

    ErrorOr<ByteBuffer> digest(
        ReadonlyBytes data,
        u32 length,
        Optional<ReadonlyBytes> customization,
        Optional<ReadonlyBytes> function_name) const;

private:
    EVP_MD const* m_md;
};

}
