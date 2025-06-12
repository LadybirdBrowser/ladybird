/*
 * Copyright (c) 2018-2023, the SerenityOS developers.
 * Copyright (c) 2023, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Vector.h>
#include <LibRIFF/ChunkID.h>
#include <LibRIFF/Details.h>

// RIFF chunks (as often used by Microsoft's older formats) use little-endian fields.
namespace RIFF {

static constexpr StringView const riff_magic = "RIFF"sv;
static constexpr StringView const list_chunk_id = "LIST"sv;

using WordType = LittleEndian<u32>;
using ChunkHeader = RIFF::Detail::ChunkHeader<WordType>;
using FileHeader = RIFF::Detail::FileHeader<ChunkHeader>;
using Chunk = RIFF::Detail::Chunk<ChunkHeader>;
using OwnedChunk = RIFF::Detail::OwnedChunk<ChunkHeader>;

// http://www-mmsp.ece.mcgill.ca/Documents/AudioFormats/WAVE/Docs/riffmci.pdf page 23 (LIST type)
struct OwnedList {
    static ErrorOr<OwnedList> read_from_stream(Stream& stream);

    ChunkID type;
    Vector<OwnedChunk> chunks;
};

}
