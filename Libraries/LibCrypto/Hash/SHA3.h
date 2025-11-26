/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <LibCrypto/Hash/OpenSSLHashFunction.h>

namespace Crypto::Hash {

class SHA3_256 final : public OpenSSLHashFunction<SHA3_256, 1088, 256> {
    AK_MAKE_NONCOPYABLE(SHA3_256);

public:
    explicit SHA3_256(EVP_MD_CTX* context);

    virtual ByteString class_name() const override
    {
        return "SHA3-256";
    }
};

class SHA3_384 final : public OpenSSLHashFunction<SHA3_384, 832, 384> {
    AK_MAKE_NONCOPYABLE(SHA3_384);

public:
    explicit SHA3_384(EVP_MD_CTX* context);

    virtual ByteString class_name() const override
    {
        return "SHA3-384";
    }
};

class SHA3_512 final : public OpenSSLHashFunction<SHA3_512, 576, 512> {
    AK_MAKE_NONCOPYABLE(SHA3_512);

public:
    explicit SHA3_512(EVP_MD_CTX* context);

    virtual ByteString class_name() const override
    {
        return "SHA3-512";
    }
};

}
