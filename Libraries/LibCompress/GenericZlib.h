/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/FixedArray.h>
#include <AK/MaybeOwned.h>
#include <AK/MemoryStream.h>
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

template<class T>
ErrorOr<ByteBuffer> decompress_all(ReadonlyBytes bytes)
{
    auto input_stream = make<AK::FixedMemoryStream>(bytes);
    auto deflate_stream = TRY(T::create(MaybeOwned<Stream>(move(input_stream))));
    return TRY(deflate_stream->read_until_eof(4096));
}

template<class T>
ErrorOr<ByteBuffer> compress_all(ReadonlyBytes bytes, GenericZlibCompressionLevel compression_level)
{
    auto output_stream = TRY(try_make<AllocatingMemoryStream>());
    auto gzip_stream = TRY(T::create(MaybeOwned { *output_stream }, compression_level));

    TRY(gzip_stream->write_until_depleted(bytes));
    TRY(gzip_stream->finish());

    auto buffer = TRY(ByteBuffer::create_uninitialized(output_stream->used_buffer_size()));
    TRY(output_stream->read_until_filled(buffer.bytes()));

    return buffer;
}

}
