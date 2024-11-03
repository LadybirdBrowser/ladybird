/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Span.h>
#include <LibCrypto/Cipher/Cipher.h>

namespace Crypto::Cipher {

template<typename T>
class Mode {
public:
    virtual ~Mode() = default;

    virtual void encrypt(ReadonlyBytes in, Bytes& out, ReadonlyBytes ivec = {}, Bytes* ivec_out = nullptr) = 0;
    virtual void decrypt(ReadonlyBytes in, Bytes& out, ReadonlyBytes ivec = {}) = 0;

    virtual size_t IV_length() const = 0;

    T const& cipher() const { return m_cipher; }

    static ErrorOr<ByteBuffer> create_aligned_buffer(size_t input_size)
    {
        size_t remainder = (input_size + T::block_size()) % T::block_size();
        if (remainder == 0)
            return ByteBuffer::create_uninitialized(input_size);
        else
            return ByteBuffer::create_uninitialized(input_size + T::block_size() - remainder);
    }

    virtual ByteString class_name() const = 0;

    T& cipher()
    {
        return m_cipher;
    }

protected:
    virtual void prune_padding(Bytes& data)
    {
        auto size = data.size();
        switch (m_cipher.padding_mode()) {
        case PaddingMode::CMS: {
            // rfc5652 Cryptographic Message Syntax (CMS):
            //     the input shall be padded at the trailing end with k-(lth mod k) octets
            //     all having value k-(lth mod k), where lth is the length of the input.
            auto maybe_padding_length = data[size - 1];
            if (maybe_padding_length > T::block_size()) {
                // Invalid padding length (too long)
                return;
            }
            for (auto i = size - maybe_padding_length; i < size; ++i) {
                if (data[i] != maybe_padding_length) {
                    // not padding, part of data
                    return;
                }
            }
            data = data.slice(0, size - maybe_padding_length);
            break;
        }
        case PaddingMode::RFC5246: {
            auto maybe_padding_length = data[size - 1];
            // FIXME: If we want constant-time operations, this loop should not stop
            for (auto i = size - maybe_padding_length - 1; i < size; ++i) {
                if (data[i] != maybe_padding_length) {
                    // note that this is likely invalid padding
                    return;
                }
            }
            data = data.slice(0, size - maybe_padding_length - 1);
            break;
        }
        case PaddingMode::Null: {
            while (data[size - 1] == 0)
                --size;
            data = data.slice(0, size);
            break;
        }
        case PaddingMode::ZeroLength: {
            // No padding
            break;
        }
        default:
            // FIXME: support other padding modes
            VERIFY_NOT_REACHED();
            break;
        }
    }

    // FIXME: Somehow add a reference version of this
    template<typename... Args>
    Mode(Args... args)
        : m_cipher(args...)
    {
    }

private:
    T m_cipher;
};
}
