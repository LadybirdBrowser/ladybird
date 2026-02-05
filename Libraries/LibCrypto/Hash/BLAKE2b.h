/*
 * Copyright (c) 2023, the SerenityOS developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <LibCrypto/Hash/OpenSSLHashFunction.h>

namespace Crypto::Hash {

class BLAKE2b final : public OpenSSLHashFunction<BLAKE2b, 1024, 512> {
    AK_MAKE_NONCOPYABLE(BLAKE2b);

public:
    explicit BLAKE2b(EVP_MD_CTX* context);

    virtual ByteString class_name() const override
    {
        return "BLAKE2b";
    }
};

};
