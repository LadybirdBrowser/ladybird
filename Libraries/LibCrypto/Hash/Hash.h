/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Error.h>
#include <AK/Noncopyable.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <LibCrypto/OpenSSLForward.h>

namespace Crypto::Hash {

enum class HashKind {
    Unknown,
    None,
    SHA1,
    SHA256,
    SHA384,
    SHA512,
    SHA3_256,
    SHA3_384,
    SHA3_512
};

ErrorOr<char const*> openssl_digest_name(HashKind);
size_t digest_size(HashKind);

struct Digest {
    ReadonlyBytes bytes() const LIFETIME_BOUND { return data.span().trim(size); }

    Array<u8, 64> data {};
    size_t size { 0 };
};

Digest digest(HashKind, ReadonlyBytes);

class Hasher {
    AK_MAKE_NONCOPYABLE(Hasher);
    AK_MAKE_NONMOVABLE(Hasher);

public:
    explicit Hasher(HashKind);
    ~Hasher();

    void update(ReadonlyBytes);
    void update(StringView string) { update(string.bytes()); }

    Digest finish();

private:
    EVP_MD_CTX* m_context { nullptr };
};

}
