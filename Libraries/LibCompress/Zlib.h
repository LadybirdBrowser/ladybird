/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Endian.h>
#include <AK/MaybeOwned.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/Span.h>
#include <AK/Stream.h>
#include <AK/Types.h>
#include <LibCrypto/Checksum/Adler32.h>

namespace Compress {

enum class ZlibCompressionMethod : u8 {
    Deflate = 8,
};

enum class ZlibCompressionLevel : u8 {
    Fastest,
    Fast,
    Default,
    Best,
};

struct ZlibHeader {
    union {
        struct {
            ZlibCompressionMethod compression_method : 4;
            u8 compression_info : 4;

            u8 check_bits : 5;
            bool present_dictionary : 1;
            ZlibCompressionLevel compression_level : 2;
        };
        NetworkOrdered<u16> as_u16;
    };
};
static_assert(sizeof(ZlibHeader) == sizeof(u16));

class ZlibDecompressor : public Stream {
public:
    static ErrorOr<NonnullOwnPtr<ZlibDecompressor>> create(MaybeOwned<Stream>);

    virtual ErrorOr<Bytes> read_some(Bytes) override;
    virtual ErrorOr<size_t> write_some(ReadonlyBytes) override;
    virtual bool is_eof() const override;
    virtual bool is_open() const override;
    virtual void close() override;

private:
    ZlibDecompressor(ZlibHeader, NonnullOwnPtr<Stream>);

    ZlibHeader m_header;
    NonnullOwnPtr<Stream> m_stream;
};

class ZlibCompressor : public Stream {
public:
    static ErrorOr<NonnullOwnPtr<ZlibCompressor>> construct(MaybeOwned<Stream>, ZlibCompressionLevel = ZlibCompressionLevel::Default);
    ~ZlibCompressor();

    virtual ErrorOr<Bytes> read_some(Bytes) override;
    virtual ErrorOr<size_t> write_some(ReadonlyBytes) override;
    virtual bool is_eof() const override;
    virtual bool is_open() const override;
    virtual void close() override;
    ErrorOr<void> finish();

    static ErrorOr<ByteBuffer> compress_all(ReadonlyBytes bytes, ZlibCompressionLevel = ZlibCompressionLevel::Default);

private:
    ZlibCompressor(MaybeOwned<Stream> stream, NonnullOwnPtr<Stream> compressor_stream);
    ErrorOr<void> write_header(ZlibCompressionMethod, ZlibCompressionLevel);

    bool m_finished { false };
    MaybeOwned<Stream> m_output_stream;
    NonnullOwnPtr<Stream> m_compressor;
    Crypto::Checksum::Adler32 m_adler32_checksum;
};

}

template<>
struct AK::Traits<Compress::ZlibHeader> : public AK::DefaultTraits<Compress::ZlibHeader> {
    static constexpr bool is_trivially_serializable() { return true; }
};
