/*
 * Copyright (c) 2023, Tim Schumacher <timschumi@gmx.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ConstrainedStream.h>
#include <AK/Endian.h>
#include <LibCompress/Lzma2.h>

namespace Compress {

ErrorOr<NonnullOwnPtr<Lzma2Decompressor>> Lzma2Decompressor::create_from_raw_stream(MaybeOwned<Stream> stream, u32 dictionary_size)
{
    auto dictionary = TRY(CircularBuffer::create_empty(dictionary_size));
    auto decompressor = TRY(adopt_nonnull_own_or_enomem(new (nothrow) Lzma2Decompressor(move(stream), move(dictionary))));
    return decompressor;
}

Lzma2Decompressor::Lzma2Decompressor(MaybeOwned<Stream> stream, CircularBuffer dictionary)
    : m_stream(move(stream))
    , m_dictionary(move(dictionary))
{
}

ErrorOr<Bytes> Lzma2Decompressor::read_some(Bytes bytes)
{
    if (!m_current_chunk_stream.has_value() || (*m_current_chunk_stream)->is_eof()) {
        // "LZMA2 data consists of packets starting with a control byte, with the following values:"
        auto const control_byte = TRY(m_stream->read_value<u8>());

        if (control_byte == 0) {
            // " - 0 denotes the end of the file"
            m_found_end_of_stream = true;
            return bytes.trim(0);
        }

        if (control_byte == 1) {
            // " - 1 denotes a dictionary reset followed by an uncompressed chunk"
            m_dictionary.clear();
            m_dictionary_initialized = true;

            // The XZ utils test files (bad-1-lzma2-8.xz) check that the decompressor
            // requires a new set of properties after a dictionary reset.
            m_last_lzma_options = {};
        }

        if (control_byte == 1 || control_byte == 2) {
            // " - 2 denotes an uncompressed chunk without a dictionary reset"

            if (!m_dictionary_initialized)
                return Error::from_string_literal("LZMA2 stream uses dictionary without ever resetting it");

            // "Uncompressed chunks consist of:
            //   - A 16-bit big-endian value encoding the data size minus one
            //   - The data to be copied verbatim into the dictionary and the output"
            u32 data_size = TRY(m_stream->read_value<BigEndian<u16>>()) + 1;

            m_in_uncompressed_chunk = true;
            m_current_chunk_stream = TRY(try_make<ConstrainedStream>(MaybeOwned { *m_stream }, data_size));
        }

        if (3 <= control_byte && control_byte <= 0x7f) {
            // " - 3-0x7f are invalid values"
            return Error::from_string_literal("Invalid control byte in LZMA2 stream");
        }

        if (0x80 <= control_byte) {
            // " - 0x80-0xff denotes an LZMA chunk, where the lowest 5 bits are used as bit 16-20
            //     of the uncompressed size minus one, and bit 5-6 indicates what should be reset."
            auto encoded_uncompressed_size_high = control_byte & 0b11111;
            auto reset_indicator = (control_byte & 0b1100000) >> 5;

            // "LZMA chunks consist of:
            //   - A 16-bit big-endian value encoding the low 16-bits of the uncompressed size minus one
            //   - A 16-bit big-endian value encoding the compressed size minus one
            //   - A properties/lclppb byte if bit 6 in the control byte is set
            //   - The LZMA compressed data, starting with the 5 bytes (of which the first is ignored)
            //     used to initialize the range coder (which are included in the compressed size)"
            u16 encoded_uncompressed_size_low = TRY(m_stream->read_value<BigEndian<u16>>());
            u16 encoded_compressed_size = TRY(m_stream->read_value<BigEndian<u16>>());

            u64 uncompressed_size = ((encoded_uncompressed_size_high << 16) | encoded_uncompressed_size_low) + 1;
            u32 compressed_size = encoded_compressed_size + 1;

            m_current_chunk_stream = TRY(try_make<ConstrainedStream>(MaybeOwned { *m_stream }, compressed_size));

            // "Bits 5-6 for LZMA chunks can be:"
            switch (reset_indicator) {
            case 3: {
                // " - 3: state reset, properties reset using properties byte, dictionary reset"
                m_dictionary.clear();
                m_dictionary_initialized = true;
                [[fallthrough]];
            }
            case 2: {
                // " - 2: state reset, properties reset using properties byte"

                // Update the stored LZMA options with the new settings, the stream will be recreated later.
                auto encoded_properties = TRY(m_stream->read_value<u8>());
                auto properties = TRY(LzmaHeader::decode_model_properties(encoded_properties));
                auto dictionary_size = m_dictionary.capacity();
                VERIFY(dictionary_size <= NumericLimits<u32>::max());
                m_last_lzma_options = LzmaDecompressorOptions {
                    .literal_context_bits = properties.literal_context_bits,
                    .literal_position_bits = properties.literal_position_bits,
                    .position_bits = properties.position_bits,
                    .dictionary_size = static_cast<u32>(dictionary_size),
                    .uncompressed_size = uncompressed_size,

                    // Note: This is not specified anywhere. However, it is apparently tested by bad-1-lzma2-7.xz from the XZ utils test files.
                    .reject_end_of_stream_marker = true,
                };
                [[fallthrough]];
            }
            case 1: {
                // " - 1: state reset"
                if (!m_last_lzma_options.has_value())
                    return Error::from_string_literal("LZMA2 stream contains LZMA chunk without settings");

                if (!m_dictionary_initialized)
                    return Error::from_string_literal("LZMA2 stream uses dictionary without ever resetting it");

                m_last_lzma_options->uncompressed_size = uncompressed_size;
                m_last_lzma_stream = TRY(LzmaDecompressor::create_from_raw_stream(m_current_chunk_stream.release_value(), *m_last_lzma_options, MaybeOwned<CircularBuffer> { m_dictionary }));

                break;
            }
            case 0: {
                // " - 0: nothing reset"
                if (!m_last_lzma_stream.has_value())
                    return Error::from_string_literal("LZMA2 stream contains no-reset LZMA chunk without previous state");

                if (!m_dictionary_initialized)
                    return Error::from_string_literal("LZMA2 stream uses dictionary without ever resetting it");

                TRY((*m_last_lzma_stream)->append_input_stream(m_current_chunk_stream.release_value(), uncompressed_size));
                break;
            }
            }

            m_in_uncompressed_chunk = false;
            m_current_chunk_stream = MaybeOwned<Stream> { **m_last_lzma_stream };
        }
    }

    auto result = TRY((*m_current_chunk_stream)->read_some(bytes));

    // For an uncompressed block we are reading directly from the input stream,
    // so we need to capture the 'uncompressed' data into the dictionary manually.
    // Since we only care about having the correct value in the seekback buffer,
    // we can also immediately discard the written data and only ever have to write
    // the last <dictionary size> bytes into it.
    if (m_in_uncompressed_chunk) {
        VERIFY(m_dictionary.used_space() == 0);

        auto relevant_data = result;
        if (relevant_data.size() > m_dictionary.capacity())
            relevant_data = relevant_data.slice(relevant_data.size() - m_dictionary.capacity(), relevant_data.size());

        auto written_bytes = m_dictionary.write(relevant_data);
        VERIFY(written_bytes == relevant_data.size());

        MUST(m_dictionary.discard(written_bytes));
    }

    return result;
}

ErrorOr<size_t> Lzma2Decompressor::write_some(ReadonlyBytes)
{
    return Error::from_errno(EBADF);
}

bool Lzma2Decompressor::is_eof() const
{
    return m_found_end_of_stream;
}

bool Lzma2Decompressor::is_open() const
{
    return true;
}

void Lzma2Decompressor::close()
{
}

}
