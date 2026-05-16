/*
 * Copyright (c) 2026, Ladybird contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/FixedArray.h>
#include <AK/MaybeOwned.h>
#include <AK/Stream.h>

extern "C" {
typedef struct ZSTD_DCtx_s ZSTD_DStream;
}

namespace Compress {

class ZstdDecompressor final : public Stream {
    AK_MAKE_NONCOPYABLE(ZstdDecompressor);

public:
    static ErrorOr<NonnullOwnPtr<ZstdDecompressor>> create(MaybeOwned<Stream>);
    static ErrorOr<ByteBuffer> decompress_all(ReadonlyBytes);

    ~ZstdDecompressor() override;

    virtual ErrorOr<Bytes> read_some(Bytes) override;
    virtual ErrorOr<size_t> write_some(ReadonlyBytes) override;
    virtual bool is_eof() const override;
    virtual bool is_open() const override;
    virtual void close() override;

private:
    ZstdDecompressor(AK::FixedArray<u8>, MaybeOwned<Stream>, ZSTD_DStream*);

    MaybeOwned<Stream> m_stream;
    ZSTD_DStream* m_decoder { nullptr };
    AK::FixedArray<u8> m_buffer;
    size_t m_input_offset { 0 };
    size_t m_input_size { 0 };
    bool m_eof { false };
};

}
