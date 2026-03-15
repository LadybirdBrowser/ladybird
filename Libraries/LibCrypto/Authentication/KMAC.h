/*
 * Copyright (c) 2026, mikiubo <michele.uboldi@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/Noncopyable.h>
#include <LibCrypto/OpenSSLForward.h>

namespace Crypto::Authentication {

enum class KMACKind {
    KMAC128,
    KMAC256,
};

class KMAC {
    AK_MAKE_NONCOPYABLE(KMAC);

public:
    explicit KMAC(KMACKind kind);

    ~KMAC() = default;

    static Optional<KMACKind> kind_from_algorithm_name(StringView);
    static u32 default_key_length(KMACKind);

    ErrorOr<ByteBuffer> sign(
        ReadonlyBytes key,
        ReadonlyBytes message,
        u32 output_length_bits,
        Optional<ReadonlyBytes> customization) const;

private:
    KMACKind m_kind;
};

}
