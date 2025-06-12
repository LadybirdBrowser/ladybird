/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompress/GenericZlib.h>

#include <zlib.h>

namespace Compress {

static Error handle_zlib_error(int ret)
{
    switch (ret) {
    case Z_ERRNO:
        return Error::from_errno(errno);
    case Z_DATA_ERROR:
        // Z_DATA_ERROR if the input data was corrupted
        return Error::from_string_literal("zlib data error");
    case Z_STREAM_ERROR:
        // Z_STREAM_ERROR if the parameters are invalid, such as a null pointer to the structure
        return Error::from_string_literal("zlib stream error");
    case Z_VERSION_ERROR:
        // Z_VERSION_ERROR if the zlib library version is incompatible with the version assumed by the caller
        return Error::from_string_literal("zlib version mismatch");
    case Z_MEM_ERROR:
        // Z_MEM_ERROR if there was not enough memory
        return Error::from_errno(ENOMEM);
    default:
        VERIFY_NOT_REACHED();
    }
}

GenericZlibDecompressor::GenericZlibDecompressor(AK::FixedArray<u8> buffer, MaybeOwned<Stream> stream, z_stream* zstream)
    : m_stream(move(stream))
    , m_zstream(zstream)
    , m_buffer(move(buffer))
{
}

ErrorOr<z_stream*> GenericZlibDecompressor::new_z_stream(int window_bits)
{
    auto zstream = new (nothrow) z_stream {};
    if (!zstream)
        return Error::from_errno(ENOMEM);

    // The fields next_in, avail_in, zalloc, zfree and opaque must be initialized before by the caller.
    zstream->next_in = nullptr;
    zstream->avail_in = 0;
    zstream->zalloc = nullptr;
    zstream->zfree = nullptr;
    zstream->opaque = nullptr;

    if (auto ret = inflateInit2(zstream, window_bits); ret != Z_OK)
        return handle_zlib_error(ret);

    return zstream;
}

GenericZlibDecompressor::~GenericZlibDecompressor()
{
    inflateEnd(m_zstream);
    delete m_zstream;
}

ErrorOr<Bytes> GenericZlibDecompressor::read_some(Bytes bytes)
{
    m_zstream->avail_out = bytes.size();
    m_zstream->next_out = bytes.data();

    if (m_zstream->avail_in == 0) {
        auto in = TRY(m_stream->read_some(m_buffer.span()));
        m_zstream->avail_in = in.size();
        m_zstream->next_in = m_buffer.data();
    }

    auto ret = inflate(m_zstream, Z_NO_FLUSH);
    if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR)
        return handle_zlib_error(ret);

    if (ret == Z_STREAM_END) {
        inflateReset(m_zstream);
        if (m_zstream->avail_in == 0)
            m_eof = true;
    }

    return bytes.slice(0, bytes.size() - m_zstream->avail_out);
}

ErrorOr<size_t> GenericZlibDecompressor::write_some(ReadonlyBytes)
{
    return Error::from_errno(EBADF);
}

bool GenericZlibDecompressor::is_eof() const
{
    return m_eof;
}

bool GenericZlibDecompressor::is_open() const
{
    return m_stream->is_open();
}

void GenericZlibDecompressor::close()
{
}

GenericZlibCompressor::GenericZlibCompressor(AK::FixedArray<u8> buffer, MaybeOwned<Stream> stream, z_stream* zstream)
    : m_stream(move(stream))
    , m_zstream(zstream)
    , m_buffer(move(buffer))
{
}

ErrorOr<z_stream*> GenericZlibCompressor::new_z_stream(int window_bits, GenericZlibCompressionLevel compression_level)
{
    auto zstream = new (nothrow) z_stream {};
    if (!zstream)
        return Error::from_errno(ENOMEM);

    // The fields zalloc, zfree and opaque must be initialized before by the caller.
    zstream->zalloc = nullptr;
    zstream->zfree = nullptr;
    zstream->opaque = nullptr;

    int level = [&] {
        switch (compression_level) {
        case GenericZlibCompressionLevel::Fastest:
            return Z_BEST_SPEED;
        case GenericZlibCompressionLevel::Default:
            return Z_DEFAULT_COMPRESSION;
        case GenericZlibCompressionLevel::Best:
            return Z_BEST_COMPRESSION;
        default:
            VERIFY_NOT_REACHED();
        }
    }();

    if (auto ret = deflateInit2(zstream, level, Z_DEFLATED, window_bits, MAX_MEM_LEVEL, Z_DEFAULT_STRATEGY); ret != Z_OK)
        return handle_zlib_error(ret);

    return zstream;
}

GenericZlibCompressor::~GenericZlibCompressor()
{
    deflateEnd(m_zstream);
    delete m_zstream;
}

ErrorOr<Bytes> GenericZlibCompressor::read_some(Bytes)
{
    return Error::from_errno(EBADF);
}

ErrorOr<size_t> GenericZlibCompressor::write_some(ReadonlyBytes bytes)
{
    m_zstream->avail_in = bytes.size();
    m_zstream->next_in = const_cast<u8*>(bytes.data());

    // If deflate returns with avail_out == 0, this function must be called again with the same value of the flush parameter
    // and more output space (updated avail_out), until the flush is complete (deflate returns with non-zero avail_out).
    do {
        m_zstream->avail_out = m_buffer.size();
        m_zstream->next_out = m_buffer.data();

        auto ret = deflate(m_zstream, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_BUF_ERROR)
            return handle_zlib_error(ret);

        auto have = m_buffer.size() - m_zstream->avail_out;
        TRY(m_stream->write_until_depleted(m_buffer.span().slice(0, have)));
    } while (m_zstream->avail_out == 0);

    VERIFY(m_zstream->avail_in == 0);
    return bytes.size();
}

bool GenericZlibCompressor::is_eof() const
{
    return false;
}

bool GenericZlibCompressor::is_open() const
{
    return m_stream->is_open();
}

void GenericZlibCompressor::close()
{
}

ErrorOr<void> GenericZlibCompressor::finish()
{
    VERIFY(m_zstream->avail_in == 0);

    // If the parameter flush is set to Z_FINISH, pending input is processed, pending output is flushed and deflate returns with Z_STREAM_END
    // if there was enough output space. If deflate returns with Z_OK or Z_BUF_ERROR, this function must be called again with Z_FINISH
    // and more output space (updated avail_out) but no more input data, until it returns with Z_STREAM_END or an error.
    while (true) {
        m_zstream->avail_out = m_buffer.size();
        m_zstream->next_out = m_buffer.data();

        auto ret = deflate(m_zstream, Z_FINISH);
        if (ret == Z_STREAM_END || ret == Z_BUF_ERROR || ret == Z_OK) {
            auto have = m_buffer.size() - m_zstream->avail_out;
            TRY(m_stream->write_until_depleted(m_buffer.span().slice(0, have)));

            if (ret == Z_STREAM_END)
                return {};
        } else {
            return handle_zlib_error(ret);
        }
    }
}

}
