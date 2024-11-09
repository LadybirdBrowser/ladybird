/*
 * Copyright (c) 2023, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Concepts.h>
#include <AK/MaybeOwned.h>
#include <AK/Stream.h>
#include <LibCrypto/Checksum/ChecksumFunction.h>

namespace Crypto::Checksum {

// A stream wrapper type which passes all read and written data through a checksum function.
template<typename ChecksumFunctionType, typename ChecksumType = typename ChecksumFunctionType::ChecksumType>
requires(
    IsBaseOf<ChecksumFunction<ChecksumType>, ChecksumFunctionType>,
    // Require checksum function to be constructible without arguments, since we have no initial data.
    requires() {
        ChecksumFunctionType {};
    })
class ChecksummingStream : public Stream {
public:
    virtual ~ChecksummingStream() = default;

    ChecksummingStream(MaybeOwned<Stream> stream)
        : m_stream(move(stream))
    {
    }

    virtual ErrorOr<Bytes> read_some(Bytes bytes) override
    {
        auto const written_bytes = TRY(m_stream->read_some(bytes));
        update(written_bytes);
        return written_bytes;
    }

    virtual ErrorOr<void> read_until_filled(Bytes bytes) override
    {
        TRY(m_stream->read_until_filled(bytes));
        update(bytes);
        return {};
    }

    virtual ErrorOr<size_t> write_some(ReadonlyBytes bytes) override
    {
        auto bytes_written = TRY(m_stream->write_some(bytes));
        // Only update with the bytes that were actually written
        update(bytes.trim(bytes_written));
        return bytes_written;
    }

    virtual ErrorOr<void> write_until_depleted(ReadonlyBytes bytes) override
    {
        update(bytes);
        return m_stream->write_until_depleted(bytes);
    }

    virtual bool is_eof() const override { return m_stream->is_eof(); }
    virtual bool is_open() const override { return m_stream->is_open(); }
    virtual void close() override { m_stream->close(); }

    ChecksumType digest()
    {
        return m_checksum.digest();
    }

private:
    ALWAYS_INLINE void update(ReadonlyBytes bytes)
    {
        m_checksum.update(bytes);
    }

    MaybeOwned<Stream> m_stream;
    ChecksumFunctionType m_checksum {};
};

}
