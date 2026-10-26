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
    case Z_NEED_DICT:
        // Z_NEED_DICT if the input data needed a dictionary
        return Error::from_string_literal("zlib data needs dictionary");
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

    // We got Z_BUF_ERROR (no progress was possible), no more input, stream is EOF and no output was produced.
    // There is no way to get out of this loop, error out.
    if (ret == Z_BUF_ERROR && m_zstream->avail_in == 0 && m_stream->is_eof() && bytes.size() == m_zstream->avail_out)
        return Error::from_string_literal("No decompression progress on EOF stream");

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

}
