/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <LibCrypto/Hash/OpenSSLHashFunction.h>

namespace Crypto::Hash {

class SHA256 final : public OpenSSLHashFunction<SHA256, 512, 256> {
    AK_MAKE_NONCOPYABLE(SHA256);

public:
    explicit SHA256(EVP_MD_CTX* context);

    virtual ByteString class_name() const override
    {
        return "SHA256";
    }
};

class SHA384 final : public OpenSSLHashFunction<SHA384, 1024, 384> {
    AK_MAKE_NONCOPYABLE(SHA384);

public:
    explicit SHA384(EVP_MD_CTX* context);

    virtual ByteString class_name() const override
    {
        return "SHA384";
    }
};

class SHA512 final : public OpenSSLHashFunction<SHA512, 1024, 512> {
    AK_MAKE_NONCOPYABLE(SHA512);

public:
    explicit SHA512(EVP_MD_CTX* context);

    virtual ByteString class_name() const override
    {
        return "SHA512";
    }
};

}
