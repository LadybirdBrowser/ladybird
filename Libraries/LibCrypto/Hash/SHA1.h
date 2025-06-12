/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <LibCrypto/Hash/OpenSSLHashFunction.h>

namespace Crypto::Hash {

class SHA1 final : public OpenSSLHashFunction<SHA1, 512, 160> {
    AK_MAKE_NONCOPYABLE(SHA1);

public:
    explicit SHA1(EVP_MD_CTX* context);

    virtual ByteString class_name() const override
    {
        return "SHA1";
    }
};

}
