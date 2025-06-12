/*
 * Copyright (c) 2023, Martin Janiczek <martin@janiczek.cz>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Format.h>
#include <AK/String.h>

namespace Test {
namespace Randomized {

// Chunk is a description of a RandomRun slice.
// Used to say which part of a given RandomRun will be shrunk by some
// ShrinkCommand.
//
// For a RandomRun [0,1,2,3,4,5,6,7,8], the Chunk{size=4, index=2} means this:
//                 [_,_,X,X,X,X,_,_,_]
//
// Different ShrinkCommands will use the Chunk in different ways.
// A few examples:
//
//     Original RandomRun:             [5,1,3,9,4,2,3,0]
//     Chunk we'll show off:           [_,_,X,X,X,X,_,_]
//
//     ZeroChunk:                      [5,1,0,0,0,0,3,0]
//     SortChunk:                      [5,1,2,3,4,9,3,0]
//     DeleteChunkAndMaybeDecPrevious: [5,1,        3,0]
struct Chunk {
    // Possible sizes: 1,2,3,4,8
    u8 size = 0;
    size_t index = 0;
};

} // namespace Randomized
} // namespace Test

template<>
struct AK::Formatter<Test::Randomized::Chunk> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Test::Randomized::Chunk chunk)
    {
        return Formatter<StringView>::format(builder, TRY(String::formatted("Chunk<size={}, i={}>", chunk.size, chunk.index)));
    }
};
