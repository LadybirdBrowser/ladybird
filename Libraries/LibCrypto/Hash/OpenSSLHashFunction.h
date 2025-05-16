/*
 * Copyright (c) 2024, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Types.h>
#include <LibCrypto/Hash/HashFunction.h>
#include <LibCrypto/OpenSSLForward.h>

namespace Crypto::Hash {

template<typename T, size_t BlockS, size_t DigestS, typename DigestT = Digest<DigestS>>
class OpenSSLHashFunction : public HashFunction<BlockS, DigestS, DigestT> {
    AK_MAKE_NONCOPYABLE(OpenSSLHashFunction);

public:
    using HashFunction<BlockS, DigestS, DigestT>::update;

    static NonnullOwnPtr<T> create()
    {
        auto* context = EVP_MD_CTX_new();
        return make<T>(context);
    }

    OpenSSLHashFunction(EVP_MD const* type, EVP_MD_CTX* context)
        : m_type(type)
        , m_context(context)
    {
        OpenSSLHashFunction::reset();
    }

    virtual ~OpenSSLHashFunction() override
    {
        EVP_MD_CTX_free(m_context);
    }

    virtual ByteString class_name() const override = 0;

    void update(u8 const* input, size_t length) override
    {
        if (EVP_DigestUpdate(m_context, input, length) != 1) {
            VERIFY_NOT_REACHED();
        }
    }

    DigestT digest() override
    {
        DigestT digest;
        if (EVP_DigestFinal_ex(m_context, digest.data, nullptr) != 1) {
            VERIFY_NOT_REACHED();
        }

        reset();
        return digest;
    }

    DigestT peek() override
    {
        auto c = copy();
        return c->digest();
    }

    void reset() override
    {
        if (EVP_DigestInit_ex(m_context, m_type, nullptr) != 1) {
            VERIFY_NOT_REACHED();
        }
    }

    NonnullOwnPtr<T> copy() const
    {
        auto context = create();
        if (EVP_MD_CTX_copy_ex(context->m_context, m_context) != 1) {
            VERIFY_NOT_REACHED();
        }

        return context;
    }

    static DigestT hash(u8 const* data, size_t length)
    {
        auto hasher = create();
        hasher->update(data, length);
        return hasher->digest();
    }

    static DigestT hash(ByteBuffer const& buffer) { return hash(buffer.data(), buffer.size()); }
    static DigestT hash(StringView buffer) { return hash(reinterpret_cast<u8 const*>(buffer.characters_without_null_termination()), buffer.length()); }

private:
    EVP_MD const* m_type;
    EVP_MD_CTX* m_context;
};

}
