/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/FixedArray.h>
#include <AK/MaybeOwned.h>
#include <AK/Stream.h>

extern "C" {
typedef struct z_stream_s z_stream;
}

namespace Compress {

enum class GenericZlibCompressionLevel : u8 {
    Fastest,
    Default,
    Best,
};

class GenericZlibDecompressor : public Stream {
    AK_MAKE_NONCOPYABLE(GenericZlibDecompressor);

public:
    ~GenericZlibDecompressor() override;

    virtual ErrorOr<Bytes> read_some(Bytes) override;
    virtual ErrorOr<size_t> write_some(ReadonlyBytes) override;
    virtual bool is_eof() const override;
    virtual bool is_open() const override;
    virtual void close() override;

protected:
    GenericZlibDecompressor(AK::FixedArray<u8>, MaybeOwned<Stream>, z_stream*);

    static ErrorOr<z_stream*> new_z_stream(int window_bits);

private:
    MaybeOwned<Stream> m_stream;
    z_stream* m_zstream;

    bool m_eof { false };

    AK::FixedArray<u8> m_buffer;
};

class GenericZlibCompressor : public Stream {
    AK_MAKE_NONCOPYABLE(GenericZlibCompressor);

public:
    ~GenericZlibCompressor() override;

    virtual ErrorOr<Bytes> read_some(Bytes) override;
    virtual ErrorOr<size_t> write_some(ReadonlyBytes) override;
    virtual bool is_eof() const override;
    virtual bool is_open() const override;
    virtual void close() override;
    ErrorOr<void> finish();

protected:
    GenericZlibCompressor(AK::FixedArray<u8>, MaybeOwned<Stream>, z_stream*);

    static ErrorOr<z_stream*> new_z_stream(int window_bits, GenericZlibCompressionLevel compression_level);

private:
    MaybeOwned<Stream> m_stream;
    z_stream* m_zstream;

    AK::FixedArray<u8> m_buffer;
};

}
