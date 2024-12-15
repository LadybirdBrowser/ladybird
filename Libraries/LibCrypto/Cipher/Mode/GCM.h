/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Memory.h>
#include <AK/OwnPtr.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <LibCrypto/Authentication/GHash.h>
#include <LibCrypto/Cipher/Mode/CTR.h>
#include <LibCrypto/Verification.h>

namespace Crypto::Cipher {

using IncrementFunction = IncrementInplace;

template<typename T>
class GCM : public CTR<T, IncrementFunction> {
public:
    constexpr static size_t IVSizeInBits = 128;

    virtual ~GCM() = default;

    template<typename... Args>
    explicit constexpr GCM(Args... args)
        : CTR<T>(args...)
    {
        static_assert(T::BlockSizeInBits == 128u, "GCM Mode is only available for 128-bit Ciphers");

        __builtin_memset(m_auth_key_storage, 0, block_size);
        typename T::BlockType key_block(m_auth_key_storage, block_size);
        this->cipher().encrypt_block(key_block, key_block);
        key_block.bytes().copy_to(m_auth_key);

        m_ghash = Authentication::GHash(m_auth_key);
    }

    virtual ByteString class_name() const override
    {
        StringBuilder builder;
        builder.append(this->cipher().class_name());
        builder.append("_GCM"sv);
        return builder.to_byte_string();
    }

    virtual size_t IV_length() const override
    {
        return IVSizeInBits / 8;
    }

    // FIXME: This overload throws away the auth stuff, think up a better way to return more than a single bytebuffer.
    virtual void encrypt(ReadonlyBytes in, Bytes& out, ReadonlyBytes ivec = {}, Bytes* = nullptr) override
    {
        VERIFY(!ivec.is_empty());

        static ByteBuffer dummy;

        encrypt(in, out, ivec, dummy, dummy);
    }
    virtual void decrypt(ReadonlyBytes in, Bytes& out, ReadonlyBytes ivec = {}) override
    {
        encrypt(in, out, ivec);
    }

    ByteBuffer process_iv(ReadonlyBytes iv_in)
    {
        if (iv_in.size() == 12) {
            auto buf = MUST(ByteBuffer::create_zeroed(16));
            buf.overwrite(0, iv_in.data(), iv_in.size());

            // Increment the IV for block 0
            auto iv = buf.bytes();
            CTR<T>::increment(iv);

            return buf;
        }

        // https://nvlpubs.nist.gov/nistpubs/Legacy/SP/nistspecialpublication800-38d.pdf
        // Otherwise, the IV is padded with the minimum number of '0' bits, possibly none,
        // so that the length of the resulting string is a multiple of 128 bits (the block size);
        // this string in turn is appended with 64 additional '0' bits, followed by
        // the 64-bit representation of the length of the IV, and the GHASH function
        // is applied to the resulting string to form the precounter block.
        auto iv_pad = iv_in.size() % 16 == 0 ? 0 : 16 - (iv_in.size() % 16);
        auto data = MUST(ByteBuffer::create_zeroed(iv_in.size() + iv_pad + 8 + 8));
        data.overwrite(0, iv_in.data(), iv_in.size());
        ByteReader::store(data.data() + iv_in.size() + iv_pad + 8, AK::convert_between_host_and_big_endian<u64>(iv_in.size() * 8));

        u32 out[4] { 0, 0, 0, 0 };
        m_ghash->process_one(out, data);

        auto buf = MUST(ByteBuffer::create_uninitialized(16));
        for (size_t i = 0; i < 4; ++i)
            ByteReader::store(buf.data() + (i * 4), AK::convert_between_host_and_big_endian(out[i]));
        return buf;
    }

    void encrypt(ReadonlyBytes in, Bytes out, ReadonlyBytes iv_in, ReadonlyBytes aad, Bytes tag)
    {
        auto iv_buf = process_iv(iv_in);
        auto iv = iv_buf.bytes();

        typename T::BlockType block0;
        block0.overwrite(iv);
        this->cipher().encrypt_block(block0, block0);

        // Skip past block 0
        CTR<T>::increment(iv);

        if (in.is_empty())
            CTR<T>::key_stream(out, iv);
        else
            CTR<T>::encrypt(in, out, iv);

        auto auth_tag = m_ghash->process(aad, out);
        block0.apply_initialization_vector({ auth_tag.data, array_size(auth_tag.data) });
        (void)block0.bytes().copy_trimmed_to(tag);
    }

    VerificationConsistency decrypt(ReadonlyBytes in, Bytes out, ReadonlyBytes iv_in, ReadonlyBytes aad, ReadonlyBytes tag)
    {
        auto iv_buf = process_iv(iv_in);
        auto iv = iv_buf.bytes();

        typename T::BlockType block0;
        block0.overwrite(iv);
        this->cipher().encrypt_block(block0, block0);

        // Skip past block 0
        CTR<T>::increment(iv);

        auto auth_tag = m_ghash->process(aad, in);
        block0.apply_initialization_vector({ auth_tag.data, array_size(auth_tag.data) });

        auto test_consistency = [&] {
            VERIFY(block0.block_size() >= tag.size());
            if (block0.block_size() < tag.size() || !timing_safe_compare(block0.bytes().data(), tag.data(), tag.size()))
                return VerificationConsistency::Inconsistent;

            return VerificationConsistency::Consistent;
        };

        if (in.is_empty()) {
            out = {};
            return test_consistency();
        }

        CTR<T>::encrypt(in, out, iv);
        return test_consistency();
    }

private:
    static constexpr auto block_size = T::BlockType::BlockSizeInBits / 8;
    u8 m_auth_key_storage[block_size];
    Bytes m_auth_key { m_auth_key_storage, block_size };
    Optional<Authentication::GHash> m_ghash;
};

}
