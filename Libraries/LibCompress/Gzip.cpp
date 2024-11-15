/*
 * Copyright (c) 2020-2022, the SerenityOS developers.
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompress/Gzip.h>

#include <AK/BitStream.h>
#include <AK/MemoryStream.h>
#include <AK/String.h>
#include <LibCore/DateTime.h>
#include <LibCore/File.h>
#include <LibCore/MappedFile.h>
#include <LibCore/System.h>

namespace Compress {

bool GzipDecompressor::is_likely_compressed(ReadonlyBytes bytes)
{
    return bytes.size() >= 2 && bytes[0] == gzip_magic_1 && bytes[1] == gzip_magic_2;
}

bool BlockHeader::valid_magic_number() const
{
    return identification_1 == gzip_magic_1 && identification_2 == gzip_magic_2;
}

bool BlockHeader::supported_by_implementation() const
{
    if (compression_method != 0x08) {
        // RFC 1952 does not define any compression methods other than deflate.
        return false;
    }

    if (flags > Flags::MAX) {
        // RFC 1952 does not define any more flags.
        return false;
    }

    return true;
}

ErrorOr<NonnullOwnPtr<GzipDecompressor::Member>> GzipDecompressor::Member::construct(BlockHeader header, LittleEndianInputBitStream& stream)
{
    auto deflate_stream = TRY(DeflateDecompressor::construct(MaybeOwned<LittleEndianInputBitStream>(stream)));
    return TRY(adopt_nonnull_own_or_enomem(new (nothrow) Member(header, move(deflate_stream))));
}

GzipDecompressor::Member::Member(BlockHeader header, NonnullOwnPtr<DeflateDecompressor> stream)
    : m_header(header)
    , m_stream(move(stream))
{
}

GzipDecompressor::GzipDecompressor(MaybeOwned<Stream> stream)
    : m_input_stream(make<LittleEndianInputBitStream>(move(stream)))
{
}

GzipDecompressor::~GzipDecompressor()
{
    m_current_member.clear();
}

ErrorOr<Bytes> GzipDecompressor::read_some(Bytes bytes)
{
    size_t total_read = 0;
    while (total_read < bytes.size()) {
        if (is_eof())
            break;

        auto slice = bytes.slice(total_read);

        if (m_current_member) {
            auto current_slice = TRY(current_member().m_stream->read_some(slice));
            current_member().m_checksum.update(current_slice);
            current_member().m_nread += current_slice.size();

            if (current_slice.size() < slice.size()) {
                u32 crc32 = TRY(m_input_stream->read_value<LittleEndian<u32>>());
                u32 input_size = TRY(m_input_stream->read_value<LittleEndian<u32>>());

                if (crc32 != current_member().m_checksum.digest())
                    return Error::from_string_literal("Stored CRC32 does not match the calculated CRC32 of the current member");

                if (input_size != current_member().m_nread)
                    return Error::from_string_literal("Input size does not match the number of read bytes");

                m_current_member.clear();

                total_read += current_slice.size();
                continue;
            }

            total_read += current_slice.size();
            continue;
        } else {
            auto current_partial_header_slice = Bytes { m_partial_header, sizeof(BlockHeader) }.slice(m_partial_header_offset);
            auto current_partial_header_data = TRY(m_input_stream->read_some(current_partial_header_slice));
            m_partial_header_offset += current_partial_header_data.size();

            if (is_eof())
                break;

            if (m_partial_header_offset < sizeof(BlockHeader)) {
                break; // partial header read
            }
            m_partial_header_offset = 0;

            BlockHeader header = *(reinterpret_cast<BlockHeader*>(m_partial_header));

            if (!header.valid_magic_number())
                return Error::from_string_literal("Header does not have a valid magic number");

            if (!header.supported_by_implementation())
                return Error::from_string_literal("Header is not supported by implementation");

            if (header.flags & Flags::FEXTRA) {
                u16 subfield_id = TRY(m_input_stream->read_value<LittleEndian<u16>>());
                u16 length = TRY(m_input_stream->read_value<LittleEndian<u16>>());
                TRY(m_input_stream->discard(length));
                (void)subfield_id;
            }

            auto discard_string = [&]() -> ErrorOr<void> {
                char next_char;
                do {
                    next_char = TRY(m_input_stream->read_value<char>());
                } while (next_char);

                return {};
            };

            if (header.flags & Flags::FNAME)
                TRY(discard_string());

            if (header.flags & Flags::FCOMMENT)
                TRY(discard_string());

            if (header.flags & Flags::FHCRC) {
                u16 crc = TRY(m_input_stream->read_value<LittleEndian<u16>>());
                // FIXME: we should probably verify this instead of just assuming it matches
                (void)crc;
            }

            m_current_member = TRY(Member::construct(header, *m_input_stream));
            continue;
        }
    }
    return bytes.slice(0, total_read);
}

ErrorOr<Optional<String>> GzipDecompressor::describe_header(ReadonlyBytes bytes)
{
    if (bytes.size() < sizeof(BlockHeader))
        return OptionalNone {};

    auto& header = *(reinterpret_cast<BlockHeader const*>(bytes.data()));
    if (!header.valid_magic_number() || !header.supported_by_implementation())
        return OptionalNone {};

    LittleEndian<u32> original_size = *reinterpret_cast<u32 const*>(bytes.offset(bytes.size() - sizeof(u32)));
    return TRY(String::formatted("last modified: {}, original size {}", Core::DateTime::from_timestamp(header.modification_time), (u32)original_size));
}

ErrorOr<ByteBuffer> GzipDecompressor::decompress_all(ReadonlyBytes bytes)
{
    auto memory_stream = TRY(try_make<FixedMemoryStream>(bytes));
    auto gzip_stream = make<GzipDecompressor>(move(memory_stream));
    AllocatingMemoryStream output_stream;

    auto buffer = TRY(ByteBuffer::create_uninitialized(4096));
    while (!gzip_stream->is_eof()) {
        auto const data = TRY(gzip_stream->read_some(buffer));
        TRY(output_stream.write_until_depleted(data));
    }

    auto output_buffer = TRY(ByteBuffer::create_uninitialized(output_stream.used_buffer_size()));
    TRY(output_stream.read_until_filled(output_buffer));
    return output_buffer;
}

bool GzipDecompressor::is_eof() const { return m_input_stream->is_eof(); }

ErrorOr<size_t> GzipDecompressor::write_some(ReadonlyBytes)
{
    return Error::from_errno(EBADF);
}

ErrorOr<NonnullOwnPtr<GzipCompressor>> GzipCompressor::create(MaybeOwned<Stream> output_stream)
{
    BlockHeader header;
    header.identification_1 = 0x1f;
    header.identification_2 = 0x8b;
    header.compression_method = 0x08;
    header.flags = 0;
    header.modification_time = 0;
    header.extra_flags = 3;      // DEFLATE sets 2 for maximum compression and 4 for minimum compression
    header.operating_system = 3; // unix
    TRY(output_stream->write_until_depleted({ &header, sizeof(header) }));

    auto deflate_compressor = TRY(DeflateCompressor::construct(MaybeOwned(*output_stream)));
    return adopt_own(*new GzipCompressor { move(output_stream), move(deflate_compressor) });
}

GzipCompressor::GzipCompressor(MaybeOwned<Stream> output_stream, NonnullOwnPtr<DeflateCompressor> deflate_compressor)
    : m_output_stream(move(output_stream))
    , m_deflate_compressor(move(deflate_compressor))
{
}

ErrorOr<Bytes> GzipCompressor::read_some(Bytes)
{
    return Error::from_errno(EBADF);
}

ErrorOr<size_t> GzipCompressor::write_some(ReadonlyBytes bytes)
{
    VERIFY(!m_finished);

    TRY(m_deflate_compressor->write_until_depleted(bytes));
    m_total_bytes += bytes.size();
    m_crc32.update(bytes);

    return bytes.size();
}

ErrorOr<void> GzipCompressor::finish()
{
    VERIFY(!m_finished);
    m_finished = true;

    TRY(m_deflate_compressor->final_flush());
    TRY(m_output_stream->write_value<LittleEndian<u32>>(m_crc32.digest()));
    TRY(m_output_stream->write_value<LittleEndian<u32>>(m_total_bytes));

    return {};
}

bool GzipCompressor::is_eof() const
{
    return true;
}

bool GzipCompressor::is_open() const
{
    return m_output_stream->is_open();
}

void GzipCompressor::close()
{
}

ErrorOr<ByteBuffer> GzipCompressor::compress_all(ReadonlyBytes bytes)
{
    auto output_stream = TRY(try_make<AllocatingMemoryStream>());
    auto gzip_stream = TRY(GzipCompressor::create(MaybeOwned { *output_stream }));

    TRY(gzip_stream->write_until_depleted(bytes));
    TRY(gzip_stream->finish());

    auto buffer = TRY(ByteBuffer::create_uninitialized(output_stream->used_buffer_size()));
    TRY(output_stream->read_until_filled(buffer.bytes()));

    return buffer;
}

}
