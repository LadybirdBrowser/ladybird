/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <AK/Vector.h>

constexpr static auto IPAD = 0x36;
constexpr static auto OPAD = 0x5c;

namespace Crypto::Authentication {

template<typename HashT>
class HMAC {
public:
    using HashType = HashT;
    using TagType = typename HashType::DigestType;

    constexpr size_t digest_size() const { return m_inner_hasher.digest_size(); }

    template<typename KeyBufferType, typename... Args>
    HMAC(KeyBufferType key, Args... args)
        : m_inner_hasher(args...)
        , m_outer_hasher(args...)
    {
        derive_key(key);
        reset();
    }

    TagType process(u8 const* message, size_t length)
    {
        reset();
        update(message, length);
        return digest();
    }

    void update(u8 const* message, size_t length)
    {
        m_inner_hasher.update(message, length);
    }

    TagType process(ReadonlyBytes span) { return process(span.data(), span.size()); }
    TagType process(StringView string) { return process((u8 const*)string.characters_without_null_termination(), string.length()); }

    void update(ReadonlyBytes span) { return update(span.data(), span.size()); }
    void update(StringView string) { return update((u8 const*)string.characters_without_null_termination(), string.length()); }

    TagType digest()
    {
        m_outer_hasher.update(m_inner_hasher.digest().immutable_data(), m_inner_hasher.digest_size());
        auto result = m_outer_hasher.digest();
        reset();
        return result;
    }

    void reset()
    {
        m_inner_hasher.reset();
        m_outer_hasher.reset();
        m_inner_hasher.update(m_key_data, m_inner_hasher.block_size());
        m_outer_hasher.update(m_key_data + m_inner_hasher.block_size(), m_outer_hasher.block_size());
    }

    ByteString class_name() const
    {
        StringBuilder builder;
        builder.append("HMAC-"sv);
        builder.append(m_inner_hasher.class_name());
        return builder.to_byte_string();
    }

private:
    void derive_key(u8 const* key, size_t length)
    {
        auto block_size = m_inner_hasher.block_size();
        // Note: The block size of all the current hash functions is 512 bits.
        Vector<u8, 64> v_key;
        v_key.resize(block_size);
        auto key_buffer = v_key.span();
        // m_key_data is zero'd, so copying the data in
        // the first few bytes leaves the rest zero, which
        // is exactly what we want (zero padding)
        if (length > block_size) {
            m_inner_hasher.update(key, length);
            auto digest = m_inner_hasher.digest();
            // FIXME: should we check if the hash function creates more data than its block size?
            key_buffer.overwrite(0, digest.immutable_data(), m_inner_hasher.digest_size());
        } else if (length > 0) {
            key_buffer.overwrite(0, key, length);
        }

        // fill out the inner and outer padded keys
        auto* i_key = m_key_data;
        auto* o_key = m_key_data + block_size;
        for (size_t i = 0; i < block_size; ++i) {
            auto key_byte = key_buffer[i];
            i_key[i] = key_byte ^ IPAD;
            o_key[i] = key_byte ^ OPAD;
        }
    }

    void derive_key(ReadonlyBytes key) { derive_key(key.data(), key.size()); }
    void derive_key(StringView key) { derive_key(key.bytes()); }

    HashType m_inner_hasher, m_outer_hasher;
    u8 m_key_data[2048];
};

}
