/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCrypto/Hash/HashFunction.h>
#include <LibCrypto/Verification.h>

namespace Crypto::PK {

template<typename HashFunction>
class Code {
public:
    template<typename... Args>
    Code(Args... args)
        : m_hasher(args...)
    {
    }

    virtual void encode(ReadonlyBytes in, ByteBuffer& out, size_t em_bits) = 0;
    virtual VerificationConsistency verify(ReadonlyBytes msg, ReadonlyBytes emsg, size_t em_bits) = 0;

    HashFunction const& hasher() const { return m_hasher; }
    HashFunction& hasher() { return m_hasher; }

protected:
    virtual ~Code() = default;

    HashFunction m_hasher;
};

}
