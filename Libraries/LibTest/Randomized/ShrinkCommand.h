/*
 * Copyright (c) 2023, Martin Janiczek <martin@janiczek.cz>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibTest/Randomized/Chunk.h>
#include <LibTest/Randomized/RandomRun.h>

#include <AK/String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>

namespace Test {
namespace Randomized {

struct ZeroChunk {
    Chunk chunk;
};
struct SortChunk {
    Chunk chunk;
};
struct DeleteChunkAndMaybeDecPrevious {
    Chunk chunk;
};
struct MinimizeChoice {
    size_t index;
};
struct SwapChunkWithNeighbour {
    Chunk chunk;

    Chunk const neighbour()
    {
        return Chunk { chunk.size, chunk.index + chunk.size };
    }
};
struct RedistributeChoicesAndMaybeInc {
    size_t left_index;
    size_t right_index;
};

using CommandVariant = Variant<
    ZeroChunk,
    SortChunk,
    DeleteChunkAndMaybeDecPrevious,
    MinimizeChoice,
    SwapChunkWithNeighbour,
    RedistributeChoicesAndMaybeInc>;

enum class AllowSizeOneChunks {
    Yes,
    No,
};

class ShrinkCommand {
public:
    explicit ShrinkCommand(CommandVariant const& command)
        : m_command(command)
    {
    }

    static Vector<ShrinkCommand> for_run(RandomRun const& run)
    {
        size_t run_size = run.size();

        Vector<ShrinkCommand> all;
        // Sorted roughly in the order of effectiveness. Deleting chunks is better
        // than minimizing them.
        all.extend(deletion_commands(run_size));
        all.extend(zero_commands(run_size));
        all.extend(sort_commands(run_size));
        all.extend(swap_chunk_commands(run_size));
        all.extend(minimize_commands(run_size));
        all.extend(redistribute_commands(run_size));
        return all;
    }

    bool has_a_chance(RandomRun const& run) const
    {
        return m_command.visit(
            [&](ZeroChunk c) { return run.contains_chunk(c.chunk); },
            [&](SortChunk c) { return run.contains_chunk(c.chunk); },
            [&](DeleteChunkAndMaybeDecPrevious c) { return run.contains_chunk(c.chunk); },
            [&](MinimizeChoice c) { return run.size() > c.index; },
            [&](RedistributeChoicesAndMaybeInc c) { return run.size() > c.right_index; },
            [&](SwapChunkWithNeighbour c) { return run.contains_chunk(c.neighbour()); });
    }

    ErrorOr<String> to_string()
    {
        return m_command.visit(
            [](ZeroChunk c) { return String::formatted("ZeroChunk({})", c.chunk); },
            [](SortChunk c) { return String::formatted("SortChunk({})", c.chunk); },
            [](DeleteChunkAndMaybeDecPrevious c) { return String::formatted("DeleteChunkAndMaybeDecPrevious({})", c.chunk); },
            [](MinimizeChoice c) { return String::formatted("MinimizeChoice(i={})", c.index); },
            [](RedistributeChoicesAndMaybeInc c) { return String::formatted("RedistributeChoicesAndMaybeInc(left={},right={})", c.left_index, c.right_index); },
            [](SwapChunkWithNeighbour c) { return String::formatted("SwapChunkWithNeighbour({})", c.chunk); });
    }

    template<typename... Fs>
    auto visit(Fs&&... callbacks)
    {
        return m_command.visit(forward<Fs>(callbacks)...);
    }

private:
    CommandVariant m_command;

    // Will generate ShrinkCommands for all chunks of sizes 1,2,3,4,8 in bounds of the
    // given RandomRun size.
    //
    // They will be given in a reverse order (largest chunks first), to maximize our
    // chances of saving work (minimizing the RandomRun faster).
    //
    // chunkCommands(10, false, [](Chunk c){ return SortChunk(c); })
    // -->
    // [ // Chunks of size 8
    //   SortChunk { chunk_size = 8, start_index = 0 }, // [XXXXXXXX..]
    //   SortChunk { chunk_size = 8, start_index = 1 }, // [.XXXXXXXX.]
    //   SortChunk { chunk_size = 8, start_index = 2 }, // [..XXXXXXXX]
    //
    //   // Chunks of size 4
    //   SortChunk { chunk_size = 4, start_index = 0 }, // [XXXX......]
    //   SortChunk { chunk_size = 4, start_index = 1 }, // [.XXXX.....]
    //   // ...
    //   SortChunk { chunk_size = 4, start_index = 5 }, // [.....XXXX.]
    //   SortChunk { chunk_size = 4, start_index = 6 }, // [......XXXX]
    //
    //   // Chunks of size 3
    //   SortChunk { chunk_size = 3, start_index = 0 }, // [XXX.......]
    //   SortChunk { chunk_size = 3, start_index = 1 }, // [.XXX......]
    //   // ...
    //   SortChunk { chunk_size = 3, start_index = 6 }, // [......XXX.]
    //   SortChunk { chunk_size = 3, start_index = 7 }, // [.......XXX]
    //
    //   // Chunks of size 2
    //   SortChunk { chunk_size = 2, start_index = 0 }, // [XX........]
    //   SortChunk { chunk_size = 2, start_index = 1 }, // [.XX.......]
    //   // ...
    //   SortChunk { chunk_size = 2, start_index = 7 }, // [.......XX.]
    //   SortChunk { chunk_size = 2, start_index = 8 }, // [........XX]
    // ]
    template<typename FN>
    static Vector<ShrinkCommand> chunk_commands(size_t run_size, AllowSizeOneChunks allow_chunks_size1, FN chunk_to_command)
    {
        Vector<u8> sizes = { 8, 4, 3, 2 };
        switch (allow_chunks_size1) {
        case AllowSizeOneChunks::Yes:
            sizes.append(1);
            break;
        case AllowSizeOneChunks::No:
            break;
        }

        Vector<ShrinkCommand> commands;
        for (u8 chunk_size : sizes) {
            if (chunk_size > run_size)
                continue;

            for (size_t i = 0; i < run_size - chunk_size + 1; ++i) {
                ShrinkCommand command = chunk_to_command(Chunk { chunk_size, i });
                commands.append(command);
            }
        }
        return commands;
    }

    static Vector<ShrinkCommand> deletion_commands(size_t run_size)
    {
        return chunk_commands(
            run_size,
            AllowSizeOneChunks::Yes,
            [](Chunk c) { return ShrinkCommand(DeleteChunkAndMaybeDecPrevious { c }); });
    }

    static Vector<ShrinkCommand> minimize_commands(size_t run_size)
    {
        Vector<ShrinkCommand> commands;
        for (size_t i = 0; i < run_size; ++i) {
            ShrinkCommand command = ShrinkCommand(MinimizeChoice { i });
            commands.append(command);
        }
        return commands;
    }

    static Vector<ShrinkCommand> redistribute_commands(size_t run_size)
    {
        Vector<ShrinkCommand> commands;
        for (size_t offset = 3; offset > 0; --offset) {
            if (offset >= run_size)
                continue;
            for (size_t i = 0; i < run_size - offset; ++i) {
                ShrinkCommand command = ShrinkCommand(RedistributeChoicesAndMaybeInc { i, i + offset });
                commands.append(command);
            }
        }
        return commands;
    }

    static Vector<ShrinkCommand> sort_commands(size_t run_size)
    {
        return chunk_commands(
            run_size,
            AllowSizeOneChunks::No, // doesn't make sense for sorting
            [](Chunk c) { return ShrinkCommand(SortChunk { c }); });
    }

    static Vector<ShrinkCommand> zero_commands(size_t run_size)
    {
        return chunk_commands(
            run_size,
            AllowSizeOneChunks::No, // already happens in binary search
            [](Chunk c) { return ShrinkCommand(ZeroChunk { c }); });
    }

    static Vector<ShrinkCommand> swap_chunk_commands(size_t run_size)
    {
        return chunk_commands(
            // TODO: This is not optimal as the chunks near the end of
            // the RandomRun will hit OOB.
            //
            // This is because SwapChunkWithNeighbour doesn't just
            // touch the Chunk but also its right neighbour:
            // [_,_,X,X,X,Y,Y,Y,_]
            //
            // If the chunk is too far to the right, it would go OOB:
            // [_,_,_,_,X,X,X,Y,Y]Y
            //
            // For now, this will work though, there will just be a
            // bit of unnecessary work calling .has_a_chance() on
            // these chunks that are too far to the right.
            run_size,
            AllowSizeOneChunks::No, // already happens in "redistribute choice"
            [](Chunk c) { return ShrinkCommand(SwapChunkWithNeighbour { c }); });
    }
};

} // namespace Randomized
} // namespace Test

template<>
struct AK::Formatter<Test::Randomized::ShrinkCommand> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Test::Randomized::ShrinkCommand command)
    {
        return Formatter<StringView>::format(builder, TRY(command.to_string()));
    }
};
