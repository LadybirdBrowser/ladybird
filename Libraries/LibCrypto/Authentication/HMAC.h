/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <LibCrypto/Hash/HashManager.h>
#include <LibCrypto/OpenSSL.h>
#include <LibCrypto/OpenSSLForward.h>

namespace Crypto::Authentication {

class HMAC {
public:
    explicit HMAC(Hash::HashKind hash, ReadonlyBytes key);
    ~HMAC();

    size_t digest_size() const;

    void update(u8 const* message, size_t length);
    void update(ReadonlyBytes span) { return update(span.data(), span.size()); }
    void update(StringView string) { return update((u8 const*)string.characters_without_null_termination(), string.length()); }

    ByteBuffer process(u8 const* message, size_t length)
    {
        reset();
        update(message, length);
        return digest();
    }
    ByteBuffer process(ReadonlyBytes span) { return process(span.data(), span.size()); }
    ByteBuffer process(StringView string) { return process((u8 const*)string.characters_without_null_termination(), string.length()); }

    ByteBuffer digest();

    void reset();

    ByteString class_name() const
    {
        auto hash_name = MUST(hash_kind_to_openssl_digest_name(m_hash_kind));

        StringBuilder builder;
        builder.append("HMAC-"sv);
        builder.append(hash_name);
        return builder.to_byte_string();
    }

private:
    Hash::HashKind m_hash_kind;
    ReadonlyBytes m_key;
    EVP_MAC* m_mac { nullptr };
    EVP_MAC_CTX* m_ctx { nullptr };
};

}
