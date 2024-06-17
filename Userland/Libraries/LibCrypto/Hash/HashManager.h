/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/Variant.h>
#include <LibCrypto/Hash/BLAKE2b.h>
#include <LibCrypto/Hash/HashFunction.h>
#include <LibCrypto/Hash/MD5.h>
#include <LibCrypto/Hash/SHA1.h>
#include <LibCrypto/Hash/SHA2.h>

namespace Crypto::Hash {

enum class HashKind {
    Unknown,
    None,
    BLAKE2b,
    MD5,
    SHA1,
    SHA256,
    SHA384,
    SHA512,
};

struct MultiHashDigestVariant {
    constexpr static size_t Size = 0;

    MultiHashDigestVariant(Empty digest)
        : m_digest(move(digest))
    {
    }

    MultiHashDigestVariant(MD5::DigestType digest)
        : m_digest(move(digest))
    {
    }

    MultiHashDigestVariant(SHA1::DigestType digest)
        : m_digest(move(digest))
    {
    }

    MultiHashDigestVariant(SHA256::DigestType digest)
        : m_digest(move(digest))
    {
    }

    MultiHashDigestVariant(SHA384::DigestType digest)
        : m_digest(move(digest))
    {
    }

    MultiHashDigestVariant(SHA512::DigestType digest)
        : m_digest(move(digest))
    {
    }

    [[nodiscard]] u8 const* immutable_data() const
    {
        return m_digest.visit(
            [&](Empty const&) -> u8 const* { VERIFY_NOT_REACHED(); },
            [&](auto const& value) { return value.immutable_data(); });
    }

    [[nodiscard]] size_t data_length() const
    {
        return m_digest.visit(
            [&](Empty const&) -> size_t { VERIFY_NOT_REACHED(); },
            [&](auto const& value) { return value.data_length(); });
    }

    [[nodiscard]] ReadonlyBytes bytes() const
    {
        return m_digest.visit(
            [&](Empty const&) -> ReadonlyBytes { VERIFY_NOT_REACHED(); },
            [&](auto const& value) { return value.bytes(); });
    }

    using DigestVariant = Variant<Empty, MD5::DigestType, SHA1::DigestType, SHA256::DigestType, SHA384::DigestType, SHA512::DigestType>;
    DigestVariant m_digest {};
};

class Manager final : public HashFunction<0, 0, MultiHashDigestVariant> {
public:
    using HashFunction::update;

    Manager()
    {
        m_pre_init_buffer = ByteBuffer();
    }

    Manager(Manager const& other) // NOT a copy constructor!
    {
        m_pre_init_buffer = ByteBuffer(); // will not be used
        initialize(other.m_kind);
    }

    Manager(HashKind kind)
    {
        m_pre_init_buffer = ByteBuffer();
        initialize(kind);
    }

    ~Manager()
    {
        m_algorithm = Empty {};
    }

    inline size_t digest_size() const
    {
        return m_algorithm.visit(
            [&](Empty const&) -> size_t { return 0; },
            [&](auto const& hash) { return hash.digest_size(); });
    }

    inline size_t block_size() const
    {
        return m_algorithm.visit(
            [&](Empty const&) -> size_t { return 0; },
            [&](auto const& hash) { return hash.block_size(); });
    }

    inline void initialize(HashKind kind)
    {
        if (!m_algorithm.has<Empty>()) {
            VERIFY_NOT_REACHED();
        }

        m_kind = kind;
        switch (kind) {
        case HashKind::BLAKE2b:
            m_algorithm = BLAKE2b();
            break;
        case HashKind::MD5:
            m_algorithm = MD5();
            break;
        case HashKind::SHA1:
            m_algorithm = SHA1();
            break;
        case HashKind::SHA256:
            m_algorithm = SHA256();
            break;
        case HashKind::SHA384:
            m_algorithm = SHA384();
            break;
        case HashKind::SHA512:
            m_algorithm = SHA512();
            break;
        default:
        case HashKind::None:
            m_algorithm = Empty {};
            break;
        }
    }

    virtual void update(u8 const* data, size_t length) override
    {
        auto size = m_pre_init_buffer.size();
        if (size) {
            m_algorithm.visit(
                [&](Empty&) {},
                [&](auto& hash) { hash.update(m_pre_init_buffer); });
        }
        m_algorithm.visit(
            [&](Empty&) { m_pre_init_buffer.append(data, length); },
            [&](auto& hash) { hash.update(data, length); });
        if (size && m_kind != HashKind::None)
            m_pre_init_buffer.clear();
    }

    virtual DigestType peek() override
    {
        return m_algorithm.visit(
            [&](Empty&) -> DigestType { VERIFY_NOT_REACHED(); },
            [&](auto& hash) -> DigestType { return hash.peek(); });
    }

    virtual DigestType digest() override
    {
        auto digest = peek();
        reset();
        return digest;
    }

    virtual void reset() override
    {
        m_pre_init_buffer.clear();
        m_algorithm.visit(
            [&](Empty&) {},
            [&](auto& hash) { hash.reset(); });
    }

    virtual ByteString class_name() const override
    {
        return m_algorithm.visit(
            [&](Empty const&) -> ByteString { return "UninitializedHashManager"; },
            [&](auto const& hash) { return hash.class_name(); });
    }

    inline HashKind kind() const
    {
        return m_kind;
    }

    inline bool is(HashKind kind) const
    {
        return m_kind == kind;
    }

    inline Manager copy() const
    {
        Manager result;
        result.m_algorithm = m_algorithm;
        result.m_kind = m_kind;
        result.m_pre_init_buffer = m_pre_init_buffer;
        return result;
    }

private:
    using AlgorithmVariant = Variant<Empty, BLAKE2b, MD5, SHA1, SHA256, SHA384, SHA512>;
    AlgorithmVariant m_algorithm {};
    HashKind m_kind { HashKind::None };
    ByteBuffer m_pre_init_buffer;
};

}
