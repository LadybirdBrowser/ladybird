/*
 * Copyright (c) 2018-2023, the SerenityOS developers.
 * Copyright (c) 2023, Nico Weber <thakis@chromium.org>
 * Copyright (c) 2023, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibRIFF/ChunkID.h>
#include <LibRIFF/Details.h>

// IFF chunks (as often used by Amiga, EA and more modern formats) use big-endian fields.
namespace IFF {

using WordType = BigEndian<u32>;
using ChunkHeader = RIFF::Detail::ChunkHeader<WordType>;
using FileHeader = RIFF::Detail::FileHeader<ChunkHeader>;
using Chunk = RIFF::Detail::Chunk<ChunkHeader>;
using OwnedChunk = RIFF::Detail::OwnedChunk<ChunkHeader>;

}
