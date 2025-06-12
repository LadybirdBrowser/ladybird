/*
 * Copyright (c) 2021, Mustafa Quraish <mustafa@serenityos.org>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Generator.h"

namespace Diff {

ErrorOr<Vector<Hunk>> from_text(StringView old_text, StringView new_text, size_t context)
{
    auto old_lines = old_text.lines();
    auto new_lines = new_text.lines();

    /**
     * This is a simple implementation of the Longest Common Subsequence algorithm (over
     * the lines of the text as opposed to the characters). A Dynamic programming approach
     * is used here.
     */

    enum class Direction {
        Down,     // Added a new line
        Right,    // Removed a line
        Diagonal, // Line remained the same
    };

    // A single cell in the DP-matrix. Cell (i, j) represents the longest common
    // sub-sequence of lines between old_lines[0 : i] and new_lines[0 : j].
    struct Cell {
        size_t length;
        Direction direction;
    };

    auto dp_matrix = Vector<Cell>();
    TRY(dp_matrix.try_resize((old_lines.size() + 1) * (new_lines.size() + 1)));

    auto dp = [&dp_matrix, width = old_lines.size() + 1](size_t i, size_t j) -> Cell& {
        return dp_matrix[i + width * j];
    };

    // Initialize the first row and column
    for (size_t i = 0; i <= old_lines.size(); ++i)
        dp(i, new_lines.size()) = { 0, Direction::Right };

    for (size_t j = 0; j <= new_lines.size(); ++j)
        dp(old_lines.size(), 0) = { 0, Direction::Down };

    // Fill in the rest of the DP table
    for (int i = old_lines.size() - 1; i >= 0; --i) {
        for (int j = new_lines.size() - 1; j >= 0; --j) {
            if (old_lines[i] == new_lines[j]) {
                dp(i, j) = { dp(i + 1, j + 1).length + 1, Direction::Diagonal };
            } else {
                auto down = dp(i, j + 1).length;
                auto right = dp(i + 1, j).length;
                if (down > right)
                    dp(i, j) = { down, Direction::Down };
                else
                    dp(i, j) = { right, Direction::Right };
            }
        }
    }

    Vector<Hunk> hunks;
    Hunk cur_hunk;

    auto flush_hunk = [&]() -> ErrorOr<void> {
        // A file with no content has a zero indexed start line.
        if (cur_hunk.location.new_range.start_line != 0 || cur_hunk.location.new_range.number_of_lines != 0)
            cur_hunk.location.new_range.start_line++;
        if (cur_hunk.location.old_range.start_line != 0 || cur_hunk.location.old_range.number_of_lines != 0)
            cur_hunk.location.old_range.start_line++;

        TRY(hunks.try_append(cur_hunk));
        cur_hunk.lines.clear();

        return {};
    };

    size_t i = 0;
    size_t j = 0;

    auto set_up_hunk_prepended_with_context = [&](Hunk& hunk) -> ErrorOr<void> {
        // Prefix the hunk with requested number context lines, and set the hunk location to where that context begins.
        size_t available_context = min(i, context);
        hunk.location.old_range = { i - available_context, available_context };
        hunk.location.new_range = { j - available_context, available_context };

        for (size_t offset = 0; offset < available_context; ++offset) {
            size_t context_line = i + offset - available_context;
            TRY(hunk.lines.try_append(Line { Line::Operation::Context, TRY(String::from_utf8(old_lines[context_line])) }));
        }

        return {};
    };

    size_t current_context = 0;
    while (i < old_lines.size() && j < new_lines.size()) {

        auto& cell = dp(i, j);
        if (cell.direction == Direction::Down) {
            if (cur_hunk.lines.is_empty())
                TRY(set_up_hunk_prepended_with_context(cur_hunk));
            TRY(cur_hunk.lines.try_append(Line { Line::Operation::Addition, TRY(String::from_utf8(new_lines[j])) }));
            cur_hunk.location.new_range.number_of_lines++;

            ++j;
            current_context = 0;
        } else if (cell.direction == Direction::Right) {
            if (cur_hunk.lines.is_empty())
                TRY(set_up_hunk_prepended_with_context(cur_hunk));
            TRY(cur_hunk.lines.try_append(Line { Line::Operation::Removal, TRY(String::from_utf8(old_lines[i])) }));
            cur_hunk.location.old_range.number_of_lines++;

            ++i;
            current_context = 0;
        } else {
            if (!cur_hunk.lines.is_empty()) {
                // We're currently in the middle of generating a hunk and have found a context line. If we have already added
                // the number of context lines that were requested then we have already finished with this hunk. Otherwise we
                // need to continue looking through the hunk until we have located the requested number of context lines in a
                // row.
                if (current_context == context) {
                    TRY(flush_hunk());
                } else {
                    TRY(cur_hunk.lines.try_append(Line { Line::Operation::Context, TRY(String::from_utf8(old_lines[i])) }));
                    cur_hunk.location.new_range.number_of_lines++;
                    cur_hunk.location.old_range.number_of_lines++;
                }

                ++current_context;
            }

            ++i;
            ++j;
        }
    }

    while (i < old_lines.size()) {
        if (cur_hunk.lines.is_empty())
            TRY(set_up_hunk_prepended_with_context(cur_hunk));

        TRY(cur_hunk.lines.try_append(Line { Line::Operation::Removal, TRY(String::from_utf8(old_lines[i])) }));
        cur_hunk.location.old_range.number_of_lines++;
        ++i;
    }

    while (j < new_lines.size()) {
        if (cur_hunk.lines.is_empty())
            TRY(set_up_hunk_prepended_with_context(cur_hunk));
        TRY(cur_hunk.lines.try_append(Line { Line::Operation::Addition, TRY(String::from_utf8(new_lines[j])) }));
        cur_hunk.location.new_range.number_of_lines++;

        ++j;
    }

    if (!cur_hunk.lines.is_empty())
        TRY(flush_hunk());

    return hunks;
}

}
