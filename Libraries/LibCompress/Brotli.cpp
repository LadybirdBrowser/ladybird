/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompress/Brotli.h>
#include <LibCompress/GenericZlib.h>

#include <brotli/decode.h>
#include <brotli/encode.h>
#include <string.h>

namespace Compress {

static Error brotli_decoder_error(BrotliDecoderState* state)
{
    auto const* message = BrotliDecoderErrorString(BrotliDecoderGetErrorCode(state));
    return Error::from_string_view({ message, strlen(message) });
}

static u32 quality_from_compression_level(BrotliCompressionLevel compression_level)
{
    switch (compression_level) {
    case BrotliCompressionLevel::Fastest:
        return BROTLI_MIN_QUALITY;
    case BrotliCompressionLevel::Default:
        return BROTLI_DEFAULT_QUALITY;
    case BrotliCompressionLevel::Best:
        return BROTLI_MAX_QUALITY;
    }

    VERIFY_NOT_REACHED();
}

BrotliDecompressor::BrotliDecompressor(AK::FixedArray<u8> buffer, MaybeOwned<Stream> stream, BrotliDecoderState* decoder)
    : m_buffer(move(buffer))
    , m_stream(move(stream))
    , m_decoder(decoder)
{
}

ErrorOr<NonnullOwnPtr<BrotliDecompressor>> BrotliDecompressor::create(MaybeOwned<Stream> stream)
{
    auto buffer = TRY(AK::FixedArray<u8>::create(16 * KiB));
    auto* decoder = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
    if (!decoder)
        return Error::from_errno(ENOMEM);

    return adopt_nonnull_own_or_enomem(new (nothrow) BrotliDecompressor(move(buffer), move(stream), decoder));
}

ErrorOr<ByteBuffer> BrotliDecompressor::decompress_all(ReadonlyBytes bytes)
{
    return ::Compress::decompress_all<BrotliDecompressor>(bytes);
}

BrotliDecompressor::~BrotliDecompressor()
{
    BrotliDecoderDestroyInstance(m_decoder);
}

ErrorOr<Bytes> BrotliDecompressor::read_some(Bytes bytes)
{
    if (bytes.is_empty())
        return bytes.trim(0);

    if (m_eof) {
        if (!m_stream->is_eof())
            return Error::from_string_literal("Brotli stream has trailing data");
        return bytes.trim(0);
    }

    if (m_available_in == 0) {
        auto input = TRY(m_stream->read_some(m_buffer.span()));
        m_available_in = input.size();
        m_next_in = input.data();
    }

    auto* next_out = bytes.data();
    auto available_out = bytes.size();
    auto had_input = m_available_in > 0;

    auto result = BrotliDecoderDecompressStream(m_decoder, &m_available_in, &m_next_in, &available_out, &next_out, nullptr);
    auto produced = bytes.size() - available_out;

    switch (result) {
    case BROTLI_DECODER_RESULT_ERROR:
        return brotli_decoder_error(m_decoder);
    case BROTLI_DECODER_RESULT_SUCCESS:
        if (m_available_in > 0 || !m_stream->is_eof())
            return Error::from_string_literal("Brotli stream has trailing data");

        m_eof = true;
        return bytes.trim(produced);
    case BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT:
        return bytes.trim(produced);
    case BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT:
        if (produced > 0 || had_input)
            return bytes.trim(produced);
        return Error::from_string_literal("Brotli stream ended before the end marker");
    }

    VERIFY_NOT_REACHED();
}

ErrorOr<size_t> BrotliDecompressor::write_some(ReadonlyBytes)
{
    return Error::from_errno(EBADF);
}

bool BrotliDecompressor::is_eof() const
{
    return m_eof;
}

bool BrotliDecompressor::is_open() const
{
    return m_stream->is_open();
}

void BrotliDecompressor::close()
{
}

BrotliCompressor::BrotliCompressor(AK::FixedArray<u8> buffer, MaybeOwned<Stream> stream, BrotliEncoderState* encoder)
    : m_buffer(move(buffer))
    , m_stream(move(stream))
    , m_encoder(encoder)
{
}

ErrorOr<NonnullOwnPtr<BrotliCompressor>> BrotliCompressor::create(MaybeOwned<Stream> stream, BrotliCompressionLevel compression_level)
{
    auto buffer = TRY(AK::FixedArray<u8>::create(16 * KiB));
    auto* encoder = BrotliEncoderCreateInstance(nullptr, nullptr, nullptr);
    if (!encoder)
        return Error::from_errno(ENOMEM);

    if (!BrotliEncoderSetParameter(encoder, BROTLI_PARAM_QUALITY, quality_from_compression_level(compression_level))) {
        BrotliEncoderDestroyInstance(encoder);
        return Error::from_string_literal("Failed to set Brotli compression quality");
    }

    return adopt_nonnull_own_or_enomem(new (nothrow) BrotliCompressor(move(buffer), move(stream), encoder));
}

ErrorOr<ByteBuffer> BrotliCompressor::compress_all(ReadonlyBytes bytes, BrotliCompressionLevel compression_level)
{
    return ::Compress::compress_all<BrotliCompressor>(bytes, compression_level);
}

BrotliCompressor::~BrotliCompressor()
{
    BrotliEncoderDestroyInstance(m_encoder);
}

ErrorOr<Bytes> BrotliCompressor::read_some(Bytes)
{
    return Error::from_errno(EBADF);
}

ErrorOr<size_t> BrotliCompressor::write_some(ReadonlyBytes bytes)
{
    if (m_finished)
        return Error::from_string_literal("Brotli stream is already finished");

    auto available_in = bytes.size();
    auto const* next_in = bytes.data();

    while (true) {
        auto* next_out = m_buffer.data();
        auto available_out = m_buffer.size();

        if (!BrotliEncoderCompressStream(m_encoder, BROTLI_OPERATION_PROCESS, &available_in, &next_in, &available_out, &next_out, nullptr))
            return Error::from_string_literal("Brotli compression failed");

        auto have = m_buffer.size() - available_out;
        TRY(m_stream->write_until_depleted(m_buffer.span().slice(0, have)));

        if (available_in == 0 && !BrotliEncoderHasMoreOutput(m_encoder))
            break;
    }

    return bytes.size();
}

bool BrotliCompressor::is_eof() const
{
    return false;
}

bool BrotliCompressor::is_open() const
{
    return m_stream->is_open();
}

void BrotliCompressor::close()
{
}

ErrorOr<void> BrotliCompressor::finish()
{
    if (m_finished)
        return {};

    size_t available_in = 0;
    u8 const* next_in = nullptr;

    while (!BrotliEncoderIsFinished(m_encoder)) {
        auto* next_out = m_buffer.data();
        auto available_out = m_buffer.size();

        if (!BrotliEncoderCompressStream(m_encoder, BROTLI_OPERATION_FINISH, &available_in, &next_in, &available_out, &next_out, nullptr))
            return Error::from_string_literal("Brotli compression failed");

        auto have = m_buffer.size() - available_out;
        TRY(m_stream->write_until_depleted(m_buffer.span().slice(0, have)));

        if (have == 0 && !BrotliEncoderIsFinished(m_encoder))
            return Error::from_string_literal("No Brotli compression progress");
    }

    m_finished = true;
    return {};
}

}
