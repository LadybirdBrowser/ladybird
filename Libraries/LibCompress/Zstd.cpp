/*
 * Copyright (c) 2026, Ladybird contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompress/Helpers.h>
#include <LibCompress/Zstd.h>
#include <zstd.h>

namespace Compress {

ZstdDecompressor::ZstdDecompressor(AK::FixedArray<u8> buffer, MaybeOwned<Stream> stream, ZSTD_DStream* decoder)
    : m_stream(move(stream))
    , m_decoder(decoder)
    , m_buffer(move(buffer))
{
}

ErrorOr<NonnullOwnPtr<ZstdDecompressor>> ZstdDecompressor::create(MaybeOwned<Stream> stream)
{
    auto buffer = TRY(AK::FixedArray<u8>::create(ZSTD_DStreamInSize()));
    auto* decoder = ZSTD_createDStream();
    if (!decoder)
        return Error::from_errno(ENOMEM);
    return adopt_nonnull_own_or_enomem(new (nothrow) ZstdDecompressor(move(buffer), move(stream), decoder));
}

ErrorOr<ByteBuffer> ZstdDecompressor::decompress_all(ReadonlyBytes bytes)
{
    return ::Compress::decompress_all<ZstdDecompressor>(bytes);
}

ZstdDecompressor::~ZstdDecompressor()
{
    ZSTD_freeDStream(m_decoder);
}

ErrorOr<Bytes> ZstdDecompressor::read_some(Bytes bytes)
{
    if (m_eof || bytes.is_empty())
        return bytes.trim(0);

    size_t total_output = 0;

    while (total_output < bytes.size()) {
        if (m_input_offset == m_input_size) {
            auto input = TRY(m_stream->read_some(m_buffer.span()));
            m_input_offset = 0;
            m_input_size = input.size();

            if (m_input_size == 0 && !m_stream->is_eof())
                break;
        }

        ZSTD_inBuffer input {
            .src = m_buffer.data() + m_input_offset,
            .size = m_input_size - m_input_offset,
            .pos = 0,
        };
        ZSTD_outBuffer output {
            .dst = bytes.data() + total_output,
            .size = bytes.size() - total_output,
            .pos = 0,
        };

        auto result = ZSTD_decompressStream(m_decoder, &output, &input);
        if (ZSTD_isError(result))
            return Error::from_string_literal("Zstd data error");

        m_input_offset += input.pos;
        total_output += output.pos;

        if (result == 0 && m_input_offset == m_input_size) {
            m_eof = true;
            break;
        }

        if (output.pos == 0 && input.pos == 0) {
            if (m_stream->is_eof())
                return Error::from_string_literal("Zstd stream ended before end-of-stream marker");
            break;
        }
    }

    return bytes.trim(total_output);
}

ErrorOr<size_t> ZstdDecompressor::write_some(ReadonlyBytes)
{
    return Error::from_errno(EBADF);
}

bool ZstdDecompressor::is_eof() const
{
    return m_eof;
}

bool ZstdDecompressor::is_open() const
{
    return m_stream->is_open();
}

void ZstdDecompressor::close()
{
}

}
