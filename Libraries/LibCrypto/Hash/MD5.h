/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <LibCrypto/Hash/OpenSSLHashFunction.h>

namespace Crypto::Hash {

class MD5 final : public OpenSSLHashFunction<MD5, 512, 128> {
    AK_MAKE_NONCOPYABLE(MD5);

public:
    explicit MD5(EVP_MD_CTX* context);

    virtual ByteString class_name() const override
    {
        return "MD5";
    }
};

}
