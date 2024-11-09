/*
 * Copyright (c) 2023, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Details.h"
#include <AK/TypeCasts.h>
#include <LibRIFF/IFF.h>
#include <LibRIFF/RIFF.h>

namespace RIFF {

StringView ChunkID::as_ascii_string() const
{
    return StringView { id_data.span() };
}

bool ChunkID::operator==(StringView other_string) const
{
    return as_ascii_string() == other_string;
}

namespace Detail {

template<typename WordType>
auto ChunkHeader<WordType>::read_from_stream(Stream& stream) -> ErrorOr<ChunkHeader>
{
    auto id = TRY(stream.read_value<RIFF::ChunkID>());
    u32 size = TRY(stream.read_value<WordType>());
    return ChunkHeader { id, size };
}

template<typename HeaderType>
auto FileHeader<HeaderType>::read_from_stream(Stream& stream) -> ErrorOr<FileHeader>
{
    auto header = TRY(stream.read_value<HeaderType>());
    auto subformat = TRY(stream.read_value<RIFF::ChunkID>());
    return FileHeader { header, subformat };
}

template<typename HeaderType>
Chunk<HeaderType>::Chunk(HeaderType header, ReadonlyBytes data)
    : m_header(header)
    , m_data(data)
{
    VERIFY(data.size() == header.size);
}

template<typename HeaderType>
FixedMemoryStream Chunk<HeaderType>::data_stream() const
{
    return FixedMemoryStream { m_data };
}

template<typename HeaderType>
auto Chunk<HeaderType>::decode(ReadonlyBytes data) -> ErrorOr<Chunk>
{
    auto data_stream = FixedMemoryStream { data };
    auto header = TRY(HeaderType::read_from_stream(data_stream));

    if (data.size() < sizeof(HeaderType) + header.size)
        return Error::from_string_literal("Not enough data for IFF/RIFF chunk");

    return Chunk { header, data.slice(sizeof(HeaderType), header.size) };
}

template<typename HeaderType>
auto Chunk<HeaderType>::decode_and_advance(ReadonlyBytes& data) -> ErrorOr<Chunk>
{
    auto chunk = TRY(decode(data));
    data = data.slice(sizeof(HeaderType) + chunk.size());
    // add padding if needed
    if (chunk.size() % 2 != 0) {
        if (data.is_empty())
            return Error::from_string_literal("Missing data for padding byte");
        if (*data.data() != 0)
            return Error::from_string_literal("Padding byte is not 0");
        data = data.slice(1);
    }

    return chunk;
}

template<typename HeaderType>
OwnedChunk<HeaderType>::OwnedChunk(HeaderType header, Buffer backing_data)
    : Chunk<HeaderType>(header, backing_data.span())
    , m_backing_data(move(backing_data))
{
}

template<typename HeaderType>
auto OwnedChunk<HeaderType>::read_from_stream(Stream& stream) -> ErrorOr<OwnedChunk>
{
    auto header = TRY(stream.read_value<HeaderType>());

    auto data = TRY(Buffer::create_uninitialized(header.size));
    TRY(stream.read_until_filled(data.span()));

    // RIFF chunks may have trailing padding to align to x86 "words" (i.e. 2 bytes).
    if (is<SeekableStream>(stream)) {
        if (!stream.is_eof()) {
            auto stream_position = TRY(static_cast<SeekableStream&>(stream).tell());
            if (stream_position % 2 != 0)
                TRY(static_cast<SeekableStream&>(stream).seek(1, SeekMode::FromCurrentPosition));
        }
    } else {
        dbgln("RIFF Warning: Cannot align stream to 2-byte boundary, next chunk may be bogus!");
    }

    return OwnedChunk { header, data };
}

template class Chunk<IFF::ChunkHeader>;
template class Chunk<RIFF::ChunkHeader>;
template class OwnedChunk<IFF::ChunkHeader>;
template class OwnedChunk<RIFF::ChunkHeader>;
template struct ChunkHeader<IFF::WordType>;
template struct ChunkHeader<RIFF::WordType>;
template struct FileHeader<IFF::ChunkHeader>;
template struct FileHeader<RIFF::ChunkHeader>;

}

}
