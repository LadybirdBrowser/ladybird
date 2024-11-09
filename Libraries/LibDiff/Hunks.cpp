/*
 * Copyright (c) 2020, Itamar S. <itamar8910@gmail.com>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Hunks.h"
#include <AK/Debug.h>
#include <AK/LexicalPath.h>

namespace Diff {

Optional<HunkLocation> Parser::consume_unified_location()
{
    auto consume_range = [this](Range& range) {
        if (!consume_line_number(range.start_line))
            return false;

        if (consume_specific(',')) {
            if (!consume_line_number(range.number_of_lines))
                return false;
        } else {
            range.number_of_lines = 1;
        }
        return true;
    };

    if (!consume_specific("@@ -"sv))
        return {};

    HunkLocation location;

    if (!consume_range(location.old_range))
        return {};

    if (!consume_specific(" +"sv))
        return {};

    if (!consume_range(location.new_range))
        return {};

    if (!consume_specific(" @@"sv))
        return {};

    return location;
}

bool Parser::consume_line_number(size_t& number)
{
    auto line = consume_while(is_ascii_digit);

    auto maybe_number = line.to_number<size_t>();
    if (!maybe_number.has_value())
        return false;

    number = maybe_number.value();
    return true;
}

ErrorOr<String> Parser::parse_file_line(Optional<size_t> const& strip_count)
{
    // FIXME: handle parsing timestamps as well.
    auto line = consume_line();

    GenericLexer line_parser(line);
    auto path = line_parser.consume_until('\t');

    // No strip count given. Default to basename of file.
    if (!strip_count.has_value())
        return String::from_byte_string(LexicalPath::basename(path));

    // NOTE: We cannot use LexicalPath::parts as we want to strip the non-canonicalized path.
    auto const& parts = path.split_view('/');

    // More components to strip than the filename has. Just pretend it is missing.
    if (strip_count.value() >= parts.size())
        return String();

    // Remove given number of leading components from the path.
    size_t components = parts.size() - strip_count.value();

    StringBuilder stripped_path;
    for (size_t i = parts.size() - components; i < parts.size(); ++i) {
        TRY(stripped_path.try_append(parts[i]));
        if (i != parts.size() - 1)
            TRY(stripped_path.try_append("/"sv));
    }

    return stripped_path.to_string();
}

ErrorOr<Patch> Parser::parse_patch(Optional<size_t> const& strip_count)
{
    Patch patch;
    patch.header = TRY(parse_header(strip_count));
    patch.hunks = TRY(parse_hunks());
    return patch;
}

ErrorOr<Header> Parser::parse_header(Optional<size_t> const& strip_count)
{
    Header header;

    while (!is_eof()) {

        if (consume_specific("+++ "sv)) {
            header.new_file_path = TRY(parse_file_line(strip_count));
            continue;
        }

        if (consume_specific("--- "sv)) {
            header.old_file_path = TRY(parse_file_line(strip_count));
            continue;
        }

        if (next_is("@@ ")) {
            header.format = Format::Unified;
            return header;
        }

        consume_line();
    }

    return header;
}

ErrorOr<Vector<Hunk>> Parser::parse_hunks()
{
    Vector<Hunk> hunks;

    while (next_is("@@ ")) {
        // Try an locate a hunk location in this hunk. It may be prefixed with information.
        auto maybe_location = consume_unified_location();
        consume_line();

        if (!maybe_location.has_value())
            break;

        Hunk hunk { *maybe_location, {} };

        auto old_lines_expected = hunk.location.old_range.number_of_lines;
        auto new_lines_expected = hunk.location.new_range.number_of_lines;

        // We've found a location. Now parse out all of the expected content lines.
        while (old_lines_expected != 0 || new_lines_expected != 0) {
            StringView line = consume_line();

            if (line.is_empty())
                return Error::from_string_literal("Malformed empty content line in patch");

            if (line[0] != ' ' && line[0] != '+' && line[0] != '-')
                return Error::from_string_literal("Invaid operation in patch");

            auto const operation = Line::operation_from_symbol(line[0]);

            if (operation != Line::Operation::Removal) {
                if (new_lines_expected == 0)
                    return Error::from_string_literal("Found more removal and context lines in patch than expected");

                --new_lines_expected;
            }

            if (operation != Line::Operation::Addition) {
                if (old_lines_expected == 0)
                    return Error::from_string_literal("Found more addition and context lines in patch than expected");

                --old_lines_expected;
            }

            auto const content = line.substring_view(1, line.length() - 1);
            TRY(hunk.lines.try_append(Line { operation, TRY(String::from_utf8(content)) }));
        }

        TRY(hunks.try_append(hunk));
    }

    if constexpr (HUNKS_DEBUG) {
        for (auto const& hunk : hunks) {
            dbgln("{}", hunk.location);
            for (auto const& line : hunk.lines)
                dbgln("{}", line);
        }
    }

    return hunks;
}

ErrorOr<Vector<Hunk>> parse_hunks(StringView diff)
{
    Parser lexer(diff);
    while (!lexer.next_is("@@ ") && !lexer.is_eof())
        lexer.consume_line();
    return lexer.parse_hunks();
}
}
