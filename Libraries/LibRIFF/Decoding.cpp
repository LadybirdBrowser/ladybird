/*
 * Copyright (c) 2023, kleines Filmröllchen <filmroellchen@serenityos.org>
 * Copyright (c) 2023, Nicolas Ramz <nicolas.ramz@gmail.com>
 * Copyright (c) 2023, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Stream.h>
#include <LibRIFF/ChunkID.h>
#include <LibRIFF/RIFF.h>

ErrorOr<RIFF::ChunkID> RIFF::ChunkID::read_from_stream(Stream& stream)
{
    Array<u8, chunk_id_size> id;
    TRY(stream.read_until_filled(id.span()));
    return ChunkID { id };
}

ErrorOr<RIFF::OwnedList> RIFF::OwnedList::read_from_stream(Stream& stream)
{
    auto type = TRY(stream.read_value<ChunkID>());
    Vector<RIFF::OwnedChunk> chunks;
    while (!stream.is_eof())
        TRY(chunks.try_append(TRY(stream.read_value<RIFF::OwnedChunk>())));

    return RIFF::OwnedList { .type = type, .chunks = move(chunks) };
}
