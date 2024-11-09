/*
 * Copyright (c) 2020, Itamar S. <itamar8910@gmail.com>
 * Copyright (c) 2021, Mustafa Quraish <mustafa@serenityos.org>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Format.h"
#include <AK/Assertions.h>
#include <AK/ByteString.h>
#include <AK/Stream.h>
#include <AK/StringBuilder.h>
#include <AK/Vector.h>
#include <LibDiff/Hunks.h>

namespace Diff {
ByteString generate_only_additions(StringView text)
{
    auto lines = text.split_view('\n', SplitBehavior::KeepEmpty);
    StringBuilder builder;
    builder.appendff("@@ -0,0 +1,{} @@\n", lines.size());
    for (auto const& line : lines) {
        builder.appendff("+{}\n", line);
    }
    return builder.to_byte_string();
}

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

ErrorOr<void> write_normal(Hunk const& hunk, Stream& stream, ColorOutput color_output)
{
    // Source line(s)
    TRY(stream.write_formatted("{}", hunk.location.old_range.start_line));
    if (hunk.location.old_range.number_of_lines > 1)
        TRY(stream.write_formatted(",{}", (hunk.location.old_range.start_line + hunk.location.old_range.number_of_lines - 1)));

    // Action
    if (hunk.location.old_range.number_of_lines > 0 && hunk.location.new_range.number_of_lines > 0)
        TRY(stream.write_formatted("c"));
    else if (hunk.location.new_range.number_of_lines > 0)
        TRY(stream.write_formatted("a"));
    else
        TRY(stream.write_formatted("d"));

    // Target line(s)
    TRY(stream.write_formatted("{}", hunk.location.new_range.start_line));
    if (hunk.location.new_range.number_of_lines > 1)
        TRY(stream.write_formatted(",{}", (hunk.location.new_range.start_line + hunk.location.new_range.number_of_lines - 1)));

    TRY(stream.write_formatted("\n"));

    for (auto const& line : hunk.lines) {
        VERIFY(line.operation == Line::Operation::Removal || line.operation == Line::Operation::Addition);

        if (line.operation == Line::Operation::Addition) {
            if (color_output == ColorOutput::Yes)
                TRY(stream.write_formatted("\033[32;1m> {}\033[0m\n", line.content));
            else
                TRY(stream.write_formatted("> {}\n", line.content));
        } else {
            if (color_output == ColorOutput::Yes)
                TRY(stream.write_formatted("\033[31;1m< {}\033[0m\n", line.content));
            else
                TRY(stream.write_formatted("< {}\n", line.content));
        }
    }

    return {};
}

struct SplitLines {
    Vector<Line> old_lines;
    Vector<Line> new_lines;
};

static ErrorOr<SplitLines> split_hunk_into_old_and_new_lines(Hunk const& hunk)
{
    size_t new_lines_last_context = 0;
    size_t old_lines_last_context = 0;
    SplitLines lines;

    auto operation = Line::Operation::Context;

    bool is_all_insertions = true;
    bool is_all_deletions = true;

    auto check_if_line_is_a_change = [&](Line::Operation op) {
        if (operation != op) {
            // We've switched from additions to removals or vice-versa.
            // All lines starting from the last context line we saw must be changes.
            operation = Line::Operation::Change;
            for (size_t i = new_lines_last_context; i < lines.new_lines.size(); ++i)
                lines.new_lines[i].operation = Line::Operation::Change;
            for (size_t i = old_lines_last_context; i < lines.old_lines.size(); ++i)
                lines.old_lines[i].operation = Line::Operation::Change;
        }
    };

    for (auto const& line : hunk.lines) {
        switch (line.operation) {
        case Line::Operation::Context:
            VERIFY(lines.old_lines.size() < hunk.location.old_range.number_of_lines);
            VERIFY(lines.new_lines.size() < hunk.location.new_range.number_of_lines);

            operation = Line::Operation::Context;
            TRY(lines.new_lines.try_append(Line { operation, line.content }));
            TRY(lines.old_lines.try_append(Line { operation, line.content }));
            new_lines_last_context = lines.new_lines.size();
            old_lines_last_context = lines.old_lines.size();
            break;
        case Line::Operation::Addition:
            VERIFY(lines.new_lines.size() < hunk.location.new_range.number_of_lines);

            if (operation != Line::Operation::Context)
                check_if_line_is_a_change(Line::Operation::Addition);
            else
                operation = Line::Operation::Addition;

            TRY(lines.new_lines.try_append(Line { operation, line.content }));
            is_all_deletions = false;
            break;
        case Line::Operation::Removal:
            VERIFY(lines.old_lines.size() < hunk.location.old_range.number_of_lines);

            if (operation != Line::Operation::Context)
                check_if_line_is_a_change(Line::Operation::Removal);
            else
                operation = Line::Operation::Removal;

            TRY(lines.old_lines.try_append(Line { operation, line.content }));
            is_all_insertions = false;
            break;
        default:
            VERIFY_NOT_REACHED();
        }
    }

    VERIFY(lines.new_lines.size() == hunk.location.new_range.number_of_lines && lines.old_lines.size() == hunk.location.old_range.number_of_lines);

    if (is_all_insertions)
        lines.old_lines.clear();
    else if (is_all_deletions)
        lines.new_lines.clear();

    return lines;
}

static ErrorOr<void> write_hunk_as_context(Vector<Line> const& old_lines, Vector<Line> const& new_lines, HunkLocation const& location, Stream& stream, ColorOutput color_output)
{
    TRY(stream.write_formatted("*** {}", location.old_range.start_line));

    if (location.old_range.number_of_lines > 1)
        TRY(stream.write_formatted(",{}", location.old_range.start_line + location.old_range.number_of_lines - 1));

    TRY(stream.write_formatted(" ****\n"));

    for (auto const& line : old_lines) {
        if (color_output == ColorOutput::Yes && (line.operation == Line::Operation::Removal || line.operation == Line::Operation::Change))
            TRY(stream.write_formatted("\033[31;1m{} {}\033[0m\n", line.operation, line.content));
        else
            TRY(stream.write_formatted("{} {}\n", line.operation, line.content));
    }

    TRY(stream.write_formatted("--- {}", location.new_range.start_line));
    if (location.new_range.number_of_lines > 1)
        TRY(stream.write_formatted(",{}", location.new_range.start_line + location.new_range.number_of_lines - 1));

    TRY(stream.write_formatted(" ----\n"));

    for (auto const& line : new_lines) {
        if (color_output == ColorOutput::Yes && (line.operation == Line::Operation::Addition || line.operation == Line::Operation::Change))
            TRY(stream.write_formatted("\033[32;1m{} {}\033[0m\n", line.operation, line.content));
        else
            TRY(stream.write_formatted("{} {}\n", line.operation, line.content));
    }

    return {};
}

ErrorOr<void> write_context(Hunk const& hunk, Stream& stream, ColorOutput color_output)
{
    auto const split_lines = TRY(split_hunk_into_old_and_new_lines(hunk));
    return write_hunk_as_context(split_lines.old_lines, split_lines.new_lines, hunk.location, stream, color_output);
}

ErrorOr<void> write_context_header(StringView old_path, StringView new_path, Stream& stream)
{
    TRY(stream.write_formatted("*** {}\n", old_path));
    TRY(stream.write_formatted("--- {}\n", new_path));

    return stream.write_formatted("***************\n");
}

}
