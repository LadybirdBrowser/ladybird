/*
 * Copyright (c) 2020, Itamar S. <itamar8910@gmail.com>
 * Copyright (c) 2021, Mustafa Quraish <mustafa@serenityos.org>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Format.h"
#include <AK/Stream.h>
#include <AK/Vector.h>
#include <LibDiff/Hunks.h>

namespace Diff {

ErrorOr<void> write_unified_header(StringView old_path, StringView new_path, Stream& stream)
{
    TRY(stream.write_formatted("--- {}\n", old_path));
    TRY(stream.write_formatted("+++ {}\n", new_path));

    return {};
}

ErrorOr<void> write_unified(Hunk const& hunk, Stream& stream, ColorOutput color_output)
{
    TRY(stream.write_formatted("{}\n", hunk.location));

    if (color_output == ColorOutput::Yes) {
        for (auto const& line : hunk.lines) {
            if (line.operation == Line::Operation::Addition)
                TRY(stream.write_formatted("\033[32;1m{}\033[0m\n", line));
            else if (line.operation == Line::Operation::Removal)
                TRY(stream.write_formatted("\033[31;1m{}\033[0m\n", line));
            else
                TRY(stream.write_formatted("{}\n", line));
        }
    } else {
        for (auto const& line : hunk.lines)
            TRY(stream.write_formatted("{}\n", line));
    }

    return {};
}

}
