/*
 * Copyright (c) 2020-2022, the SerenityOS developers.
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <AK/Stream.h>
#include <LibCompress/Deflate.h>
#include <LibCrypto/Checksum/CRC32.h>

namespace Compress {

constexpr u8 gzip_magic_1 = 0x1f;
constexpr u8 gzip_magic_2 = 0x8b;
struct [[gnu::packed]] BlockHeader {
    u8 identification_1;
    u8 identification_2;
    u8 compression_method;
    u8 flags;
    LittleEndian<u32> modification_time;
    u8 extra_flags;
    u8 operating_system;

    bool valid_magic_number() const;
    bool supported_by_implementation() const;
};

struct Flags {
    static constexpr u8 FTEXT = 1 << 0;
    static constexpr u8 FHCRC = 1 << 1;
    static constexpr u8 FEXTRA = 1 << 2;
    static constexpr u8 FNAME = 1 << 3;
    static constexpr u8 FCOMMENT = 1 << 4;

    static constexpr u8 MAX = FTEXT | FHCRC | FEXTRA | FNAME | FCOMMENT;
};

class GzipDecompressor final : public Stream {
public:
    GzipDecompressor(MaybeOwned<Stream>);
    ~GzipDecompressor();

    virtual ErrorOr<Bytes> read_some(Bytes) override;
    virtual ErrorOr<size_t> write_some(ReadonlyBytes) override;
    virtual bool is_eof() const override;
    virtual bool is_open() const override { return true; }
    virtual void close() override {};

    static ErrorOr<ByteBuffer> decompress_all(ReadonlyBytes);

    static ErrorOr<Optional<String>> describe_header(ReadonlyBytes);
    static bool is_likely_compressed(ReadonlyBytes bytes);

private:
    class Member {
    public:
        static ErrorOr<NonnullOwnPtr<Member>> construct(BlockHeader header, LittleEndianInputBitStream&);

        BlockHeader m_header;
        NonnullOwnPtr<DeflateDecompressor> m_stream;
        Crypto::Checksum::CRC32 m_checksum;
        size_t m_nread { 0 };

    private:
        Member(BlockHeader, NonnullOwnPtr<DeflateDecompressor>);
    };

    Member const& current_member() const { return *m_current_member; }
    Member& current_member() { return *m_current_member; }

    NonnullOwnPtr<LittleEndianInputBitStream> m_input_stream;
    u8 m_partial_header[sizeof(BlockHeader)];
    size_t m_partial_header_offset { 0 };
    OwnPtr<Member> m_current_member {};

    bool m_eof { false };
};

class GzipCompressor final : public Stream {
public:
    GzipCompressor(MaybeOwned<Stream>);

    virtual ErrorOr<Bytes> read_some(Bytes) override;
    virtual ErrorOr<size_t> write_some(ReadonlyBytes) override;
    virtual bool is_eof() const override;
    virtual bool is_open() const override;
    virtual void close() override;

    static ErrorOr<ByteBuffer> compress_all(ReadonlyBytes bytes);

private:
    MaybeOwned<Stream> m_output_stream;
};

}
