/*
 * Copyright (c) 2023, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/MemoryStream.h>
#include <LibRIFF/ChunkID.h>

// Despite the name, this header contains details for both RIFF and IFF
namespace RIFF::Detail {

// http://www-mmsp.ece.mcgill.ca/Documents/AudioFormats/WAVE/Docs/riffmci.pdf page 11 (Chunks)
template<typename WordType>
struct ChunkHeader {
    static ErrorOr<ChunkHeader> read_from_stream(Stream& stream);

    RIFF::ChunkID id;
    u32 size;
};

// Standard RIFF/IFF file formats use a global chunk with a chunk ID (magic bytes) such as "RIFF" or "FORM".
// A chunk ID right at the start of the global chunk specifies the subformat specific to the file type.
// Example for RIFF from WebP: https://developers.google.com/speed/webp/docs/riff_container#webp_file_header
template<typename HeaderType>
struct FileHeader {
    HeaderType global_header;
    RIFF::ChunkID subformat;

    static ErrorOr<FileHeader> read_from_stream(Stream& stream);

    constexpr ChunkID magic() const { return global_header.id; }
    constexpr u32 file_size() const { return global_header.size; }
};

// An RIFF or IFF chunk.
template<typename HeaderType>
class Chunk {
public:
    Chunk(HeaderType header, ReadonlyBytes data);

    // Note that the resulting chunk will refer to the provided data.
    static ErrorOr<Chunk> decode(ReadonlyBytes data);
    static ErrorOr<Chunk> decode_and_advance(ReadonlyBytes& data);

    RIFF::ChunkID id() const { return m_header.id; }
    u32 size() const { return m_header.size; }
    ReadonlyBytes data() const { return m_data; }
    FixedMemoryStream data_stream() const;

    u8 operator[](size_t index) const { return data()[index]; }

private:
    HeaderType m_header;
    ReadonlyBytes m_data;
};

// Owns the chunk data and can therefore be parsed from a stream.
template<typename HeaderType>
class OwnedChunk : public Chunk<HeaderType> {
public:
    using Buffer = AK::Detail::ByteBuffer<0>;
    OwnedChunk(HeaderType, Buffer);

    static ErrorOr<OwnedChunk> read_from_stream(Stream& stream);

private:
    Buffer m_backing_data;
};

}
