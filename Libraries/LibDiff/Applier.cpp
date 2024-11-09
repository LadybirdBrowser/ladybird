/*
 * Copyright (c) 2023-2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Stream.h>
#include <LibDiff/Applier.h>
#include <LibDiff/Hunks.h>

namespace Diff {

static size_t expected_line_number(HunkLocation const& location)
{
    auto line = location.old_range.start_line;

    // NOTE: This is to handle the case we are adding a file, e.g for a range such as:
    // '@@ -0,0 +1,3 @@'
    if (location.old_range.start_line == 0)
        ++line;

    VERIFY(line != 0);

    return line;
}

struct Location {
    size_t line_number;
    size_t fuzz { 0 };
    ssize_t offset { 0 };
};

static Optional<Location> locate_hunk(Vector<StringView> const& content, Hunk const& hunk, ssize_t offset, size_t max_fuzz = 3)
{
    // Make a first best guess at where the from-file range is telling us where the hunk should be.
    size_t offset_guess = expected_line_number(hunk.location) - 1 + offset;

    // If there's no lines surrounding this hunk - it will always succeed,
    // so there is no point in checking any further. Note that this check is
    // also what makes matching against an empty 'from file' work (with no lines),
    // as in that case there is no content for us to even match against in the
    // first place!
    //
    // However, we also should reject patches being added when the hunk is
    // claiming the file is completely empty - but there are actually lines in
    // that file.
    if (hunk.location.old_range.number_of_lines == 0) {
        if (hunk.location.old_range.start_line == 0 && !content.is_empty())
            return {};
        return Location { offset_guess, 0, 0 };
    }

    size_t patch_prefix_context = 0;
    for (auto const& line : hunk.lines) {
        if (line.operation != Line::Operation::Context)
            break;
        ++patch_prefix_context;
    }

    size_t patch_suffix_context = 0;
    for (auto const& line : hunk.lines.in_reverse()) {
        if (line.operation != Line::Operation::Context)
            break;
        ++patch_suffix_context;
    }

    size_t context = max(patch_prefix_context, patch_suffix_context);

    // Look through the file trying to match the hunk for it. If we can't find anything anywhere in the file, then try and
    // match the hunk by ignoring an increasing amount of context lines. The number of context lines that are ignored is
    // called the 'fuzz'.
    for (size_t fuzz = 0; fuzz <= max_fuzz; ++fuzz) {
        auto suffix_fuzz = (patch_suffix_context >= context) ? (fuzz + patch_suffix_context - context) : 0;
        auto prefix_fuzz = (patch_prefix_context >= context) ? (fuzz + patch_prefix_context - context) : 0;

        // If the fuzz is greater than the total number of lines for a hunk, then it may be possible for the hunk to match anything.
        if (suffix_fuzz + prefix_fuzz >= hunk.lines.size())
            return {};

        auto hunk_matches_starting_from_line = [&](size_t line) {
            line += prefix_fuzz;

            // Ensure that all of the lines in the hunk match starting from 'line', ignoring the specified number of context lines.
            return all_of(hunk.lines.begin() + prefix_fuzz, hunk.lines.end() - suffix_fuzz, [&](Line const& hunk_line) {
                // Ignore additions in our increment of line and comparison as they are not part of the 'original file'
                if (hunk_line.operation == Line::Operation::Addition)
                    return true;

                if (line >= content.size())
                    return false;

                if (content[line] != hunk_line.content)
                    return false;

                ++line;
                return true;
            });
        };

        for (size_t line = offset_guess; line < content.size(); ++line) {
            if (hunk_matches_starting_from_line(line))
                return Location { line, fuzz, static_cast<ssize_t>(line - offset_guess) };
        }

        for (size_t line = offset_guess; line != 0; --line) {
            if (hunk_matches_starting_from_line(line - 1))
                return Location { line - 1, fuzz, static_cast<ssize_t>(line - offset_guess) };
        }
    }

    // No bueno.
    return {};
}

static ErrorOr<size_t> write_hunk(Stream& out, Hunk const& hunk, Location const& location, Vector<StringView> const& lines)
{
    auto line_number = location.line_number;

    for (auto const& patch_line : hunk.lines) {
        if (patch_line.operation == Line::Operation::Context) {
            TRY(out.write_formatted("{}\n", lines.at(line_number)));
            ++line_number;
        } else if (patch_line.operation == Line::Operation::Addition) {
            TRY(out.write_formatted("{}\n", patch_line.content));
        } else if (patch_line.operation == Line::Operation::Removal) {
            ++line_number;
        }
    }

    return line_number;
}

static ErrorOr<size_t> write_define_hunk(Stream& out, Hunk const& hunk, Location const& location, Vector<StringView> const& lines, StringView define)
{
    enum class State {
        Outside,
        InsideIFNDEF,
        InsideIFDEF,
        InsideELSE,
    };

    auto state = State::Outside;

    auto line_number = location.line_number;

    for (auto const& patch_line : hunk.lines) {
        if (patch_line.operation == Diff::Line::Operation::Context) {
            auto const& line = lines.at(line_number);
            ++line_number;
            if (state != State::Outside) {
                TRY(out.write_formatted("#endif\n"));
                state = State::Outside;
            }
            TRY(out.write_formatted("{}\n", line));
        } else if (patch_line.operation == Diff::Line::Operation::Addition) {
            if (state == State::Outside) {
                state = State::InsideIFDEF;
                TRY(out.write_formatted("#ifdef {}\n", define));
            } else if (state == State::InsideIFNDEF) {
                state = State::InsideELSE;
                TRY(out.write_formatted("#else\n"));
            }
            TRY(out.write_formatted("{}\n", patch_line.content));
        } else if (patch_line.operation == Diff::Line::Operation::Removal) {
            auto const& line = lines.at(line_number);
            ++line_number;

            if (state == State::Outside) {
                state = State::InsideIFNDEF;
                TRY(out.write_formatted("#ifndef {}\n", define));
            } else if (state == State::InsideIFDEF) {
                state = State::InsideELSE;
                TRY(out.write_formatted("#else\n"));
            }
            TRY(out.write_formatted("{}\n", line));
        }
    }

    if (state != State::Outside)
        TRY(out.write_formatted("#endif\n"));

    return line_number;
}

ErrorOr<void> apply_patch(Stream& out, Vector<StringView> const& lines, Patch const& patch, Optional<StringView> const& define)
{
    size_t line_number = 0; // NOTE: relative to 'old' file.
    ssize_t offset_error = 0;

    for (size_t hunk_num = 0; hunk_num < patch.hunks.size(); ++hunk_num) {
        auto const& hunk = patch.hunks[hunk_num];

        auto maybe_location = locate_hunk(lines, hunk, offset_error);
        if (!maybe_location.has_value())
            return Error::from_string_literal("Failed to locate where to apply patch");

        auto location = *maybe_location;
        offset_error += location.offset;

        // Write up until where we have found this latest hunk from the old file.
        for (; line_number < location.line_number; ++line_number)
            TRY(out.write_formatted("{}\n", lines.at(line_number)));

        // Then output the hunk to what we hope is the correct location in the file.
        if (define.has_value())
            line_number = TRY(write_define_hunk(out, hunk, location, lines, define.value()));
        else
            line_number = TRY(write_hunk(out, hunk, location, lines));
    }

    // We've finished applying all hunks, write out anything from the old file we haven't already.
    for (; line_number < lines.size(); ++line_number)
        TRY(out.write_formatted("{}\n", lines[line_number]));

    return {};
}

}
