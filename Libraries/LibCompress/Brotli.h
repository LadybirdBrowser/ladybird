/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/FixedArray.h>
#include <AK/MaybeOwned.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Stream.h>

extern "C" {
typedef struct BrotliDecoderStateStruct BrotliDecoderState;
typedef struct BrotliEncoderStateStruct BrotliEncoderState;
}

namespace Compress {

enum class BrotliCompressionLevel : u8 {
    Fastest,
    Default,
    Best,
};

class BrotliDecompressor final : public Stream {
    AK_MAKE_NONCOPYABLE(BrotliDecompressor);

public:
    static ErrorOr<NonnullOwnPtr<BrotliDecompressor>> create(MaybeOwned<Stream>);
    static ErrorOr<ByteBuffer> decompress_all(ReadonlyBytes);
    ~BrotliDecompressor() override;

    virtual ErrorOr<Bytes> read_some(Bytes) override;
    virtual ErrorOr<size_t> write_some(ReadonlyBytes) override;
    virtual bool is_eof() const override;
    virtual bool is_open() const override;
    virtual void close() override;

private:
    BrotliDecompressor(AK::FixedArray<u8>, MaybeOwned<Stream>, BrotliDecoderState*);

    AK::FixedArray<u8> m_buffer;
    MaybeOwned<Stream> m_stream;
    BrotliDecoderState* m_decoder { nullptr };
    u8 const* m_next_in { nullptr };
    size_t m_available_in { 0 };
    bool m_eof { false };
};

class BrotliCompressor final : public Stream {
    AK_MAKE_NONCOPYABLE(BrotliCompressor);

public:
    static ErrorOr<NonnullOwnPtr<BrotliCompressor>> create(MaybeOwned<Stream>, BrotliCompressionLevel = BrotliCompressionLevel::Default);
    static ErrorOr<ByteBuffer> compress_all(ReadonlyBytes, BrotliCompressionLevel = BrotliCompressionLevel::Default);
    ~BrotliCompressor() override;

    virtual ErrorOr<Bytes> read_some(Bytes) override;
    virtual ErrorOr<size_t> write_some(ReadonlyBytes) override;
    virtual bool is_eof() const override;
    virtual bool is_open() const override;
    virtual void close() override;
    ErrorOr<void> finish();

private:
    BrotliCompressor(AK::FixedArray<u8>, MaybeOwned<Stream>, BrotliEncoderState*);

    AK::FixedArray<u8> m_buffer;
    MaybeOwned<Stream> m_stream;
    BrotliEncoderState* m_encoder { nullptr };
    bool m_finished { false };
};

}
